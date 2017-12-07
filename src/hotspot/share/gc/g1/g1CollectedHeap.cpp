/*
 * Copyright (c) 2001, 2017, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/metadataOnStackMark.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/symbolTable.hpp"
#include "code/codeCache.hpp"
#include "code/icBuffer.hpp"
#include "gc/g1/bufferingOopClosure.hpp"
#include "gc/g1/concurrentMarkThread.inline.hpp"
#include "gc/g1/g1Allocator.inline.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1CollectionSet.hpp"
#include "gc/g1/g1CollectorPolicy.hpp"
#include "gc/g1/g1CollectorState.hpp"
#include "gc/g1/g1ConcurrentRefine.hpp"
#include "gc/g1/g1ConcurrentRefineThread.hpp"
#include "gc/g1/g1EvacStats.inline.hpp"
#include "gc/g1/g1FullCollector.hpp"
#include "gc/g1/g1FullGCScope.hpp"
#include "gc/g1/g1GCPhaseTimes.hpp"
#include "gc/g1/g1HeapSizingPolicy.hpp"
#include "gc/g1/g1HeapTransition.hpp"
#include "gc/g1/g1HeapVerifier.hpp"
#include "gc/g1/g1HotCardCache.hpp"
#include "gc/g1/g1OopClosures.inline.hpp"
#include "gc/g1/g1ParScanThreadState.inline.hpp"
#include "gc/g1/g1Policy.hpp"
#include "gc/g1/g1RegionToSpaceMapper.hpp"
#include "gc/g1/g1RemSet.hpp"
#include "gc/g1/g1RootClosures.hpp"
#include "gc/g1/g1RootProcessor.hpp"
#include "gc/g1/g1StringDedup.hpp"
#include "gc/g1/g1YCTypes.hpp"
#include "gc/g1/g1YoungRemSetSamplingThread.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#include "gc/g1/heapRegionSet.inline.hpp"
#include "gc/g1/vm_operations_g1.hpp"
#include "gc/shared/gcHeapSummary.hpp"
#include "gc/shared/gcId.hpp"
#include "gc/shared/gcLocker.inline.hpp"
#include "gc/shared/gcTimer.hpp"
#include "gc/shared/gcTrace.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/generationSpec.hpp"
#include "gc/shared/isGCActiveMark.hpp"
#include "gc/shared/preservedMarks.inline.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/shared/referenceProcessor.inline.hpp"
#include "gc/shared/taskqueue.inline.hpp"
#include "gc/shared/weakProcessor.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/iterator.hpp"
#include "memory/resourceArea.hpp"
#include "oops/oop.inline.hpp"
#include "prims/resolvedMethodTable.hpp"
#include "runtime/atomic.hpp"
#include "runtime/init.hpp"
#include "runtime/orderAccess.inline.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/align.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/stack.inline.hpp"

size_t G1CollectedHeap::_humongous_object_threshold_in_words = 0;

// INVARIANTS/NOTES
//
// All allocation activity covered by the G1CollectedHeap interface is
// serialized by acquiring the HeapLock.  This happens in mem_allocate
// and allocate_new_tlab, which are the "entry" points to the
// allocation code from the rest of the JVM.  (Note that this does not
// apply to TLAB allocation, which is not part of this interface: it
// is done by clients of this interface.)

class RedirtyLoggedCardTableEntryClosure : public CardTableEntryClosure {
 private:
  size_t _num_dirtied;
  G1CollectedHeap* _g1h;
  G1SATBCardTableLoggingModRefBS* _g1_bs;

  HeapRegion* region_for_card(jbyte* card_ptr) const {
    return _g1h->heap_region_containing(_g1_bs->addr_for(card_ptr));
  }

  bool will_become_free(HeapRegion* hr) const {
    // A region will be freed by free_collection_set if the region is in the
    // collection set and has not had an evacuation failure.
    return _g1h->is_in_cset(hr) && !hr->evacuation_failed();
  }

 public:
  RedirtyLoggedCardTableEntryClosure(G1CollectedHeap* g1h) : CardTableEntryClosure(),
    _num_dirtied(0), _g1h(g1h), _g1_bs(g1h->g1_barrier_set()) { }

  bool do_card_ptr(jbyte* card_ptr, uint worker_i) {
    HeapRegion* hr = region_for_card(card_ptr);

    // Should only dirty cards in regions that won't be freed.
    if (!will_become_free(hr)) {
      *card_ptr = CardTableModRefBS::dirty_card_val();
      _num_dirtied++;
    }

    return true;
  }

  size_t num_dirtied()   const { return _num_dirtied; }
};


void G1RegionMappingChangedListener::reset_from_card_cache(uint start_idx, size_t num_regions) {
  HeapRegionRemSet::invalidate_from_card_cache(start_idx, num_regions);
}

void G1RegionMappingChangedListener::on_commit(uint start_idx, size_t num_regions, bool zero_filled) {
  // The from card cache is not the memory that is actually committed. So we cannot
  // take advantage of the zero_filled parameter.
  reset_from_card_cache(start_idx, num_regions);
}


HeapRegion* G1CollectedHeap::new_heap_region(uint hrs_index,
                                             MemRegion mr) {
  return new HeapRegion(hrs_index, bot(), mr);
}

// Private methods.

HeapRegion*
G1CollectedHeap::new_region_try_secondary_free_list(bool is_old) {
  MutexLockerEx x(SecondaryFreeList_lock, Mutex::_no_safepoint_check_flag);
  while (!_secondary_free_list.is_empty() || free_regions_coming()) {
    if (!_secondary_free_list.is_empty()) {
      log_develop_trace(gc, freelist)("G1ConcRegionFreeing [region alloc] : "
                                      "secondary_free_list has %u entries",
                                      _secondary_free_list.length());
      // It looks as if there are free regions available on the
      // secondary_free_list. Let's move them to the free_list and try
      // again to allocate from it.
      append_secondary_free_list();

      assert(_hrm.num_free_regions() > 0, "if the secondary_free_list was not "
             "empty we should have moved at least one entry to the free_list");
      HeapRegion* res = _hrm.allocate_free_region(is_old);
      log_develop_trace(gc, freelist)("G1ConcRegionFreeing [region alloc] : "
                                      "allocated " HR_FORMAT " from secondary_free_list",
                                      HR_FORMAT_PARAMS(res));
      return res;
    }

    // Wait here until we get notified either when (a) there are no
    // more free regions coming or (b) some regions have been moved on
    // the secondary_free_list.
    SecondaryFreeList_lock->wait(Mutex::_no_safepoint_check_flag);
  }

  log_develop_trace(gc, freelist)("G1ConcRegionFreeing [region alloc] : "
                                  "could not allocate from secondary_free_list");
  return NULL;
}

HeapRegion* G1CollectedHeap::new_region(size_t word_size, bool is_old, bool do_expand) {
  assert(!is_humongous(word_size) || word_size <= HeapRegion::GrainWords,
         "the only time we use this to allocate a humongous region is "
         "when we are allocating a single humongous region");

  HeapRegion* res;
  if (G1StressConcRegionFreeing) {
    if (!_secondary_free_list.is_empty()) {
      log_develop_trace(gc, freelist)("G1ConcRegionFreeing [region alloc] : "
                                      "forced to look at the secondary_free_list");
      res = new_region_try_secondary_free_list(is_old);
      if (res != NULL) {
        return res;
      }
    }
  }

  res = _hrm.allocate_free_region(is_old);

  if (res == NULL) {
    log_develop_trace(gc, freelist)("G1ConcRegionFreeing [region alloc] : "
                                    "res == NULL, trying the secondary_free_list");
    res = new_region_try_secondary_free_list(is_old);
  }
  if (res == NULL && do_expand && _expand_heap_after_alloc_failure) {
    // Currently, only attempts to allocate GC alloc regions set
    // do_expand to true. So, we should only reach here during a
    // safepoint. If this assumption changes we might have to
    // reconsider the use of _expand_heap_after_alloc_failure.
    assert(SafepointSynchronize::is_at_safepoint(), "invariant");

    log_debug(gc, ergo, heap)("Attempt heap expansion (region allocation request failed). Allocation request: " SIZE_FORMAT "B",
                              word_size * HeapWordSize);

    if (expand(word_size * HeapWordSize)) {
      // Given that expand() succeeded in expanding the heap, and we
      // always expand the heap by an amount aligned to the heap
      // region size, the free list should in theory not be empty.
      // In either case allocate_free_region() will check for NULL.
      res = _hrm.allocate_free_region(is_old);
    } else {
      _expand_heap_after_alloc_failure = false;
    }
  }
  return res;
}

HeapWord*
G1CollectedHeap::humongous_obj_allocate_initialize_regions(uint first,
                                                           uint num_regions,
                                                           size_t word_size,
                                                           AllocationContext_t context) {
  assert(first != G1_NO_HRM_INDEX, "pre-condition");
  assert(is_humongous(word_size), "word_size should be humongous");
  assert(num_regions * HeapRegion::GrainWords >= word_size, "pre-condition");

  // Index of last region in the series.
  uint last = first + num_regions - 1;

  // We need to initialize the region(s) we just discovered. This is
  // a bit tricky given that it can happen concurrently with
  // refinement threads refining cards on these regions and
  // potentially wanting to refine the BOT as they are scanning
  // those cards (this can happen shortly after a cleanup; see CR
  // 6991377). So we have to set up the region(s) carefully and in
  // a specific order.

  // The word size sum of all the regions we will allocate.
  size_t word_size_sum = (size_t) num_regions * HeapRegion::GrainWords;
  assert(word_size <= word_size_sum, "sanity");

  // This will be the "starts humongous" region.
  HeapRegion* first_hr = region_at(first);
  // The header of the new object will be placed at the bottom of
  // the first region.
  HeapWord* new_obj = first_hr->bottom();
  // This will be the new top of the new object.
  HeapWord* obj_top = new_obj + word_size;

  // First, we need to zero the header of the space that we will be
  // allocating. When we update top further down, some refinement
  // threads might try to scan the region. By zeroing the header we
  // ensure that any thread that will try to scan the region will
  // come across the zero klass word and bail out.
  //
  // NOTE: It would not have been correct to have used
  // CollectedHeap::fill_with_object() and make the space look like
  // an int array. The thread that is doing the allocation will
  // later update the object header to a potentially different array
  // type and, for a very short period of time, the klass and length
  // fields will be inconsistent. This could cause a refinement
  // thread to calculate the object size incorrectly.
  Copy::fill_to_words(new_obj, oopDesc::header_size(), 0);

  // Next, pad out the unused tail of the last region with filler
  // objects, for improved usage accounting.
  // How many words we use for filler objects.
  size_t word_fill_size = word_size_sum - word_size;

  // How many words memory we "waste" which cannot hold a filler object.
  size_t words_not_fillable = 0;

  if (word_fill_size >= min_fill_size()) {
    fill_with_objects(obj_top, word_fill_size);
  } else if (word_fill_size > 0) {
    // We have space to fill, but we cannot fit an object there.
    words_not_fillable = word_fill_size;
    word_fill_size = 0;
  }

  // We will set up the first region as "starts humongous". This
  // will also update the BOT covering all the regions to reflect
  // that there is a single object that starts at the bottom of the
  // first region.
  first_hr->set_starts_humongous(obj_top, word_fill_size);
  first_hr->set_allocation_context(context);
  // Then, if there are any, we will set up the "continues
  // humongous" regions.
  HeapRegion* hr = NULL;
  for (uint i = first + 1; i <= last; ++i) {
    hr = region_at(i);
    hr->set_continues_humongous(first_hr);
    hr->set_allocation_context(context);
  }

  // Up to this point no concurrent thread would have been able to
  // do any scanning on any region in this series. All the top
  // fields still point to bottom, so the intersection between
  // [bottom,top] and [card_start,card_end] will be empty. Before we
  // update the top fields, we'll do a storestore to make sure that
  // no thread sees the update to top before the zeroing of the
  // object header and the BOT initialization.
  OrderAccess::storestore();

  // Now, we will update the top fields of the "continues humongous"
  // regions except the last one.
  for (uint i = first; i < last; ++i) {
    hr = region_at(i);
    hr->set_top(hr->end());
  }

  hr = region_at(last);
  // If we cannot fit a filler object, we must set top to the end
  // of the humongous object, otherwise we cannot iterate the heap
  // and the BOT will not be complete.
  hr->set_top(hr->end() - words_not_fillable);

  assert(hr->bottom() < obj_top && obj_top <= hr->end(),
         "obj_top should be in last region");

  _verifier->check_bitmaps("Humongous Region Allocation", first_hr);

  assert(words_not_fillable == 0 ||
         first_hr->bottom() + word_size_sum - words_not_fillable == hr->top(),
         "Miscalculation in humongous allocation");

  increase_used((word_size_sum - words_not_fillable) * HeapWordSize);

  for (uint i = first; i <= last; ++i) {
    hr = region_at(i);
    _humongous_set.add(hr);
    _hr_printer.alloc(hr);
  }

  return new_obj;
}

size_t G1CollectedHeap::humongous_obj_size_in_regions(size_t word_size) {
  assert(is_humongous(word_size), "Object of size " SIZE_FORMAT " must be humongous here", word_size);
  return align_up(word_size, HeapRegion::GrainWords) / HeapRegion::GrainWords;
}

// If could fit into free regions w/o expansion, try.
// Otherwise, if can expand, do so.
// Otherwise, if using ex regions might help, try with ex given back.
HeapWord* G1CollectedHeap::humongous_obj_allocate(size_t word_size, AllocationContext_t context) {
  assert_heap_locked_or_at_safepoint(true /* should_be_vm_thread */);

  _verifier->verify_region_sets_optional();

  uint first = G1_NO_HRM_INDEX;
  uint obj_regions = (uint) humongous_obj_size_in_regions(word_size);

  if (obj_regions == 1) {
    // Only one region to allocate, try to use a fast path by directly allocating
    // from the free lists. Do not try to expand here, we will potentially do that
    // later.
    HeapRegion* hr = new_region(word_size, true /* is_old */, false /* do_expand */);
    if (hr != NULL) {
      first = hr->hrm_index();
    }
  } else {
    // We can't allocate humongous regions spanning more than one region while
    // cleanupComplete() is running, since some of the regions we find to be
    // empty might not yet be added to the free list. It is not straightforward
    // to know in which list they are on so that we can remove them. We only
    // need to do this if we need to allocate more than one region to satisfy the
    // current humongous allocation request. If we are only allocating one region
    // we use the one-region region allocation code (see above), that already
    // potentially waits for regions from the secondary free list.
    wait_while_free_regions_coming();
    append_secondary_free_list_if_not_empty_with_lock();

    // Policy: Try only empty regions (i.e. already committed first). Maybe we
    // are lucky enough to find some.
    first = _hrm.find_contiguous_only_empty(obj_regions);
    if (first != G1_NO_HRM_INDEX) {
      _hrm.allocate_free_regions_starting_at(first, obj_regions);
    }
  }

  if (first == G1_NO_HRM_INDEX) {
    // Policy: We could not find enough regions for the humongous object in the
    // free list. Look through the heap to find a mix of free and uncommitted regions.
    // If so, try expansion.
    first = _hrm.find_contiguous_empty_or_unavailable(obj_regions);
    if (first != G1_NO_HRM_INDEX) {
      // We found something. Make sure these regions are committed, i.e. expand
      // the heap. Alternatively we could do a defragmentation GC.
      log_debug(gc, ergo, heap)("Attempt heap expansion (humongous allocation request failed). Allocation request: " SIZE_FORMAT "B",
                                    word_size * HeapWordSize);

      _hrm.expand_at(first, obj_regions, workers());
      g1_policy()->record_new_heap_size(num_regions());

#ifdef ASSERT
      for (uint i = first; i < first + obj_regions; ++i) {
        HeapRegion* hr = region_at(i);
        assert(hr->is_free(), "sanity");
        assert(hr->is_empty(), "sanity");
        assert(is_on_master_free_list(hr), "sanity");
      }
#endif
      _hrm.allocate_free_regions_starting_at(first, obj_regions);
    } else {
      // Policy: Potentially trigger a defragmentation GC.
    }
  }

  HeapWord* result = NULL;
  if (first != G1_NO_HRM_INDEX) {
    result = humongous_obj_allocate_initialize_regions(first, obj_regions,
                                                       word_size, context);
    assert(result != NULL, "it should always return a valid result");

    // A successful humongous object allocation changes the used space
    // information of the old generation so we need to recalculate the
    // sizes and update the jstat counters here.
    g1mm()->update_sizes();
  }

  _verifier->verify_region_sets_optional();

  return result;
}

HeapWord* G1CollectedHeap::allocate_new_tlab(size_t word_size) {
  assert_heap_not_locked_and_not_at_safepoint();
  assert(!is_humongous(word_size), "we do not allow humongous TLABs");

  uint dummy_gc_count_before;
  uint dummy_gclocker_retry_count = 0;
  return attempt_allocation(word_size, &dummy_gc_count_before, &dummy_gclocker_retry_count);
}

HeapWord*
G1CollectedHeap::mem_allocate(size_t word_size,
                              bool*  gc_overhead_limit_was_exceeded) {
  assert_heap_not_locked_and_not_at_safepoint();

  // Loop until the allocation is satisfied, or unsatisfied after GC.
  for (uint try_count = 1, gclocker_retry_count = 0; /* we'll return */; try_count += 1) {
    uint gc_count_before;

    HeapWord* result = NULL;
    if (!is_humongous(word_size)) {
      result = attempt_allocation(word_size, &gc_count_before, &gclocker_retry_count);
    } else {
      result = attempt_allocation_humongous(word_size, &gc_count_before, &gclocker_retry_count);
    }
    if (result != NULL) {
      return result;
    }

    // Create the garbage collection operation...
    VM_G1CollectForAllocation op(gc_count_before, word_size);
    op.set_allocation_context(AllocationContext::current());

    // ...and get the VM thread to execute it.
    VMThread::execute(&op);

    if (op.prologue_succeeded() && op.pause_succeeded()) {
      // If the operation was successful we'll return the result even
      // if it is NULL. If the allocation attempt failed immediately
      // after a Full GC, it's unlikely we'll be able to allocate now.
      HeapWord* result = op.result();
      if (result != NULL && !is_humongous(word_size)) {
        // Allocations that take place on VM operations do not do any
        // card dirtying and we have to do it here. We only have to do
        // this for non-humongous allocations, though.
        dirty_young_block(result, word_size);
      }
      return result;
    } else {
      if (gclocker_retry_count > GCLockerRetryAllocationCount) {
        return NULL;
      }
      assert(op.result() == NULL,
             "the result should be NULL if the VM op did not succeed");
    }

    // Give a warning if we seem to be looping forever.
    if ((QueuedAllocationWarningCount > 0) &&
        (try_count % QueuedAllocationWarningCount == 0)) {
      log_warning(gc)("G1CollectedHeap::mem_allocate retries %d times", try_count);
    }
  }

  ShouldNotReachHere();
  return NULL;
}

HeapWord* G1CollectedHeap::attempt_allocation_slow(size_t word_size,
                                                   AllocationContext_t context,
                                                   uint* gc_count_before_ret,
                                                   uint* gclocker_retry_count_ret) {
  // Make sure you read the note in attempt_allocation_humongous().

  assert_heap_not_locked_and_not_at_safepoint();
  assert(!is_humongous(word_size), "attempt_allocation_slow() should not "
         "be called for humongous allocation requests");

  // We should only get here after the first-level allocation attempt
  // (attempt_allocation()) failed to allocate.

  // We will loop until a) we manage to successfully perform the
  // allocation or b) we successfully schedule a collection which
  // fails to perform the allocation. b) is the only case when we'll
  // return NULL.
  HeapWord* result = NULL;
  for (int try_count = 1; /* we'll return */; try_count += 1) {
    bool should_try_gc;
    uint gc_count_before;

    {
      MutexLockerEx x(Heap_lock);
      result = _allocator->attempt_allocation_locked(word_size, context);
      if (result != NULL) {
        return result;
      }

      if (GCLocker::is_active_and_needs_gc()) {
        if (g1_policy()->can_expand_young_list()) {
          // No need for an ergo verbose message here,
          // can_expand_young_list() does this when it returns true.
          result = _allocator->attempt_allocation_force(word_size, context);
          if (result != NULL) {
            return result;
          }
        }
        should_try_gc = false;
      } else {
        // The GCLocker may not be active but the GCLocker initiated
        // GC may not yet have been performed (GCLocker::needs_gc()
        // returns true). In this case we do not try this GC and
        // wait until the GCLocker initiated GC is performed, and
        // then retry the allocation.
        if (GCLocker::needs_gc()) {
          should_try_gc = false;
        } else {
          // Read the GC count while still holding the Heap_lock.
          gc_count_before = total_collections();
          should_try_gc = true;
        }
      }
    }

    if (should_try_gc) {
      bool succeeded;
      result = do_collection_pause(word_size, gc_count_before, &succeeded,
                                   GCCause::_g1_inc_collection_pause);
      if (result != NULL) {
        assert(succeeded, "only way to get back a non-NULL result");
        return result;
      }

      if (succeeded) {
        // If we get here we successfully scheduled a collection which
        // failed to allocate. No point in trying to allocate
        // further. We'll just return NULL.
        MutexLockerEx x(Heap_lock);
        *gc_count_before_ret = total_collections();
        return NULL;
      }
    } else {
      if (*gclocker_retry_count_ret > GCLockerRetryAllocationCount) {
        MutexLockerEx x(Heap_lock);
        *gc_count_before_ret = total_collections();
        return NULL;
      }
      // The GCLocker is either active or the GCLocker initiated
      // GC has not yet been performed. Stall until it is and
      // then retry the allocation.
      GCLocker::stall_until_clear();
      (*gclocker_retry_count_ret) += 1;
    }

    // We can reach here if we were unsuccessful in scheduling a
    // collection (because another thread beat us to it) or if we were
    // stalled due to the GC locker. In either can we should retry the
    // allocation attempt in case another thread successfully
    // performed a collection and reclaimed enough space. We do the
    // first attempt (without holding the Heap_lock) here and the
    // follow-on attempt will be at the start of the next loop
    // iteration (after taking the Heap_lock).
    result = _allocator->attempt_allocation(word_size, context);
    if (result != NULL) {
      return result;
    }

    // Give a warning if we seem to be looping forever.
    if ((QueuedAllocationWarningCount > 0) &&
        (try_count % QueuedAllocationWarningCount == 0)) {
      log_warning(gc)("G1CollectedHeap::attempt_allocation_slow() "
                      "retries %d times", try_count);
    }
  }

  ShouldNotReachHere();
  return NULL;
}

void G1CollectedHeap::begin_archive_alloc_range(bool open) {
  assert_at_safepoint(true /* should_be_vm_thread */);
  if (_archive_allocator == NULL) {
    _archive_allocator = G1ArchiveAllocator::create_allocator(this, open);
  }
}

bool G1CollectedHeap::is_archive_alloc_too_large(size_t word_size) {
  // Allocations in archive regions cannot be of a size that would be considered
  // humongous even for a minimum-sized region, because G1 region sizes/boundaries
  // may be different at archive-restore time.
  return word_size >= humongous_threshold_for(HeapRegion::min_region_size_in_words());
}

HeapWord* G1CollectedHeap::archive_mem_allocate(size_t word_size) {
  assert_at_safepoint(true /* should_be_vm_thread */);
  assert(_archive_allocator != NULL, "_archive_allocator not initialized");
  if (is_archive_alloc_too_large(word_size)) {
    return NULL;
  }
  return _archive_allocator->archive_mem_allocate(word_size);
}

void G1CollectedHeap::end_archive_alloc_range(GrowableArray<MemRegion>* ranges,
                                              size_t end_alignment_in_bytes) {
  assert_at_safepoint(true /* should_be_vm_thread */);
  assert(_archive_allocator != NULL, "_archive_allocator not initialized");

  // Call complete_archive to do the real work, filling in the MemRegion
  // array with the archive regions.
  _archive_allocator->complete_archive(ranges, end_alignment_in_bytes);
  delete _archive_allocator;
  _archive_allocator = NULL;
}

bool G1CollectedHeap::check_archive_addresses(MemRegion* ranges, size_t count) {
  assert(ranges != NULL, "MemRegion array NULL");
  assert(count != 0, "No MemRegions provided");
  MemRegion reserved = _hrm.reserved();
  for (size_t i = 0; i < count; i++) {
    if (!reserved.contains(ranges[i].start()) || !reserved.contains(ranges[i].last())) {
      return false;
    }
  }
  return true;
}

bool G1CollectedHeap::alloc_archive_regions(MemRegion* ranges,
                                            size_t count,
                                            bool open) {
  assert(!is_init_completed(), "Expect to be called at JVM init time");
  assert(ranges != NULL, "MemRegion array NULL");
  assert(count != 0, "No MemRegions provided");
  MutexLockerEx x(Heap_lock);

  MemRegion reserved = _hrm.reserved();
  HeapWord* prev_last_addr = NULL;
  HeapRegion* prev_last_region = NULL;

  // Temporarily disable pretouching of heap pages. This interface is used
  // when mmap'ing archived heap data in, so pre-touching is wasted.
  FlagSetting fs(AlwaysPreTouch, false);

  // Enable archive object checking used by G1MarkSweep. We have to let it know
  // about each archive range, so that objects in those ranges aren't marked.
  G1ArchiveAllocator::enable_archive_object_check();

  // For each specified MemRegion range, allocate the corresponding G1
  // regions and mark them as archive regions. We expect the ranges
  // in ascending starting address order, without overlap.
  for (size_t i = 0; i < count; i++) {
    MemRegion curr_range = ranges[i];
    HeapWord* start_address = curr_range.start();
    size_t word_size = curr_range.word_size();
    HeapWord* last_address = curr_range.last();
    size_t commits = 0;

    guarantee(reserved.contains(start_address) && reserved.contains(last_address),
              "MemRegion outside of heap [" PTR_FORMAT ", " PTR_FORMAT "]",
              p2i(start_address), p2i(last_address));
    guarantee(start_address > prev_last_addr,
              "Ranges not in ascending order: " PTR_FORMAT " <= " PTR_FORMAT ,
              p2i(start_address), p2i(prev_last_addr));
    prev_last_addr = last_address;

    // Check for ranges that start in the same G1 region in which the previous
    // range ended, and adjust the start address so we don't try to allocate
    // the same region again. If the current range is entirely within that
    // region, skip it, just adjusting the recorded top.
    HeapRegion* start_region = _hrm.addr_to_region(start_address);
    if ((prev_last_region != NULL) && (start_region == prev_last_region)) {
      start_address = start_region->end();
      if (start_address > last_address) {
        increase_used(word_size * HeapWordSize);
        start_region->set_top(last_address + 1);
        continue;
      }
      start_region->set_top(start_address);
      curr_range = MemRegion(start_address, last_address + 1);
      start_region = _hrm.addr_to_region(start_address);
    }

    // Perform the actual region allocation, exiting if it fails.
    // Then note how much new space we have allocated.
    if (!_hrm.allocate_containing_regions(curr_range, &commits, workers())) {
      return false;
    }
    increase_used(word_size * HeapWordSize);
    if (commits != 0) {
      log_debug(gc, ergo, heap)("Attempt heap expansion (allocate archive regions). Total size: " SIZE_FORMAT "B",
                                HeapRegion::GrainWords * HeapWordSize * commits);

    }

    // Mark each G1 region touched by the range as archive, add it to
    // the old set, and set the allocation context and top.
    HeapRegion* curr_region = _hrm.addr_to_region(start_address);
    HeapRegion* last_region = _hrm.addr_to_region(last_address);
    prev_last_region = last_region;

    while (curr_region != NULL) {
      assert(curr_region->is_empty() && !curr_region->is_pinned(),
             "Region already in use (index %u)", curr_region->hrm_index());
      curr_region->set_allocation_context(AllocationContext::system());
      if (open) {
        curr_region->set_open_archive();
      } else {
        curr_region->set_closed_archive();
      }
      _hr_printer.alloc(curr_region);
      _old_set.add(curr_region);
      HeapWord* top;
      HeapRegion* next_region;
      if (curr_region != last_region) {
        top = curr_region->end();
        next_region = _hrm.next_region_in_heap(curr_region);
      } else {
        top = last_address + 1;
        next_region = NULL;
      }
      curr_region->set_top(top);
      curr_region->set_first_dead(top);
      curr_region->set_end_of_live(top);
      curr_region = next_region;
    }

    // Notify mark-sweep of the archive
    G1ArchiveAllocator::set_range_archive(curr_range, open);
  }
  return true;
}

void G1CollectedHeap::fill_archive_regions(MemRegion* ranges, size_t count) {
  assert(!is_init_completed(), "Expect to be called at JVM init time");
  assert(ranges != NULL, "MemRegion array NULL");
  assert(count != 0, "No MemRegions provided");
  MemRegion reserved = _hrm.reserved();
  HeapWord *prev_last_addr = NULL;
  HeapRegion* prev_last_region = NULL;

  // For each MemRegion, create filler objects, if needed, in the G1 regions
  // that contain the address range. The address range actually within the
  // MemRegion will not be modified. That is assumed to have been initialized
  // elsewhere, probably via an mmap of archived heap data.
  MutexLockerEx x(Heap_lock);
  for (size_t i = 0; i < count; i++) {
    HeapWord* start_address = ranges[i].start();
    HeapWord* last_address = ranges[i].last();

    assert(reserved.contains(start_address) && reserved.contains(last_address),
           "MemRegion outside of heap [" PTR_FORMAT ", " PTR_FORMAT "]",
           p2i(start_address), p2i(last_address));
    assert(start_address > prev_last_addr,
           "Ranges not in ascending order: " PTR_FORMAT " <= " PTR_FORMAT ,
           p2i(start_address), p2i(prev_last_addr));

    HeapRegion* start_region = _hrm.addr_to_region(start_address);
    HeapRegion* last_region = _hrm.addr_to_region(last_address);
    HeapWord* bottom_address = start_region->bottom();

    // Check for a range beginning in the same region in which the
    // previous one ended.
    if (start_region == prev_last_region) {
      bottom_address = prev_last_addr + 1;
    }

    // Verify that the regions were all marked as archive regions by
    // alloc_archive_regions.
    HeapRegion* curr_region = start_region;
    while (curr_region != NULL) {
      guarantee(curr_region->is_archive(),
                "Expected archive region at index %u", curr_region->hrm_index());
      if (curr_region != last_region) {
        curr_region = _hrm.next_region_in_heap(curr_region);
      } else {
        curr_region = NULL;
      }
    }

    prev_last_addr = last_address;
    prev_last_region = last_region;

    // Fill the memory below the allocated range with dummy object(s),
    // if the region bottom does not match the range start, or if the previous
    // range ended within the same G1 region, and there is a gap.
    if (start_address != bottom_address) {
      size_t fill_size = pointer_delta(start_address, bottom_address);
      G1CollectedHeap::fill_with_objects(bottom_address, fill_size);
      increase_used(fill_size * HeapWordSize);
    }
  }
}

inline HeapWord* G1CollectedHeap::attempt_allocation(size_t word_size,
                                                     uint* gc_count_before_ret,
                                                     uint* gclocker_retry_count_ret) {
  assert_heap_not_locked_and_not_at_safepoint();
  assert(!is_humongous(word_size), "attempt_allocation() should not "
         "be called for humongous allocation requests");

  AllocationContext_t context = AllocationContext::current();
  HeapWord* result = _allocator->attempt_allocation(word_size, context);

  if (result == NULL) {
    result = attempt_allocation_slow(word_size,
                                     context,
                                     gc_count_before_ret,
                                     gclocker_retry_count_ret);
  }
  assert_heap_not_locked();
  if (result != NULL) {
    dirty_young_block(result, word_size);
  }
  return result;
}

void G1CollectedHeap::dealloc_archive_regions(MemRegion* ranges, size_t count) {
  assert(!is_init_completed(), "Expect to be called at JVM init time");
  assert(ranges != NULL, "MemRegion array NULL");
  assert(count != 0, "No MemRegions provided");
  MemRegion reserved = _hrm.reserved();
  HeapWord* prev_last_addr = NULL;
  HeapRegion* prev_last_region = NULL;
  size_t size_used = 0;
  size_t uncommitted_regions = 0;

  // For each Memregion, free the G1 regions that constitute it, and
  // notify mark-sweep that the range is no longer to be considered 'archive.'
  MutexLockerEx x(Heap_lock);
  for (size_t i = 0; i < count; i++) {
    HeapWord* start_address = ranges[i].start();
    HeapWord* last_address = ranges[i].last();

    assert(reserved.contains(start_address) && reserved.contains(last_address),
           "MemRegion outside of heap [" PTR_FORMAT ", " PTR_FORMAT "]",
           p2i(start_address), p2i(last_address));
    assert(start_address > prev_last_addr,
           "Ranges not in ascending order: " PTR_FORMAT " <= " PTR_FORMAT ,
           p2i(start_address), p2i(prev_last_addr));
    size_used += ranges[i].byte_size();
    prev_last_addr = last_address;

    HeapRegion* start_region = _hrm.addr_to_region(start_address);
    HeapRegion* last_region = _hrm.addr_to_region(last_address);

    // Check for ranges that start in the same G1 region in which the previous
    // range ended, and adjust the start address so we don't try to free
    // the same region again. If the current range is entirely within that
    // region, skip it.
    if (start_region == prev_last_region) {
      start_address = start_region->end();
      if (start_address > last_address) {
        continue;
      }
      start_region = _hrm.addr_to_region(start_address);
    }
    prev_last_region = last_region;

    // After verifying that each region was marked as an archive region by
    // alloc_archive_regions, set it free and empty and uncommit it.
    HeapRegion* curr_region = start_region;
    while (curr_region != NULL) {
      guarantee(curr_region->is_archive(),
                "Expected archive region at index %u", curr_region->hrm_index());
      uint curr_index = curr_region->hrm_index();
      _old_set.remove(curr_region);
      curr_region->set_free();
      curr_region->set_top(curr_region->bottom());
      if (curr_region != last_region) {
        curr_region = _hrm.next_region_in_heap(curr_region);
      } else {
        curr_region = NULL;
      }
      _hrm.shrink_at(curr_index, 1);
      uncommitted_regions++;
    }

    // Notify mark-sweep that this is no longer an archive range.
    G1ArchiveAllocator::set_range_archive(ranges[i], false);
  }

  if (uncommitted_regions != 0) {
    log_debug(gc, ergo, heap)("Attempt heap shrinking (uncommitted archive regions). Total size: " SIZE_FORMAT "B",
                              HeapRegion::GrainWords * HeapWordSize * uncommitted_regions);
  }
  decrease_used(size_used);
}

HeapWord* G1CollectedHeap::attempt_allocation_humongous(size_t word_size,
                                                        uint* gc_count_before_ret,
                                                        uint* gclocker_retry_count_ret) {
  // The structure of this method has a lot of similarities to
  // attempt_allocation_slow(). The reason these two were not merged
  // into a single one is that such a method would require several "if
  // allocation is not humongous do this, otherwise do that"
  // conditional paths which would obscure its flow. In fact, an early
  // version of this code did use a unified method which was harder to
  // follow and, as a result, it had subtle bugs that were hard to
  // track down. So keeping these two methods separate allows each to
  // be more readable. It will be good to keep these two in sync as
  // much as possible.

  assert_heap_not_locked_and_not_at_safepoint();
  assert(is_humongous(word_size), "attempt_allocation_humongous() "
         "should only be called for humongous allocations");

  // Humongous objects can exhaust the heap quickly, so we should check if we
  // need to start a marking cycle at each humongous object allocation. We do
  // the check before we do the actual allocation. The reason for doing it
  // before the allocation is that we avoid having to keep track of the newly
  // allocated memory while we do a GC.
  if (g1_policy()->need_to_start_conc_mark("concurrent humongous allocation",
                                           word_size)) {
    collect(GCCause::_g1_humongous_allocation);
  }

  // We will loop until a) we manage to successfully perform the
  // allocation or b) we successfully schedule a collection which
  // fails to perform the allocation. b) is the only case when we'll
  // return NULL.
  HeapWord* result = NULL;
  for (int try_count = 1; /* we'll return */; try_count += 1) {
    bool should_try_gc;
    uint gc_count_before;

    {
      MutexLockerEx x(Heap_lock);

      // Given that humongous objects are not allocated in young
      // regions, we'll first try to do the allocation without doing a
      // collection hoping that there's enough space in the heap.
      result = humongous_obj_allocate(word_size, AllocationContext::current());
      if (result != NULL) {
        size_t size_in_regions = humongous_obj_size_in_regions(word_size);
        g1_policy()->add_bytes_allocated_in_old_since_last_gc(size_in_regions * HeapRegion::GrainBytes);
        return result;
      }

      if (GCLocker::is_active_and_needs_gc()) {
        should_try_gc = false;
      } else {
         // The GCLocker may not be active but the GCLocker initiated
        // GC may not yet have been performed (GCLocker::needs_gc()
        // returns true). In this case we do not try this GC and
        // wait until the GCLocker initiated GC is performed, and
        // then retry the allocation.
        if (GCLocker::needs_gc()) {
          should_try_gc = false;
        } else {
          // Read the GC count while still holding the Heap_lock.
          gc_count_before = total_collections();
          should_try_gc = true;
        }
      }
    }

    if (should_try_gc) {
      // If we failed to allocate the humongous object, we should try to
      // do a collection pause (if we're allowed) in case it reclaims
      // enough space for the allocation to succeed after the pause.

      bool succeeded;
      result = do_collection_pause(word_size, gc_count_before, &succeeded,
                                   GCCause::_g1_humongous_allocation);
      if (result != NULL) {
        assert(succeeded, "only way to get back a non-NULL result");
        return result;
      }

      if (succeeded) {
        // If we get here we successfully scheduled a collection which
        // failed to allocate. No point in trying to allocate
        // further. We'll just return NULL.
        MutexLockerEx x(Heap_lock);
        *gc_count_before_ret = total_collections();
        return NULL;
      }
    } else {
      if (*gclocker_retry_count_ret > GCLockerRetryAllocationCount) {
        MutexLockerEx x(Heap_lock);
        *gc_count_before_ret = total_collections();
        return NULL;
      }
      // The GCLocker is either active or the GCLocker initiated
      // GC has not yet been performed. Stall until it is and
      // then retry the allocation.
      GCLocker::stall_until_clear();
      (*gclocker_retry_count_ret) += 1;
    }

    // We can reach here if we were unsuccessful in scheduling a
    // collection (because another thread beat us to it) or if we were
    // stalled due to the GC locker. In either can we should retry the
    // allocation attempt in case another thread successfully
    // performed a collection and reclaimed enough space.  Give a
    // warning if we seem to be looping forever.

    if ((QueuedAllocationWarningCount > 0) &&
        (try_count % QueuedAllocationWarningCount == 0)) {
      log_warning(gc)("G1CollectedHeap::attempt_allocation_humongous() "
                      "retries %d times", try_count);
    }
  }

  ShouldNotReachHere();
  return NULL;
}

HeapWord* G1CollectedHeap::attempt_allocation_at_safepoint(size_t word_size,
                                                           AllocationContext_t context,
                                                           bool expect_null_mutator_alloc_region) {
  assert_at_safepoint(true /* should_be_vm_thread */);
  assert(!_allocator->has_mutator_alloc_region(context) || !expect_null_mutator_alloc_region,
         "the current alloc region was unexpectedly found to be non-NULL");

  if (!is_humongous(word_size)) {
    return _allocator->attempt_allocation_locked(word_size, context);
  } else {
    HeapWord* result = humongous_obj_allocate(word_size, context);
    if (result != NULL && g1_policy()->need_to_start_conc_mark("STW humongous allocation")) {
      collector_state()->set_initiate_conc_mark_if_possible(true);
    }
    return result;
  }

  ShouldNotReachHere();
}

class PostCompactionPrinterClosure: public HeapRegionClosure {
private:
  G1HRPrinter* _hr_printer;
public:
  bool doHeapRegion(HeapRegion* hr) {
    assert(!hr->is_young(), "not expecting to find young regions");
    _hr_printer->post_compaction(hr);
    return false;
  }

  PostCompactionPrinterClosure(G1HRPrinter* hr_printer)
    : _hr_printer(hr_printer) { }
};

void G1CollectedHeap::print_hrm_post_compaction() {
  if (_hr_printer.is_active()) {
    PostCompactionPrinterClosure cl(hr_printer());
    heap_region_iterate(&cl);
  }

}

void G1CollectedHeap::abort_concurrent_cycle() {
  // Note: When we have a more flexible GC logging framework that
  // allows us to add optional attributes to a GC log record we
  // could consider timing and reporting how long we wait in the
  // following two methods.
  wait_while_free_regions_coming();
  // If we start the compaction before the CM threads finish
  // scanning the root regions we might trip them over as we'll
  // be moving objects / updating references. So let's wait until
  // they are done. By telling them to abort, they should complete
  // early.
  _cm->root_regions()->abort();
  _cm->root_regions()->wait_until_scan_finished();
  append_secondary_free_list_if_not_empty_with_lock();

  // Disable discovery and empty the discovered lists
  // for the CM ref processor.
  ref_processor_cm()->disable_discovery();
  ref_processor_cm()->abandon_partial_discovery();
  ref_processor_cm()->verify_no_references_recorded();

  // Abandon current iterations of concurrent marking and concurrent
  // refinement, if any are in progress.
  concurrent_mark()->abort();
}

void G1CollectedHeap::prepare_heap_for_full_collection() {
  // Make sure we'll choose a new allocation region afterwards.
  _allocator->release_mutator_alloc_region();
  _allocator->abandon_gc_alloc_regions();
  g1_rem_set()->cleanupHRRS();

  // We may have added regions to the current incremental collection
  // set between the last GC or pause and now. We need to clear the
  // incremental collection set and then start rebuilding it afresh
  // after this full GC.
  abandon_collection_set(collection_set());

  tear_down_region_sets(false /* free_list_only */);
  collector_state()->set_gcs_are_young(true);
}

void G1CollectedHeap::verify_before_full_collection(bool explicit_gc) {
  assert(!GCCause::is_user_requested_gc(gc_cause()) || explicit_gc, "invariant");
  assert(used() == recalculate_used(), "Should be equal");
  _verifier->verify_region_sets_optional();
  _verifier->verify_before_gc();
  _verifier->check_bitmaps("Full GC Start");
}

void G1CollectedHeap::prepare_heap_for_mutators() {
  // Delete metaspaces for unloaded class loaders and clean up loader_data graph
  ClassLoaderDataGraph::purge();
  MetaspaceAux::verify_metrics();

  // Prepare heap for normal collections.
  assert(num_free_regions() == 0, "we should not have added any free regions");
  rebuild_region_sets(false /* free_list_only */);
  abort_refinement();
  resize_if_necessary_after_full_collection();

  // Rebuild the strong code root lists for each region
  rebuild_strong_code_roots();

  // Start a new incremental collection set for the next pause
  start_new_collection_set();

  _allocator->init_mutator_alloc_region();

  // Post collection state updates.
  MetaspaceGC::compute_new_size();
}

void G1CollectedHeap::abort_refinement() {
  if (_hot_card_cache->use_cache()) {
    _hot_card_cache->reset_hot_cache();
  }

  // Discard all remembered set updates.
  JavaThread::dirty_card_queue_set().abandon_logs();
  assert(dirty_card_queue_set().completed_buffers_num() == 0, "DCQS should be empty");
}

void G1CollectedHeap::verify_after_full_collection() {
  check_gc_time_stamps();
  _hrm.verify_optional();
  _verifier->verify_region_sets_optional();
  _verifier->verify_after_gc();
  // Clear the previous marking bitmap, if needed for bitmap verification.
  // Note we cannot do this when we clear the next marking bitmap in
  // G1ConcurrentMark::abort() above since VerifyDuringGC verifies the
  // objects marked during a full GC against the previous bitmap.
  // But we need to clear it before calling check_bitmaps below since
  // the full GC has compacted objects and updated TAMS but not updated
  // the prev bitmap.
  if (G1VerifyBitmaps) {
    GCTraceTime(Debug, gc)("Clear Bitmap for Verification");
    _cm->clear_prev_bitmap(workers());
  }
  _verifier->check_bitmaps("Full GC End");

  // At this point there should be no regions in the
  // entire heap tagged as young.
  assert(check_young_list_empty(), "young list should be empty at this point");

  // Note: since we've just done a full GC, concurrent
  // marking is no longer active. Therefore we need not
  // re-enable reference discovery for the CM ref processor.
  // That will be done at the start of the next marking cycle.
  // We also know that the STW processor should no longer
  // discover any new references.
  assert(!ref_processor_stw()->discovery_enabled(), "Postcondition");
  assert(!ref_processor_cm()->discovery_enabled(), "Postcondition");
  ref_processor_stw()->verify_no_references_recorded();
  ref_processor_cm()->verify_no_references_recorded();
}

void G1CollectedHeap::print_heap_after_full_collection(G1HeapTransition* heap_transition) {
  // Post collection logging.
  // We should do this after we potentially resize the heap so
  // that all the COMMIT / UNCOMMIT events are generated before
  // the compaction events.
  print_hrm_post_compaction();
  heap_transition->print();
  print_heap_after_gc();
  print_heap_regions();
#ifdef TRACESPINNING
  ParallelTaskTerminator::print_termination_counts();
#endif
}

void G1CollectedHeap::do_full_collection_inner(G1FullGCScope* scope) {
  GCTraceTime(Info, gc) tm("Pause Full", NULL, gc_cause(), true);
  g1_policy()->record_full_collection_start();

  print_heap_before_gc();
  print_heap_regions();

  abort_concurrent_cycle();
  verify_before_full_collection(scope->is_explicit_gc());

  gc_prologue(true);
  prepare_heap_for_full_collection();

  G1FullCollector collector(scope, ref_processor_stw(), concurrent_mark()->next_mark_bitmap(), workers()->active_workers());
  collector.prepare_collection();
  collector.collect();
  collector.complete_collection();

  prepare_heap_for_mutators();

  g1_policy()->record_full_collection_end();
  gc_epilogue(true);

  verify_after_full_collection();

  print_heap_after_full_collection(scope->heap_transition());
}

bool G1CollectedHeap::do_full_collection(bool explicit_gc,
                                         bool clear_all_soft_refs) {
  assert_at_safepoint(true /* should_be_vm_thread */);

  if (GCLocker::check_active_before_gc()) {
    // Full GC was not completed.
    return false;
  }

  const bool do_clear_all_soft_refs = clear_all_soft_refs ||
      collector_policy()->should_clear_all_soft_refs();

  G1FullGCScope scope(explicit_gc, do_clear_all_soft_refs);
  do_full_collection_inner(&scope);

  // Full collection was successfully completed.
  return true;
}

void G1CollectedHeap::do_full_collection(bool clear_all_soft_refs) {
  // Currently, there is no facility in the do_full_collection(bool) API to notify
  // the caller that the collection did not succeed (e.g., because it was locked
  // out by the GC locker). So, right now, we'll ignore the return value.
  bool dummy = do_full_collection(true,                /* explicit_gc */
                                  clear_all_soft_refs);
}

void G1CollectedHeap::resize_if_necessary_after_full_collection() {
  // Capacity, free and used after the GC counted as full regions to
  // include the waste in the following calculations.
  const size_t capacity_after_gc = capacity();
  const size_t used_after_gc = capacity_after_gc - unused_committed_regions_in_bytes();

  // This is enforced in arguments.cpp.
  assert(MinHeapFreeRatio <= MaxHeapFreeRatio,
         "otherwise the code below doesn't make sense");

  // We don't have floating point command-line arguments
  const double minimum_free_percentage = (double) MinHeapFreeRatio / 100.0;
  const double maximum_used_percentage = 1.0 - minimum_free_percentage;
  const double maximum_free_percentage = (double) MaxHeapFreeRatio / 100.0;
  const double minimum_used_percentage = 1.0 - maximum_free_percentage;

  const size_t min_heap_size = collector_policy()->min_heap_byte_size();
  const size_t max_heap_size = collector_policy()->max_heap_byte_size();

  // We have to be careful here as these two calculations can overflow
  // 32-bit size_t's.
  double used_after_gc_d = (double) used_after_gc;
  double minimum_desired_capacity_d = used_after_gc_d / maximum_used_percentage;
  double maximum_desired_capacity_d = used_after_gc_d / minimum_used_percentage;

  // Let's make sure that they are both under the max heap size, which
  // by default will make them fit into a size_t.
  double desired_capacity_upper_bound = (double) max_heap_size;
  minimum_desired_capacity_d = MIN2(minimum_desired_capacity_d,
                                    desired_capacity_upper_bound);
  maximum_desired_capacity_d = MIN2(maximum_desired_capacity_d,
                                    desired_capacity_upper_bound);

  // We can now safely turn them into size_t's.
  size_t minimum_desired_capacity = (size_t) minimum_desired_capacity_d;
  size_t maximum_desired_capacity = (size_t) maximum_desired_capacity_d;

  // This assert only makes sense here, before we adjust them
  // with respect to the min and max heap size.
  assert(minimum_desired_capacity <= maximum_desired_capacity,
         "minimum_desired_capacity = " SIZE_FORMAT ", "
         "maximum_desired_capacity = " SIZE_FORMAT,
         minimum_desired_capacity, maximum_desired_capacity);

  // Should not be greater than the heap max size. No need to adjust
  // it with respect to the heap min size as it's a lower bound (i.e.,
  // we'll try to make the capacity larger than it, not smaller).
  minimum_desired_capacity = MIN2(minimum_desired_capacity, max_heap_size);
  // Should not be less than the heap min size. No need to adjust it
  // with respect to the heap max size as it's an upper bound (i.e.,
  // we'll try to make the capacity smaller than it, not greater).
  maximum_desired_capacity =  MAX2(maximum_desired_capacity, min_heap_size);

  if (capacity_after_gc < minimum_desired_capacity) {
    // Don't expand unless it's significant
    size_t expand_bytes = minimum_desired_capacity - capacity_after_gc;

    log_debug(gc, ergo, heap)("Attempt heap expansion (capacity lower than min desired capacity after Full GC). "
                              "Capacity: " SIZE_FORMAT "B occupancy: " SIZE_FORMAT "B live: " SIZE_FORMAT "B "
                              "min_desired_capacity: " SIZE_FORMAT "B (" UINTX_FORMAT " %%)",
                              capacity_after_gc, used_after_gc, used(), minimum_desired_capacity, MinHeapFreeRatio);

    expand(expand_bytes, _workers);

    // No expansion, now see if we want to shrink
  } else if (capacity_after_gc > maximum_desired_capacity) {
    // Capacity too large, compute shrinking size
    size_t shrink_bytes = capacity_after_gc - maximum_desired_capacity;

    log_debug(gc, ergo, heap)("Attempt heap shrinking (capacity higher than max desired capacity after Full GC). "
                              "Capacity: " SIZE_FORMAT "B occupancy: " SIZE_FORMAT "B live: " SIZE_FORMAT "B "
                              "maximum_desired_capacity: " SIZE_FORMAT "B (" UINTX_FORMAT " %%)",
                              capacity_after_gc, used_after_gc, used(), maximum_desired_capacity, MaxHeapFreeRatio);

    shrink(shrink_bytes);
  }
}

HeapWord* G1CollectedHeap::satisfy_failed_allocation_helper(size_t word_size,
                                                            AllocationContext_t context,
                                                            bool do_gc,
                                                            bool clear_all_soft_refs,
                                                            bool expect_null_mutator_alloc_region,
                                                            bool* gc_succeeded) {
  *gc_succeeded = true;
  // Let's attempt the allocation first.
  HeapWord* result =
    attempt_allocation_at_safepoint(word_size,
                                    context,
                                    expect_null_mutator_alloc_region);
  if (result != NULL) {
    assert(*gc_succeeded, "sanity");
    return result;
  }

  // In a G1 heap, we're supposed to keep allocation from failing by
  // incremental pauses.  Therefore, at least for now, we'll favor
  // expansion over collection.  (This might change in the future if we can
  // do something smarter than full collection to satisfy a failed alloc.)
  result = expand_and_allocate(word_size, context);
  if (result != NULL) {
    assert(*gc_succeeded, "sanity");
    return result;
  }

  if (do_gc) {
    // Expansion didn't work, we'll try to do a Full GC.
    *gc_succeeded = do_full_collection(false, /* explicit_gc */
                                       clear_all_soft_refs);
  }

  return NULL;
}

HeapWord* G1CollectedHeap::satisfy_failed_allocation(size_t word_size,
                                                     AllocationContext_t context,
                                                     bool* succeeded) {
  assert_at_safepoint(true /* should_be_vm_thread */);

  // Attempts to allocate followed by Full GC.
  HeapWord* result =
    satisfy_failed_allocation_helper(word_size,
                                     context,
                                     true,  /* do_gc */
                                     false, /* clear_all_soft_refs */
                                     false, /* expect_null_mutator_alloc_region */
                                     succeeded);

  if (result != NULL || !*succeeded) {
    return result;
  }

  // Attempts to allocate followed by Full GC that will collect all soft references.
  result = satisfy_failed_allocation_helper(word_size,
                                            context,
                                            true, /* do_gc */
                                            true, /* clear_all_soft_refs */
                                            true, /* expect_null_mutator_alloc_region */
                                            succeeded);

  if (result != NULL || !*succeeded) {
    return result;
  }

  // Attempts to allocate, no GC
  result = satisfy_failed_allocation_helper(word_size,
                                            context,
                                            false, /* do_gc */
                                            false, /* clear_all_soft_refs */
                                            true,  /* expect_null_mutator_alloc_region */
                                            succeeded);

  if (result != NULL) {
    assert(*succeeded, "sanity");
    return result;
  }

  assert(!collector_policy()->should_clear_all_soft_refs(),
         "Flag should have been handled and cleared prior to this point");

  // What else?  We might try synchronous finalization later.  If the total
  // space available is large enough for the allocation, then a more
  // complete compaction phase than we've tried so far might be
  // appropriate.
  assert(*succeeded, "sanity");
  return NULL;
}

// Attempting to expand the heap sufficiently
// to support an allocation of the given "word_size".  If
// successful, perform the allocation and return the address of the
// allocated block, or else "NULL".

HeapWord* G1CollectedHeap::expand_and_allocate(size_t word_size, AllocationContext_t context) {
  assert_at_safepoint(true /* should_be_vm_thread */);

  _verifier->verify_region_sets_optional();

  size_t expand_bytes = MAX2(word_size * HeapWordSize, MinHeapDeltaBytes);
  log_debug(gc, ergo, heap)("Attempt heap expansion (allocation request failed). Allocation request: " SIZE_FORMAT "B",
                            word_size * HeapWordSize);


  if (expand(expand_bytes, _workers)) {
    _hrm.verify_optional();
    _verifier->verify_region_sets_optional();
    return attempt_allocation_at_safepoint(word_size,
                                           context,
                                           false /* expect_null_mutator_alloc_region */);
  }
  return NULL;
}

bool G1CollectedHeap::expand(size_t expand_bytes, WorkGang* pretouch_workers, double* expand_time_ms) {
  size_t aligned_expand_bytes = ReservedSpace::page_align_size_up(expand_bytes);
  aligned_expand_bytes = align_up(aligned_expand_bytes,
                                       HeapRegion::GrainBytes);

  log_debug(gc, ergo, heap)("Expand the heap. requested expansion amount: " SIZE_FORMAT "B expansion amount: " SIZE_FORMAT "B",
                            expand_bytes, aligned_expand_bytes);

  if (is_maximal_no_gc()) {
    log_debug(gc, ergo, heap)("Did not expand the heap (heap already fully expanded)");
    return false;
  }

  double expand_heap_start_time_sec = os::elapsedTime();
  uint regions_to_expand = (uint)(aligned_expand_bytes / HeapRegion::GrainBytes);
  assert(regions_to_expand > 0, "Must expand by at least one region");

  uint expanded_by = _hrm.expand_by(regions_to_expand, pretouch_workers);
  if (expand_time_ms != NULL) {
    *expand_time_ms = (os::elapsedTime() - expand_heap_start_time_sec) * MILLIUNITS;
  }

  if (expanded_by > 0) {
    size_t actual_expand_bytes = expanded_by * HeapRegion::GrainBytes;
    assert(actual_expand_bytes <= aligned_expand_bytes, "post-condition");
    g1_policy()->record_new_heap_size(num_regions());
  } else {
    log_debug(gc, ergo, heap)("Did not expand the heap (heap expansion operation failed)");

    // The expansion of the virtual storage space was unsuccessful.
    // Let's see if it was because we ran out of swap.
    if (G1ExitOnExpansionFailure &&
        _hrm.available() >= regions_to_expand) {
      // We had head room...
      vm_exit_out_of_memory(aligned_expand_bytes, OOM_MMAP_ERROR, "G1 heap expansion");
    }
  }
  return regions_to_expand > 0;
}

void G1CollectedHeap::shrink_helper(size_t shrink_bytes) {
  size_t aligned_shrink_bytes =
    ReservedSpace::page_align_size_down(shrink_bytes);
  aligned_shrink_bytes = align_down(aligned_shrink_bytes,
                                         HeapRegion::GrainBytes);
  uint num_regions_to_remove = (uint)(shrink_bytes / HeapRegion::GrainBytes);

  uint num_regions_removed = _hrm.shrink_by(num_regions_to_remove);
  size_t shrunk_bytes = num_regions_removed * HeapRegion::GrainBytes;


  log_debug(gc, ergo, heap)("Shrink the heap. requested shrinking amount: " SIZE_FORMAT "B aligned shrinking amount: " SIZE_FORMAT "B attempted shrinking amount: " SIZE_FORMAT "B",
                            shrink_bytes, aligned_shrink_bytes, shrunk_bytes);
  if (num_regions_removed > 0) {
    g1_policy()->record_new_heap_size(num_regions());
  } else {
    log_debug(gc, ergo, heap)("Did not expand the heap (heap shrinking operation failed)");
  }
}

void G1CollectedHeap::shrink(size_t shrink_bytes) {
  _verifier->verify_region_sets_optional();

  // We should only reach here at the end of a Full GC which means we
  // should not not be holding to any GC alloc regions. The method
  // below will make sure of that and do any remaining clean up.
  _allocator->abandon_gc_alloc_regions();

  // Instead of tearing down / rebuilding the free lists here, we
  // could instead use the remove_all_pending() method on free_list to
  // remove only the ones that we need to remove.
  tear_down_region_sets(true /* free_list_only */);
  shrink_helper(shrink_bytes);
  rebuild_region_sets(true /* free_list_only */);

  _hrm.verify_optional();
  _verifier->verify_region_sets_optional();
}

// Public methods.

G1CollectedHeap::G1CollectedHeap(G1CollectorPolicy* collector_policy) :
  CollectedHeap(),
  _young_gen_sampling_thread(NULL),
  _collector_policy(collector_policy),
  _gc_timer_stw(new (ResourceObj::C_HEAP, mtGC) STWGCTimer()),
  _gc_tracer_stw(new (ResourceObj::C_HEAP, mtGC) G1NewTracer()),
  _g1_policy(create_g1_policy(_gc_timer_stw)),
  _collection_set(this, _g1_policy),
  _dirty_card_queue_set(false),
  _is_alive_closure_cm(this),
  _is_alive_closure_stw(this),
  _ref_processor_cm(NULL),
  _ref_processor_stw(NULL),
  _bot(NULL),
  _hot_card_cache(NULL),
  _g1_rem_set(NULL),
  _cr(NULL),
  _g1mm(NULL),
  _preserved_marks_set(true /* in_c_heap */),
  _secondary_free_list("Secondary Free List", new SecondaryFreeRegionListMtSafeChecker()),
  _old_set("Old Set", false /* humongous */, new OldRegionSetMtSafeChecker()),
  _humongous_set("Master Humongous Set", true /* humongous */, new HumongousRegionSetMtSafeChecker()),
  _humongous_reclaim_candidates(),
  _has_humongous_reclaim_candidates(false),
  _archive_allocator(NULL),
  _free_regions_coming(false),
  _gc_time_stamp(0),
  _summary_bytes_used(0),
  _survivor_evac_stats("Young", YoungPLABSize, PLABWeight),
  _old_evac_stats("Old", OldPLABSize, PLABWeight),
  _expand_heap_after_alloc_failure(true),
  _old_marking_cycles_started(0),
  _old_marking_cycles_completed(0),
  _in_cset_fast_test() {

  _workers = new WorkGang("GC Thread", ParallelGCThreads,
                          /* are_GC_task_threads */true,
                          /* are_ConcurrentGC_threads */false);
  _workers->initialize_workers();
  _verifier = new G1HeapVerifier(this);

  _allocator = G1Allocator::create_allocator(this);

  _heap_sizing_policy = G1HeapSizingPolicy::create(this, _g1_policy->analytics());

  _humongous_object_threshold_in_words = humongous_threshold_for(HeapRegion::GrainWords);

  // Override the default _filler_array_max_size so that no humongous filler
  // objects are created.
  _filler_array_max_size = _humongous_object_threshold_in_words;

  uint n_queues = ParallelGCThreads;
  _task_queues = new RefToScanQueueSet(n_queues);

  _evacuation_failed_info_array = NEW_C_HEAP_ARRAY(EvacuationFailedInfo, n_queues, mtGC);

  for (uint i = 0; i < n_queues; i++) {
    RefToScanQueue* q = new RefToScanQueue();
    q->initialize();
    _task_queues->register_queue(i, q);
    ::new (&_evacuation_failed_info_array[i]) EvacuationFailedInfo();
  }

  // Initialize the G1EvacuationFailureALot counters and flags.
  NOT_PRODUCT(reset_evacuation_should_fail();)

  guarantee(_task_queues != NULL, "task_queues allocation failure.");
}

G1RegionToSpaceMapper* G1CollectedHeap::create_aux_memory_mapper(const char* description,
                                                                 size_t size,
                                                                 size_t translation_factor) {
  size_t preferred_page_size = os::page_size_for_region_unaligned(size, 1);
  // Allocate a new reserved space, preferring to use large pages.
  ReservedSpace rs(size, preferred_page_size);
  G1RegionToSpaceMapper* result  =
    G1RegionToSpaceMapper::create_mapper(rs,
                                         size,
                                         rs.alignment(),
                                         HeapRegion::GrainBytes,
                                         translation_factor,
                                         mtGC);

  os::trace_page_sizes_for_requested_size(description,
                                          size,
                                          preferred_page_size,
                                          rs.alignment(),
                                          rs.base(),
                                          rs.size());

  return result;
}

jint G1CollectedHeap::initialize_concurrent_refinement() {
  jint ecode = JNI_OK;
  _cr = G1ConcurrentRefine::create(&ecode);
  return ecode;
}

jint G1CollectedHeap::initialize_young_gen_sampling_thread() {
  _young_gen_sampling_thread = new G1YoungRemSetSamplingThread();
  if (_young_gen_sampling_thread->osthread() == NULL) {
    vm_shutdown_during_initialization("Could not create G1YoungRemSetSamplingThread");
    return JNI_ENOMEM;
  }
  return JNI_OK;
}

jint G1CollectedHeap::initialize() {
  CollectedHeap::pre_initialize();
  os::enable_vtime();

  // Necessary to satisfy locking discipline assertions.

  MutexLocker x(Heap_lock);

  // While there are no constraints in the GC code that HeapWordSize
  // be any particular value, there are multiple other areas in the
  // system which believe this to be true (e.g. oop->object_size in some
  // cases incorrectly returns the size in wordSize units rather than
  // HeapWordSize).
  guarantee(HeapWordSize == wordSize, "HeapWordSize must equal wordSize");

  size_t init_byte_size = collector_policy()->initial_heap_byte_size();
  size_t max_byte_size = collector_policy()->max_heap_byte_size();
  size_t heap_alignment = collector_policy()->heap_alignment();

  // Ensure that the sizes are properly aligned.
  Universe::check_alignment(init_byte_size, HeapRegion::GrainBytes, "g1 heap");
  Universe::check_alignment(max_byte_size, HeapRegion::GrainBytes, "g1 heap");
  Universe::check_alignment(max_byte_size, heap_alignment, "g1 heap");

  // Reserve the maximum.

  // When compressed oops are enabled, the preferred heap base
  // is calculated by subtracting the requested size from the
  // 32Gb boundary and using the result as the base address for
  // heap reservation. If the requested size is not aligned to
  // HeapRegion::GrainBytes (i.e. the alignment that is passed
  // into the ReservedHeapSpace constructor) then the actual
  // base of the reserved heap may end up differing from the
  // address that was requested (i.e. the preferred heap base).
  // If this happens then we could end up using a non-optimal
  // compressed oops mode.

  ReservedSpace heap_rs = Universe::reserve_heap(max_byte_size,
                                                 heap_alignment);

  initialize_reserved_region((HeapWord*)heap_rs.base(), (HeapWord*)(heap_rs.base() + heap_rs.size()));

  // Create the barrier set for the entire reserved region.
  G1SATBCardTableLoggingModRefBS* bs
    = new G1SATBCardTableLoggingModRefBS(reserved_region());
  bs->initialize();
  assert(bs->is_a(BarrierSet::G1SATBCTLogging), "sanity");
  set_barrier_set(bs);

  // Create the hot card cache.
  _hot_card_cache = new G1HotCardCache(this);

  // Carve out the G1 part of the heap.
  ReservedSpace g1_rs = heap_rs.first_part(max_byte_size);
  size_t page_size = UseLargePages ? os::large_page_size() : os::vm_page_size();
  G1RegionToSpaceMapper* heap_storage =
    G1RegionToSpaceMapper::create_mapper(g1_rs,
                                         g1_rs.size(),
                                         page_size,
                                         HeapRegion::GrainBytes,
                                         1,
                                         mtJavaHeap);
  os::trace_page_sizes("Heap",
                       collector_policy()->min_heap_byte_size(),
                       max_byte_size,
                       page_size,
                       heap_rs.base(),
                       heap_rs.size());
  heap_storage->set_mapping_changed_listener(&_listener);

  // Create storage for the BOT, card table, card counts table (hot card cache) and the bitmaps.
  G1RegionToSpaceMapper* bot_storage =
    create_aux_memory_mapper("Block Offset Table",
                             G1BlockOffsetTable::compute_size(g1_rs.size() / HeapWordSize),
                             G1BlockOffsetTable::heap_map_factor());

  G1RegionToSpaceMapper* cardtable_storage =
    create_aux_memory_mapper("Card Table",
                             G1SATBCardTableLoggingModRefBS::compute_size(g1_rs.size() / HeapWordSize),
                             G1SATBCardTableLoggingModRefBS::heap_map_factor());

  G1RegionToSpaceMapper* card_counts_storage =
    create_aux_memory_mapper("Card Counts Table",
                             G1CardCounts::compute_size(g1_rs.size() / HeapWordSize),
                             G1CardCounts::heap_map_factor());

  size_t bitmap_size = G1CMBitMap::compute_size(g1_rs.size());
  G1RegionToSpaceMapper* prev_bitmap_storage =
    create_aux_memory_mapper("Prev Bitmap", bitmap_size, G1CMBitMap::heap_map_factor());
  G1RegionToSpaceMapper* next_bitmap_storage =
    create_aux_memory_mapper("Next Bitmap", bitmap_size, G1CMBitMap::heap_map_factor());

  _hrm.initialize(heap_storage, prev_bitmap_storage, next_bitmap_storage, bot_storage, cardtable_storage, card_counts_storage);
  g1_barrier_set()->initialize(cardtable_storage);
  // Do later initialization work for concurrent refinement.
  _hot_card_cache->initialize(card_counts_storage);

  // 6843694 - ensure that the maximum region index can fit
  // in the remembered set structures.
  const uint max_region_idx = (1U << (sizeof(RegionIdx_t)*BitsPerByte-1)) - 1;
  guarantee((max_regions() - 1) <= max_region_idx, "too many regions");

  // Also create a G1 rem set.
  _g1_rem_set = new G1RemSet(this, g1_barrier_set(), _hot_card_cache);
  _g1_rem_set->initialize(max_capacity(), max_regions());

  size_t max_cards_per_region = ((size_t)1 << (sizeof(CardIdx_t)*BitsPerByte-1)) - 1;
  guarantee(HeapRegion::CardsPerRegion > 0, "make sure it's initialized");
  guarantee(HeapRegion::CardsPerRegion < max_cards_per_region,
            "too many cards per region");

  FreeRegionList::set_unrealistically_long_length(max_regions() + 1);

  _bot = new G1BlockOffsetTable(reserved_region(), bot_storage);

  {
    HeapWord* start = _hrm.reserved().start();
    HeapWord* end = _hrm.reserved().end();
    size_t granularity = HeapRegion::GrainBytes;

    _in_cset_fast_test.initialize(start, end, granularity);
    _humongous_reclaim_candidates.initialize(start, end, granularity);
  }

  // Create the G1ConcurrentMark data structure and thread.
  // (Must do this late, so that "max_regions" is defined.)
  _cm = new G1ConcurrentMark(this, prev_bitmap_storage, next_bitmap_storage);
  if (_cm == NULL || !_cm->completed_initialization()) {
    vm_shutdown_during_initialization("Could not create/initialize G1ConcurrentMark");
    return JNI_ENOMEM;
  }
  _cmThread = _cm->cm_thread();

  // Now expand into the initial heap size.
  if (!expand(init_byte_size, _workers)) {
    vm_shutdown_during_initialization("Failed to allocate initial heap.");
    return JNI_ENOMEM;
  }

  // Perform any initialization actions delegated to the policy.
  g1_policy()->init(this, &_collection_set);

  JavaThread::satb_mark_queue_set().initialize(SATB_Q_CBL_mon,
                                               SATB_Q_FL_lock,
                                               G1SATBProcessCompletedThreshold,
                                               Shared_SATB_Q_lock);

  jint ecode = initialize_concurrent_refinement();
  if (ecode != JNI_OK) {
    return ecode;
  }

  ecode = initialize_young_gen_sampling_thread();
  if (ecode != JNI_OK) {
    return ecode;
  }

  JavaThread::dirty_card_queue_set().initialize(DirtyCardQ_CBL_mon,
                                                DirtyCardQ_FL_lock,
                                                (int)concurrent_refine()->yellow_zone(),
                                                (int)concurrent_refine()->red_zone(),
                                                Shared_DirtyCardQ_lock,
                                                NULL,  // fl_owner
                                                true); // init_free_ids

  dirty_card_queue_set().initialize(DirtyCardQ_CBL_mon,
                                    DirtyCardQ_FL_lock,
                                    -1, // never trigger processing
                                    -1, // no limit on length
                                    Shared_DirtyCardQ_lock,
                                    &JavaThread::dirty_card_queue_set());

  // Here we allocate the dummy HeapRegion that is required by the
  // G1AllocRegion class.
  HeapRegion* dummy_region = _hrm.get_dummy_region();

  // We'll re-use the same region whether the alloc region will
  // require BOT updates or not and, if it doesn't, then a non-young
  // region will complain that it cannot support allocations without
  // BOT updates. So we'll tag the dummy region as eden to avoid that.
  dummy_region->set_eden();
  // Make sure it's full.
  dummy_region->set_top(dummy_region->end());
  G1AllocRegion::setup(this, dummy_region);

  _allocator->init_mutator_alloc_region();

  // Do create of the monitoring and management support so that
  // values in the heap have been properly initialized.
  _g1mm = new G1MonitoringSupport(this);

  G1StringDedup::initialize();

  _preserved_marks_set.init(ParallelGCThreads);

  _collection_set.initialize(max_regions());

  return JNI_OK;
}

void G1CollectedHeap::stop() {
  // Stop all concurrent threads. We do this to make sure these threads
  // do not continue to execute and access resources (e.g. logging)
  // that are destroyed during shutdown.
  _cr->stop();
  _young_gen_sampling_thread->stop();
  _cmThread->stop();
  if (G1StringDedup::is_enabled()) {
    G1StringDedup::stop();
  }
}

void G1CollectedHeap::safepoint_synchronize_begin() {
  SuspendibleThreadSet::synchronize();
}

void G1CollectedHeap::safepoint_synchronize_end() {
  SuspendibleThreadSet::desynchronize();
}

size_t G1CollectedHeap::conservative_max_heap_alignment() {
  return HeapRegion::max_region_size();
}

void G1CollectedHeap::post_initialize() {
  ref_processing_init();
}

void G1CollectedHeap::ref_processing_init() {
  // Reference processing in G1 currently works as follows:
  //
  // * There are two reference processor instances. One is
  //   used to record and process discovered references
  //   during concurrent marking; the other is used to
  //   record and process references during STW pauses
  //   (both full and incremental).
  // * Both ref processors need to 'span' the entire heap as
  //   the regions in the collection set may be dotted around.
  //
  // * For the concurrent marking ref processor:
  //   * Reference discovery is enabled at initial marking.
  //   * Reference discovery is disabled and the discovered
  //     references processed etc during remarking.
  //   * Reference discovery is MT (see below).
  //   * Reference discovery requires a barrier (see below).
  //   * Reference processing may or may not be MT
  //     (depending on the value of ParallelRefProcEnabled
  //     and ParallelGCThreads).
  //   * A full GC disables reference discovery by the CM
  //     ref processor and abandons any entries on it's
  //     discovered lists.
  //
  // * For the STW processor:
  //   * Non MT discovery is enabled at the start of a full GC.
  //   * Processing and enqueueing during a full GC is non-MT.
  //   * During a full GC, references are processed after marking.
  //
  //   * Discovery (may or may not be MT) is enabled at the start
  //     of an incremental evacuation pause.
  //   * References are processed near the end of a STW evacuation pause.
  //   * For both types of GC:
  //     * Discovery is atomic - i.e. not concurrent.
  //     * Reference discovery will not need a barrier.

  MemRegion mr = reserved_region();

  bool mt_processing = ParallelRefProcEnabled && (ParallelGCThreads > 1);

  // Concurrent Mark ref processor
  _ref_processor_cm =
    new ReferenceProcessor(mr,    // span
                           mt_processing,
                                // mt processing
                           ParallelGCThreads,
                                // degree of mt processing
                           (ParallelGCThreads > 1) || (ConcGCThreads > 1),
                                // mt discovery
                           MAX2(ParallelGCThreads, ConcGCThreads),
                                // degree of mt discovery
                           false,
                                // Reference discovery is not atomic
                           &_is_alive_closure_cm);
                                // is alive closure
                                // (for efficiency/performance)

  // STW ref processor
  _ref_processor_stw =
    new ReferenceProcessor(mr,    // span
                           mt_processing,
                                // mt processing
                           ParallelGCThreads,
                                // degree of mt processing
                           (ParallelGCThreads > 1),
                                // mt discovery
                           ParallelGCThreads,
                                // degree of mt discovery
                           true,
                                // Reference discovery is atomic
                           &_is_alive_closure_stw);
                                // is alive closure
                                // (for efficiency/performance)
}

CollectorPolicy* G1CollectedHeap::collector_policy() const {
  return _collector_policy;
}

size_t G1CollectedHeap::capacity() const {
  return _hrm.length() * HeapRegion::GrainBytes;
}

size_t G1CollectedHeap::unused_committed_regions_in_bytes() const {
  return _hrm.total_free_bytes();
}

void G1CollectedHeap::reset_gc_time_stamps(HeapRegion* hr) {
  hr->reset_gc_time_stamp();
}

#ifndef PRODUCT

class CheckGCTimeStampsHRClosure : public HeapRegionClosure {
private:
  unsigned _gc_time_stamp;
  bool _failures;

public:
  CheckGCTimeStampsHRClosure(unsigned gc_time_stamp) :
    _gc_time_stamp(gc_time_stamp), _failures(false) { }

  virtual bool doHeapRegion(HeapRegion* hr) {
    unsigned region_gc_time_stamp = hr->get_gc_time_stamp();
    if (_gc_time_stamp != region_gc_time_stamp) {
      log_error(gc, verify)("Region " HR_FORMAT " has GC time stamp = %d, expected %d", HR_FORMAT_PARAMS(hr),
                            region_gc_time_stamp, _gc_time_stamp);
      _failures = true;
    }
    return false;
  }

  bool failures() { return _failures; }
};

void G1CollectedHeap::check_gc_time_stamps() {
  CheckGCTimeStampsHRClosure cl(_gc_time_stamp);
  heap_region_iterate(&cl);
  guarantee(!cl.failures(), "all GC time stamps should have been reset");
}
#endif // PRODUCT

void G1CollectedHeap::iterate_hcc_closure(CardTableEntryClosure* cl, uint worker_i) {
  _hot_card_cache->drain(cl, worker_i);
}

void G1CollectedHeap::iterate_dirty_card_closure(CardTableEntryClosure* cl, uint worker_i) {
  DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
  size_t n_completed_buffers = 0;
  while (dcqs.apply_closure_during_gc(cl, worker_i)) {
    n_completed_buffers++;
  }
  g1_policy()->phase_times()->record_thread_work_item(G1GCPhaseTimes::UpdateRS, worker_i, n_completed_buffers);
  dcqs.clear_n_completed_buffers();
  assert(!dcqs.completed_buffers_exist_dirty(), "Completed buffers exist!");
}

// Computes the sum of the storage used by the various regions.
size_t G1CollectedHeap::used() const {
  size_t result = _summary_bytes_used + _allocator->used_in_alloc_regions();
  if (_archive_allocator != NULL) {
    result += _archive_allocator->used();
  }
  return result;
}

size_t G1CollectedHeap::used_unlocked() const {
  return _summary_bytes_used;
}

class SumUsedClosure: public HeapRegionClosure {
  size_t _used;
public:
  SumUsedClosure() : _used(0) {}
  bool doHeapRegion(HeapRegion* r) {
    _used += r->used();
    return false;
  }
  size_t result() { return _used; }
};

size_t G1CollectedHeap::recalculate_used() const {
  double recalculate_used_start = os::elapsedTime();

  SumUsedClosure blk;
  heap_region_iterate(&blk);

  g1_policy()->phase_times()->record_evac_fail_recalc_used_time((os::elapsedTime() - recalculate_used_start) * 1000.0);
  return blk.result();
}

bool  G1CollectedHeap::is_user_requested_concurrent_full_gc(GCCause::Cause cause) {
  switch (cause) {
    case GCCause::_java_lang_system_gc:                 return ExplicitGCInvokesConcurrent;
    case GCCause::_dcmd_gc_run:                         return ExplicitGCInvokesConcurrent;
    case GCCause::_update_allocation_context_stats_inc: return true;
    case GCCause::_wb_conc_mark:                        return true;
    default :                                           return false;
  }
}

bool G1CollectedHeap::should_do_concurrent_full_gc(GCCause::Cause cause) {
  switch (cause) {
    case GCCause::_gc_locker:               return GCLockerInvokesConcurrent;
    case GCCause::_g1_humongous_allocation: return true;
    default:                                return is_user_requested_concurrent_full_gc(cause);
  }
}

#ifndef PRODUCT
void G1CollectedHeap::allocate_dummy_regions() {
  // Let's fill up most of the region
  size_t word_size = HeapRegion::GrainWords - 1024;
  // And as a result the region we'll allocate will be humongous.
  guarantee(is_humongous(word_size), "sanity");

  // _filler_array_max_size is set to humongous object threshold
  // but temporarily change it to use CollectedHeap::fill_with_object().
  SizeTFlagSetting fs(_filler_array_max_size, word_size);

  for (uintx i = 0; i < G1DummyRegionsPerGC; ++i) {
    // Let's use the existing mechanism for the allocation
    HeapWord* dummy_obj = humongous_obj_allocate(word_size,
                                                 AllocationContext::system());
    if (dummy_obj != NULL) {
      MemRegion mr(dummy_obj, word_size);
      CollectedHeap::fill_with_object(mr);
    } else {
      // If we can't allocate once, we probably cannot allocate
      // again. Let's get out of the loop.
      break;
    }
  }
}
#endif // !PRODUCT

void G1CollectedHeap::increment_old_marking_cycles_started() {
  assert(_old_marking_cycles_started == _old_marking_cycles_completed ||
         _old_marking_cycles_started == _old_marking_cycles_completed + 1,
         "Wrong marking cycle count (started: %d, completed: %d)",
         _old_marking_cycles_started, _old_marking_cycles_completed);

  _old_marking_cycles_started++;
}

void G1CollectedHeap::increment_old_marking_cycles_completed(bool concurrent) {
  MonitorLockerEx x(FullGCCount_lock, Mutex::_no_safepoint_check_flag);

  // We assume that if concurrent == true, then the caller is a
  // concurrent thread that was joined the Suspendible Thread
  // Set. If there's ever a cheap way to check this, we should add an
  // assert here.

  // Given that this method is called at the end of a Full GC or of a
  // concurrent cycle, and those can be nested (i.e., a Full GC can
  // interrupt a concurrent cycle), the number of full collections
  // completed should be either one (in the case where there was no
  // nesting) or two (when a Full GC interrupted a concurrent cycle)
  // behind the number of full collections started.

  // This is the case for the inner caller, i.e. a Full GC.
  assert(concurrent ||
         (_old_marking_cycles_started == _old_marking_cycles_completed + 1) ||
         (_old_marking_cycles_started == _old_marking_cycles_completed + 2),
         "for inner caller (Full GC): _old_marking_cycles_started = %u "
         "is inconsistent with _old_marking_cycles_completed = %u",
         _old_marking_cycles_started, _old_marking_cycles_completed);

  // This is the case for the outer caller, i.e. the concurrent cycle.
  assert(!concurrent ||
         (_old_marking_cycles_started == _old_marking_cycles_completed + 1),
         "for outer caller (concurrent cycle): "
         "_old_marking_cycles_started = %u "
         "is inconsistent with _old_marking_cycles_completed = %u",
         _old_marking_cycles_started, _old_marking_cycles_completed);

  _old_marking_cycles_completed += 1;

  // We need to clear the "in_progress" flag in the CM thread before
  // we wake up any waiters (especially when ExplicitInvokesConcurrent
  // is set) so that if a waiter requests another System.gc() it doesn't
  // incorrectly see that a marking cycle is still in progress.
  if (concurrent) {
    _cmThread->set_idle();
  }

  // This notify_all() will ensure that a thread that called
  // System.gc() with (with ExplicitGCInvokesConcurrent set or not)
  // and it's waiting for a full GC to finish will be woken up. It is
  // waiting in VM_G1IncCollectionPause::doit_epilogue().
  FullGCCount_lock->notify_all();
}

void G1CollectedHeap::collect(GCCause::Cause cause) {
  assert_heap_not_locked();

  uint gc_count_before;
  uint old_marking_count_before;
  uint full_gc_count_before;
  bool retry_gc;

  do {
    retry_gc = false;

    {
      MutexLocker ml(Heap_lock);

      // Read the GC count while holding the Heap_lock
      gc_count_before = total_collections();
      full_gc_count_before = total_full_collections();
      old_marking_count_before = _old_marking_cycles_started;
    }

    if (should_do_concurrent_full_gc(cause)) {
      // Schedule an initial-mark evacuation pause that will start a
      // concurrent cycle. We're setting word_size to 0 which means that
      // we are not requesting a post-GC allocation.
      VM_G1IncCollectionPause op(gc_count_before,
                                 0,     /* word_size */
                                 true,  /* should_initiate_conc_mark */
                                 g1_policy()->max_pause_time_ms(),
                                 cause);
      op.set_allocation_context(AllocationContext::current());

      VMThread::execute(&op);
      if (!op.pause_succeeded()) {
        if (old_marking_count_before == _old_marking_cycles_started) {
          retry_gc = op.should_retry_gc();
        } else {
          // A Full GC happened while we were trying to schedule the
          // initial-mark GC. No point in starting a new cycle given
          // that the whole heap was collected anyway.
        }

        if (retry_gc) {
          if (GCLocker::is_active_and_needs_gc()) {
            GCLocker::stall_until_clear();
          }
        }
      }
    } else {
      if (cause == GCCause::_gc_locker || cause == GCCause::_wb_young_gc
          DEBUG_ONLY(|| cause == GCCause::_scavenge_alot)) {

        // Schedule a standard evacuation pause. We're setting word_size
        // to 0 which means that we are not requesting a post-GC allocation.
        VM_G1IncCollectionPause op(gc_count_before,
                                   0,     /* word_size */
                                   false, /* should_initiate_conc_mark */
                                   g1_policy()->max_pause_time_ms(),
                                   cause);
        VMThread::execute(&op);
      } else {
        // Schedule a Full GC.
        VM_G1CollectFull op(gc_count_before, full_gc_count_before, cause);
        VMThread::execute(&op);
      }
    }
  } while (retry_gc);
}

bool G1CollectedHeap::is_in(const void* p) const {
  if (_hrm.reserved().contains(p)) {
    // Given that we know that p is in the reserved space,
    // heap_region_containing() should successfully
    // return the containing region.
    HeapRegion* hr = heap_region_containing(p);
    return hr->is_in(p);
  } else {
    return false;
  }
}

#ifdef ASSERT
bool G1CollectedHeap::is_in_exact(const void* p) const {
  bool contains = reserved_region().contains(p);
  bool available = _hrm.is_available(addr_to_region((HeapWord*)p));
  if (contains && available) {
    return true;
  } else {
    return false;
  }
}
#endif

// Iteration functions.

// Iterates an ObjectClosure over all objects within a HeapRegion.

class IterateObjectClosureRegionClosure: public HeapRegionClosure {
  ObjectClosure* _cl;
public:
  IterateObjectClosureRegionClosure(ObjectClosure* cl) : _cl(cl) {}
  bool doHeapRegion(HeapRegion* r) {
    if (!r->is_continues_humongous()) {
      r->object_iterate(_cl);
    }
    return false;
  }
};

void G1CollectedHeap::object_iterate(ObjectClosure* cl) {
  IterateObjectClosureRegionClosure blk(cl);
  heap_region_iterate(&blk);
}

void G1CollectedHeap::heap_region_iterate(HeapRegionClosure* cl) const {
  _hrm.iterate(cl);
}

void G1CollectedHeap::heap_region_par_iterate_from_worker_offset(HeapRegionClosure* cl,
                                                                 HeapRegionClaimer *hrclaimer,
                                                                 uint worker_id) const {
  _hrm.par_iterate(cl, hrclaimer, hrclaimer->offset_for_worker(worker_id));
}

void G1CollectedHeap::heap_region_par_iterate_from_start(HeapRegionClosure* cl,
                                                         HeapRegionClaimer *hrclaimer) const {
  _hrm.par_iterate(cl, hrclaimer, 0);
}

void G1CollectedHeap::collection_set_iterate(HeapRegionClosure* cl) {
  _collection_set.iterate(cl);
}

void G1CollectedHeap::collection_set_iterate_from(HeapRegionClosure *cl, uint worker_id) {
  _collection_set.iterate_from(cl, worker_id, workers()->active_workers());
}

HeapWord* G1CollectedHeap::block_start(const void* addr) const {
  HeapRegion* hr = heap_region_containing(addr);
  return hr->block_start(addr);
}

size_t G1CollectedHeap::block_size(const HeapWord* addr) const {
  HeapRegion* hr = heap_region_containing(addr);
  return hr->block_size(addr);
}

bool G1CollectedHeap::block_is_obj(const HeapWord* addr) const {
  HeapRegion* hr = heap_region_containing(addr);
  return hr->block_is_obj(addr);
}

bool G1CollectedHeap::supports_tlab_allocation() const {
  return true;
}

size_t G1CollectedHeap::tlab_capacity(Thread* ignored) const {
  return (_g1_policy->young_list_target_length() - _survivor.length()) * HeapRegion::GrainBytes;
}

size_t G1CollectedHeap::tlab_used(Thread* ignored) const {
  return _eden.length() * HeapRegion::GrainBytes;
}

// For G1 TLABs should not contain humongous objects, so the maximum TLAB size
// must be equal to the humongous object limit.
size_t G1CollectedHeap::max_tlab_size() const {
  return align_down(_humongous_object_threshold_in_words, MinObjAlignment);
}

size_t G1CollectedHeap::unsafe_max_tlab_alloc(Thread* ignored) const {
  AllocationContext_t context = AllocationContext::current();
  return _allocator->unsafe_max_tlab_alloc(context);
}

size_t G1CollectedHeap::max_capacity() const {
  return _hrm.reserved().byte_size();
}

jlong G1CollectedHeap::millis_since_last_gc() {
  // See the notes in GenCollectedHeap::millis_since_last_gc()
  // for more information about the implementation.
  jlong ret_val = (os::javaTimeNanos() / NANOSECS_PER_MILLISEC) -
    _g1_policy->collection_pause_end_millis();
  if (ret_val < 0) {
    log_warning(gc)("millis_since_last_gc() would return : " JLONG_FORMAT
      ". returning zero instead.", ret_val);
    return 0;
  }
  return ret_val;
}

void G1CollectedHeap::prepare_for_verify() {
  _verifier->prepare_for_verify();
}

void G1CollectedHeap::verify(VerifyOption vo) {
  _verifier->verify(vo);
}

bool G1CollectedHeap::supports_concurrent_phase_control() const {
  return true;
}

const char* const* G1CollectedHeap::concurrent_phases() const {
  return _cmThread->concurrent_phases();
}

bool G1CollectedHeap::request_concurrent_phase(const char* phase) {
  return _cmThread->request_concurrent_phase(phase);
}

class PrintRegionClosure: public HeapRegionClosure {
  outputStream* _st;
public:
  PrintRegionClosure(outputStream* st) : _st(st) {}
  bool doHeapRegion(HeapRegion* r) {
    r->print_on(_st);
    return false;
  }
};

bool G1CollectedHeap::is_obj_dead_cond(const oop obj,
                                       const HeapRegion* hr,
                                       const VerifyOption vo) const {
  switch (vo) {
  case VerifyOption_G1UsePrevMarking: return is_obj_dead(obj, hr);
  case VerifyOption_G1UseNextMarking: return is_obj_ill(obj, hr);
  case VerifyOption_G1UseFullMarking: return is_obj_dead_full(obj, hr);
  default:                            ShouldNotReachHere();
  }
  return false; // keep some compilers happy
}

bool G1CollectedHeap::is_obj_dead_cond(const oop obj,
                                       const VerifyOption vo) const {
  switch (vo) {
  case VerifyOption_G1UsePrevMarking: return is_obj_dead(obj);
  case VerifyOption_G1UseNextMarking: return is_obj_ill(obj);
  case VerifyOption_G1UseFullMarking: return is_obj_dead_full(obj);
  default:                            ShouldNotReachHere();
  }
  return false; // keep some compilers happy
}

void G1CollectedHeap::print_heap_regions() const {
  LogTarget(Trace, gc, heap, region) lt;
  if (lt.is_enabled()) {
    LogStream ls(lt);
    print_regions_on(&ls);
  }
}

void G1CollectedHeap::print_on(outputStream* st) const {
  st->print(" %-20s", "garbage-first heap");
  st->print(" total " SIZE_FORMAT "K, used " SIZE_FORMAT "K",
            capacity()/K, used_unlocked()/K);
  st->print(" [" PTR_FORMAT ", " PTR_FORMAT ")",
            p2i(_hrm.reserved().start()),
            p2i(_hrm.reserved().end()));
  st->cr();
  st->print("  region size " SIZE_FORMAT "K, ", HeapRegion::GrainBytes / K);
  uint young_regions = young_regions_count();
  st->print("%u young (" SIZE_FORMAT "K), ", young_regions,
            (size_t) young_regions * HeapRegion::GrainBytes / K);
  uint survivor_regions = survivor_regions_count();
  st->print("%u survivors (" SIZE_FORMAT "K)", survivor_regions,
            (size_t) survivor_regions * HeapRegion::GrainBytes / K);
  st->cr();
  MetaspaceAux::print_on(st);
}

void G1CollectedHeap::print_regions_on(outputStream* st) const {
  st->print_cr("Heap Regions: E=young(eden), S=young(survivor), O=old, "
               "HS=humongous(starts), HC=humongous(continues), "
               "CS=collection set, F=free, A=archive, TS=gc time stamp, "
               "AC=allocation context, "
               "TAMS=top-at-mark-start (previous, next)");
  PrintRegionClosure blk(st);
  heap_region_iterate(&blk);
}

void G1CollectedHeap::print_extended_on(outputStream* st) const {
  print_on(st);

  // Print the per-region information.
  print_regions_on(st);
}

void G1CollectedHeap::print_on_error(outputStream* st) const {
  this->CollectedHeap::print_on_error(st);

  if (_cm != NULL) {
    st->cr();
    _cm->print_on_error(st);
  }
}

void G1CollectedHeap::print_gc_threads_on(outputStream* st) const {
  workers()->print_worker_threads_on(st);
  _cmThread->print_on(st);
  st->cr();
  _cm->print_worker_threads_on(st);
  _cr->print_threads_on(st);
  _young_gen_sampling_thread->print_on(st);
  if (G1StringDedup::is_enabled()) {
    G1StringDedup::print_worker_threads_on(st);
  }
}

void G1CollectedHeap::gc_threads_do(ThreadClosure* tc) const {
  workers()->threads_do(tc);
  tc->do_thread(_cmThread);
  _cm->threads_do(tc);
  _cr->threads_do(tc);
  tc->do_thread(_young_gen_sampling_thread);
  if (G1StringDedup::is_enabled()) {
    G1StringDedup::threads_do(tc);
  }
}

void G1CollectedHeap::print_tracing_info() const {
  g1_rem_set()->print_summary_info();
  concurrent_mark()->print_summary_info();
}

#ifndef PRODUCT
// Helpful for debugging RSet issues.

class PrintRSetsClosure : public HeapRegionClosure {
private:
  const char* _msg;
  size_t _occupied_sum;

public:
  bool doHeapRegion(HeapRegion* r) {
    HeapRegionRemSet* hrrs = r->rem_set();
    size_t occupied = hrrs->occupied();
    _occupied_sum += occupied;

    tty->print_cr("Printing RSet for region " HR_FORMAT, HR_FORMAT_PARAMS(r));
    if (occupied == 0) {
      tty->print_cr("  RSet is empty");
    } else {
      hrrs->print();
    }
    tty->print_cr("----------");
    return false;
  }

  PrintRSetsClosure(const char* msg) : _msg(msg), _occupied_sum(0) {
    tty->cr();
    tty->print_cr("========================================");
    tty->print_cr("%s", msg);
    tty->cr();
  }

  ~PrintRSetsClosure() {
    tty->print_cr("Occupied Sum: " SIZE_FORMAT, _occupied_sum);
    tty->print_cr("========================================");
    tty->cr();
  }
};

void G1CollectedHeap::print_cset_rsets() {
  PrintRSetsClosure cl("Printing CSet RSets");
  collection_set_iterate(&cl);
}

void G1CollectedHeap::print_all_rsets() {
  PrintRSetsClosure cl("Printing All RSets");;
  heap_region_iterate(&cl);
}
#endif // PRODUCT

G1HeapSummary G1CollectedHeap::create_g1_heap_summary() {

  size_t eden_used_bytes = heap()->eden_regions_count() * HeapRegion::GrainBytes;
  size_t survivor_used_bytes = heap()->survivor_regions_count() * HeapRegion::GrainBytes;
  size_t heap_used = Heap_lock->owned_by_self() ? used() : used_unlocked();

  size_t eden_capacity_bytes =
    (g1_policy()->young_list_target_length() * HeapRegion::GrainBytes) - survivor_used_bytes;

  VirtualSpaceSummary heap_summary = create_heap_space_summary();
  return G1HeapSummary(heap_summary, heap_used, eden_used_bytes,
                       eden_capacity_bytes, survivor_used_bytes, num_regions());
}

G1EvacSummary G1CollectedHeap::create_g1_evac_summary(G1EvacStats* stats) {
  return G1EvacSummary(stats->allocated(), stats->wasted(), stats->undo_wasted(),
                       stats->unused(), stats->used(), stats->region_end_waste(),
                       stats->regions_filled(), stats->direct_allocated(),
                       stats->failure_used(), stats->failure_waste());
}

void G1CollectedHeap::trace_heap(GCWhen::Type when, const GCTracer* gc_tracer) {
  const G1HeapSummary& heap_summary = create_g1_heap_summary();
  gc_tracer->report_gc_heap_summary(when, heap_summary);

  const MetaspaceSummary& metaspace_summary = create_metaspace_summary();
  gc_tracer->report_metaspace_summary(when, metaspace_summary);
}

G1CollectedHeap* G1CollectedHeap::heap() {
  CollectedHeap* heap = Universe::heap();
  assert(heap != NULL, "Uninitialized access to G1CollectedHeap::heap()");
  assert(heap->kind() == CollectedHeap::G1CollectedHeap, "Not a G1CollectedHeap");
  return (G1CollectedHeap*)heap;
}

void G1CollectedHeap::gc_prologue(bool full) {
  // always_do_update_barrier = false;
  assert(InlineCacheBuffer::is_empty(), "should have cleaned up ICBuffer");

  // This summary needs to be printed before incrementing total collections.
  g1_rem_set()->print_periodic_summary_info("Before GC RS summary", total_collections());

  // Update common counters.
  increment_total_collections(full /* full gc */);
  if (full) {
    increment_old_marking_cycles_started();
    reset_gc_time_stamp();
  } else {
    increment_gc_time_stamp();
  }

  // Fill TLAB's and such
  double start = os::elapsedTime();
  accumulate_statistics_all_tlabs();
  ensure_parsability(true);
  g1_policy()->phase_times()->record_prepare_tlab_time_ms((os::elapsedTime() - start) * 1000.0);
}

void G1CollectedHeap::gc_epilogue(bool full) {
  // Update common counters.
  if (full) {
    // Update the number of full collections that have been completed.
    increment_old_marking_cycles_completed(false /* concurrent */);
  }

  // We are at the end of the GC. Total collections has already been increased.
  g1_rem_set()->print_periodic_summary_info("After GC RS summary", total_collections() - 1);

  // FIXME: what is this about?
  // I'm ignoring the "fill_newgen()" call if "alloc_event_enabled"
  // is set.
#if COMPILER2_OR_JVMCI
  assert(DerivedPointerTable::is_empty(), "derived pointer present");
#endif
  // always_do_update_barrier = true;

  double start = os::elapsedTime();
  resize_all_tlabs();
  g1_policy()->phase_times()->record_resize_tlab_time_ms((os::elapsedTime() - start) * 1000.0);

  allocation_context_stats().update(full);

  MemoryService::track_memory_usage();
  // We have just completed a GC. Update the soft reference
  // policy with the new heap occupancy
  Universe::update_heap_info_at_gc();
}

HeapWord* G1CollectedHeap::do_collection_pause(size_t word_size,
                                               uint gc_count_before,
                                               bool* succeeded,
                                               GCCause::Cause gc_cause) {
  assert_heap_not_locked_and_not_at_safepoint();
  VM_G1IncCollectionPause op(gc_count_before,
                             word_size,
                             false, /* should_initiate_conc_mark */
                             g1_policy()->max_pause_time_ms(),
                             gc_cause);

  op.set_allocation_context(AllocationContext::current());
  VMThread::execute(&op);

  HeapWord* result = op.result();
  bool ret_succeeded = op.prologue_succeeded() && op.pause_succeeded();
  assert(result == NULL || ret_succeeded,
         "the result should be NULL if the VM did not succeed");
  *succeeded = ret_succeeded;

  assert_heap_not_locked();
  return result;
}

void
G1CollectedHeap::doConcurrentMark() {
  MutexLockerEx x(CGC_lock, Mutex::_no_safepoint_check_flag);
  if (!_cmThread->in_progress()) {
    _cmThread->set_started();
    CGC_lock->notify();
  }
}

size_t G1CollectedHeap::pending_card_num() {
  size_t extra_cards = 0;
  JavaThread *curr = Threads::first();
  while (curr != NULL) {
    DirtyCardQueue& dcq = curr->dirty_card_queue();
    extra_cards += dcq.size();
    curr = curr->next();
  }
  DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
  size_t buffer_size = dcqs.buffer_size();
  size_t buffer_num = dcqs.completed_buffers_num();

  return buffer_size * buffer_num + extra_cards;
}

class RegisterHumongousWithInCSetFastTestClosure : public HeapRegionClosure {
 private:
  size_t _total_humongous;
  size_t _candidate_humongous;

  DirtyCardQueue _dcq;

  // We don't nominate objects with many remembered set entries, on
  // the assumption that such objects are likely still live.
  bool is_remset_small(HeapRegion* region) const {
    HeapRegionRemSet* const rset = region->rem_set();
    return G1EagerReclaimHumongousObjectsWithStaleRefs
      ? rset->occupancy_less_or_equal_than(G1RSetSparseRegionEntries)
      : rset->is_empty();
  }

  bool humongous_region_is_candidate(G1CollectedHeap* heap, HeapRegion* region) const {
    assert(region->is_starts_humongous(), "Must start a humongous object");

    oop obj = oop(region->bottom());

    // Dead objects cannot be eager reclaim candidates. Due to class
    // unloading it is unsafe to query their classes so we return early.
    if (heap->is_obj_dead(obj, region)) {
      return false;
    }

    // Candidate selection must satisfy the following constraints
    // while concurrent marking is in progress:
    //
    // * In order to maintain SATB invariants, an object must not be
    // reclaimed if it was allocated before the start of marking and
    // has not had its references scanned.  Such an object must have
    // its references (including type metadata) scanned to ensure no
    // live objects are missed by the marking process.  Objects
    // allocated after the start of concurrent marking don't need to
    // be scanned.
    //
    // * An object must not be reclaimed if it is on the concurrent
    // mark stack.  Objects allocated after the start of concurrent
    // marking are never pushed on the mark stack.
    //
    // Nominating only objects allocated after the start of concurrent
    // marking is sufficient to meet both constraints.  This may miss
    // some objects that satisfy the constraints, but the marking data
    // structures don't support efficiently performing the needed
    // additional tests or scrubbing of the mark stack.
    //
    // However, we presently only nominate is_typeArray() objects.
    // A humongous object containing references induces remembered
    // set entries on other regions.  In order to reclaim such an
    // object, those remembered sets would need to be cleaned up.
    //
    // We also treat is_typeArray() objects specially, allowing them
    // to be reclaimed even if allocated before the start of
    // concurrent mark.  For this we rely on mark stack insertion to
    // exclude is_typeArray() objects, preventing reclaiming an object
    // that is in the mark stack.  We also rely on the metadata for
    // such objects to be built-in and so ensured to be kept live.
    // Frequent allocation and drop of large binary blobs is an
    // important use case for eager reclaim, and this special handling
    // may reduce needed headroom.

    return obj->is_typeArray() && is_remset_small(region);
  }

 public:
  RegisterHumongousWithInCSetFastTestClosure()
  : _total_humongous(0),
    _candidate_humongous(0),
    _dcq(&JavaThread::dirty_card_queue_set()) {
  }

  virtual bool doHeapRegion(HeapRegion* r) {
    if (!r->is_starts_humongous()) {
      return false;
    }
    G1CollectedHeap* g1h = G1CollectedHeap::heap();

    bool is_candidate = humongous_region_is_candidate(g1h, r);
    uint rindex = r->hrm_index();
    g1h->set_humongous_reclaim_candidate(rindex, is_candidate);
    if (is_candidate) {
      _candidate_humongous++;
      g1h->register_humongous_region_with_cset(rindex);
      // Is_candidate already filters out humongous object with large remembered sets.
      // If we have a humongous object with a few remembered sets, we simply flush these
      // remembered set entries into the DCQS. That will result in automatic
      // re-evaluation of their remembered set entries during the following evacuation
      // phase.
      if (!r->rem_set()->is_empty()) {
        guarantee(r->rem_set()->occupancy_less_or_equal_than(G1RSetSparseRegionEntries),
                  "Found a not-small remembered set here. This is inconsistent with previous assumptions.");
        G1SATBCardTableLoggingModRefBS* bs = g1h->g1_barrier_set();
        HeapRegionRemSetIterator hrrs(r->rem_set());
        size_t card_index;
        while (hrrs.has_next(card_index)) {
          jbyte* card_ptr = (jbyte*)bs->byte_for_index(card_index);
          // The remembered set might contain references to already freed
          // regions. Filter out such entries to avoid failing card table
          // verification.
          if (g1h->is_in_closed_subset(bs->addr_for(card_ptr))) {
            if (*card_ptr != CardTableModRefBS::dirty_card_val()) {
              *card_ptr = CardTableModRefBS::dirty_card_val();
              _dcq.enqueue(card_ptr);
            }
          }
        }
        assert(hrrs.n_yielded() == r->rem_set()->occupied(),
               "Remembered set hash maps out of sync, cur: " SIZE_FORMAT " entries, next: " SIZE_FORMAT " entries",
               hrrs.n_yielded(), r->rem_set()->occupied());
        r->rem_set()->clear_locked();
      }
      assert(r->rem_set()->is_empty(), "At this point any humongous candidate remembered set must be empty.");
    }
    _total_humongous++;

    return false;
  }

  size_t total_humongous() const { return _total_humongous; }
  size_t candidate_humongous() const { return _candidate_humongous; }

  void flush_rem_set_entries() { _dcq.flush(); }
};

void G1CollectedHeap::register_humongous_regions_with_cset() {
  if (!G1EagerReclaimHumongousObjects) {
    g1_policy()->phase_times()->record_fast_reclaim_humongous_stats(0.0, 0, 0);
    return;
  }
  double time = os::elapsed_counter();

  // Collect reclaim candidate information and register candidates with cset.
  RegisterHumongousWithInCSetFastTestClosure cl;
  heap_region_iterate(&cl);

  time = ((double)(os::elapsed_counter() - time) / os::elapsed_frequency()) * 1000.0;
  g1_policy()->phase_times()->record_fast_reclaim_humongous_stats(time,
                                                                  cl.total_humongous(),
                                                                  cl.candidate_humongous());
  _has_humongous_reclaim_candidates = cl.candidate_humongous() > 0;

  // Finally flush all remembered set entries to re-check into the global DCQS.
  cl.flush_rem_set_entries();
}

class VerifyRegionRemSetClosure : public HeapRegionClosure {
  public:
    bool doHeapRegion(HeapRegion* hr) {
      if (!hr->is_archive() && !hr->is_continues_humongous()) {
        hr->verify_rem_set();
      }
      return false;
    }
};

uint G1CollectedHeap::num_task_queues() const {
  return _task_queues->size();
}

#if TASKQUEUE_STATS
void G1CollectedHeap::print_taskqueue_stats_hdr(outputStream* const st) {
  st->print_raw_cr("GC Task Stats");
  st->print_raw("thr "); TaskQueueStats::print_header(1, st); st->cr();
  st->print_raw("--- "); TaskQueueStats::print_header(2, st); st->cr();
}

void G1CollectedHeap::print_taskqueue_stats() const {
  if (!log_is_enabled(Trace, gc, task, stats)) {
    return;
  }
  Log(gc, task, stats) log;
  ResourceMark rm;
  LogStream ls(log.trace());
  outputStream* st = &ls;

  print_taskqueue_stats_hdr(st);

  TaskQueueStats totals;
  const uint n = num_task_queues();
  for (uint i = 0; i < n; ++i) {
    st->print("%3u ", i); task_queue(i)->stats.print(st); st->cr();
    totals += task_queue(i)->stats;
  }
  st->print_raw("tot "); totals.print(st); st->cr();

  DEBUG_ONLY(totals.verify());
}

void G1CollectedHeap::reset_taskqueue_stats() {
  const uint n = num_task_queues();
  for (uint i = 0; i < n; ++i) {
    task_queue(i)->stats.reset();
  }
}
#endif // TASKQUEUE_STATS

void G1CollectedHeap::wait_for_root_region_scanning() {
  double scan_wait_start = os::elapsedTime();
  // We have to wait until the CM threads finish scanning the
  // root regions as it's the only way to ensure that all the
  // objects on them have been correctly scanned before we start
  // moving them during the GC.
  bool waited = _cm->root_regions()->wait_until_scan_finished();
  double wait_time_ms = 0.0;
  if (waited) {
    double scan_wait_end = os::elapsedTime();
    wait_time_ms = (scan_wait_end - scan_wait_start) * 1000.0;
  }
  g1_policy()->phase_times()->record_root_region_scan_wait_time(wait_time_ms);
}

class G1PrintCollectionSetClosure : public HeapRegionClosure {
private:
  G1HRPrinter* _hr_printer;
public:
  G1PrintCollectionSetClosure(G1HRPrinter* hr_printer) : HeapRegionClosure(), _hr_printer(hr_printer) { }

  virtual bool doHeapRegion(HeapRegion* r) {
    _hr_printer->cset(r);
    return false;
  }
};

void G1CollectedHeap::start_new_collection_set() {
  collection_set()->start_incremental_building();

  clear_cset_fast_test();

  guarantee(_eden.length() == 0, "eden should have been cleared");
  g1_policy()->transfer_survivors_to_cset(survivor());
}

bool
G1CollectedHeap::do_collection_pause_at_safepoint(double target_pause_time_ms) {
  assert_at_safepoint(true /* should_be_vm_thread */);
  guarantee(!is_gc_active(), "collection is not reentrant");

  if (GCLocker::check_active_before_gc()) {
    return false;
  }

  _gc_timer_stw->register_gc_start();

  GCIdMark gc_id_mark;
  _gc_tracer_stw->report_gc_start(gc_cause(), _gc_timer_stw->gc_start());

  SvcGCMarker sgcm(SvcGCMarker::MINOR);
  ResourceMark rm;

  g1_policy()->note_gc_start();

  wait_for_root_region_scanning();

  print_heap_before_gc();
  print_heap_regions();
  trace_heap_before_gc(_gc_tracer_stw);

  _verifier->verify_region_sets_optional();
  _verifier->verify_dirty_young_regions();

  // We should not be doing initial mark unless the conc mark thread is running
  if (!_cmThread->should_terminate()) {
    // This call will decide whether this pause is an initial-mark
    // pause. If it is, during_initial_mark_pause() will return true
    // for the duration of this pause.
    g1_policy()->decide_on_conc_mark_initiation();
  }

  // We do not allow initial-mark to be piggy-backed on a mixed GC.
  assert(!collector_state()->during_initial_mark_pause() ||
          collector_state()->gcs_are_young(), "sanity");

  // We also do not allow mixed GCs during marking.
  assert(!collector_state()->mark_in_progress() || collector_state()->gcs_are_young(), "sanity");

  // Record whether this pause is an initial mark. When the current
  // thread has completed its logging output and it's safe to signal
  // the CM thread, the flag's value in the policy has been reset.
  bool should_start_conc_mark = collector_state()->during_initial_mark_pause();

  // Inner scope for scope based logging, timers, and stats collection
  {
    EvacuationInfo evacuation_info;

    if (collector_state()->during_initial_mark_pause()) {
      // We are about to start a marking cycle, so we increment the
      // full collection counter.
      increment_old_marking_cycles_started();
      _cm->gc_tracer_cm()->set_gc_cause(gc_cause());
    }

    _gc_tracer_stw->report_yc_type(collector_state()->yc_type());

    GCTraceCPUTime tcpu;

    FormatBuffer<> gc_string("Pause ");
    if (collector_state()->during_initial_mark_pause()) {
      gc_string.append("Initial Mark");
    } else if (collector_state()->gcs_are_young()) {
      gc_string.append("Young");
    } else {
      gc_string.append("Mixed");
    }
    GCTraceTime(Info, gc) tm(gc_string, NULL, gc_cause(), true);

    uint active_workers = AdaptiveSizePolicy::calc_active_workers(workers()->total_workers(),
                                                                  workers()->active_workers(),
                                                                  Threads::number_of_non_daemon_threads());
    workers()->update_active_workers(active_workers);
    log_info(gc,task)("Using %u workers of %u for evacuation", active_workers, workers()->total_workers());

    TraceCollectorStats tcs(g1mm()->incremental_collection_counters());
    TraceMemoryManagerStats tms(false /* fullGC */, gc_cause());

    // If the secondary_free_list is not empty, append it to the
    // free_list. No need to wait for the cleanup operation to finish;
    // the region allocation code will check the secondary_free_list
    // and wait if necessary. If the G1StressConcRegionFreeing flag is
    // set, skip this step so that the region allocation code has to
    // get entries from the secondary_free_list.
    if (!G1StressConcRegionFreeing) {
      append_secondary_free_list_if_not_empty_with_lock();
    }

    G1HeapTransition heap_transition(this);
    size_t heap_used_bytes_before_gc = used();

    // Don't dynamically change the number of GC threads this early.  A value of
    // 0 is used to indicate serial work.  When parallel work is done,
    // it will be set.

    { // Call to jvmpi::post_class_unload_events must occur outside of active GC
      IsGCActiveMark x;

      gc_prologue(false);

      if (VerifyRememberedSets) {
        log_info(gc, verify)("[Verifying RemSets before GC]");
        VerifyRegionRemSetClosure v_cl;
        heap_region_iterate(&v_cl);
      }

      _verifier->verify_before_gc();

      _verifier->check_bitmaps("GC Start");

#if COMPILER2_OR_JVMCI
      DerivedPointerTable::clear();
#endif

      // Please see comment in g1CollectedHeap.hpp and
      // G1CollectedHeap::ref_processing_init() to see how
      // reference processing currently works in G1.

      // Enable discovery in the STW reference processor
      if (g1_policy()->should_process_references()) {
        ref_processor_stw()->enable_discovery();
      } else {
        ref_processor_stw()->disable_discovery();
      }

      {
        // We want to temporarily turn off discovery by the
        // CM ref processor, if necessary, and turn it back on
        // on again later if we do. Using a scoped
        // NoRefDiscovery object will do this.
        NoRefDiscovery no_cm_discovery(ref_processor_cm());

        // Forget the current alloc region (we might even choose it to be part
        // of the collection set!).
        _allocator->release_mutator_alloc_region();

        // This timing is only used by the ergonomics to handle our pause target.
        // It is unclear why this should not include the full pause. We will
        // investigate this in CR 7178365.
        //
        // Preserving the old comment here if that helps the investigation:
        //
        // The elapsed time induced by the start time below deliberately elides
        // the possible verification above.
        double sample_start_time_sec = os::elapsedTime();

        g1_policy()->record_collection_pause_start(sample_start_time_sec);

        if (collector_state()->during_initial_mark_pause()) {
          concurrent_mark()->checkpoint_roots_initial_pre();
        }

        g1_policy()->finalize_collection_set(target_pause_time_ms, &_survivor);

        evacuation_info.set_collectionset_regions(collection_set()->region_length());

        // Make sure the remembered sets are up to date. This needs to be
        // done before register_humongous_regions_with_cset(), because the
        // remembered sets are used there to choose eager reclaim candidates.
        // If the remembered sets are not up to date we might miss some
        // entries that need to be handled.
        g1_rem_set()->cleanupHRRS();

        register_humongous_regions_with_cset();

        assert(_verifier->check_cset_fast_test(), "Inconsistency in the InCSetState table.");

        // We call this after finalize_cset() to
        // ensure that the CSet has been finalized.
        _cm->verify_no_cset_oops();

        if (_hr_printer.is_active()) {
          G1PrintCollectionSetClosure cl(&_hr_printer);
          _collection_set.iterate(&cl);
        }

        // Initialize the GC alloc regions.
        _allocator->init_gc_alloc_regions(evacuation_info);

        G1ParScanThreadStateSet per_thread_states(this, workers()->active_workers(), collection_set()->young_region_length());
        pre_evacuate_collection_set();

        // Actually do the work...
        evacuate_collection_set(evacuation_info, &per_thread_states);

        post_evacuate_collection_set(evacuation_info, &per_thread_states);

        const size_t* surviving_young_words = per_thread_states.surviving_young_words();
        free_collection_set(&_collection_set, evacuation_info, surviving_young_words);

        eagerly_reclaim_humongous_regions();

        record_obj_copy_mem_stats();
        _survivor_evac_stats.adjust_desired_plab_sz();
        _old_evac_stats.adjust_desired_plab_sz();

        double start = os::elapsedTime();
        start_new_collection_set();
        g1_policy()->phase_times()->record_start_new_cset_time_ms((os::elapsedTime() - start) * 1000.0);

        if (evacuation_failed()) {
          set_used(recalculate_used());
          if (_archive_allocator != NULL) {
            _archive_allocator->clear_used();
          }
          for (uint i = 0; i < ParallelGCThreads; i++) {
            if (_evacuation_failed_info_array[i].has_failed()) {
              _gc_tracer_stw->report_evacuation_failed(_evacuation_failed_info_array[i]);
            }
          }
        } else {
          // The "used" of the the collection set have already been subtracted
          // when they were freed.  Add in the bytes evacuated.
          increase_used(g1_policy()->bytes_copied_during_gc());
        }

        if (collector_state()->during_initial_mark_pause()) {
          // We have to do this before we notify the CM threads that
          // they can start working to make sure that all the
          // appropriate initialization is done on the CM object.
          concurrent_mark()->checkpoint_roots_initial_post();
          collector_state()->set_mark_in_progress(true);
          // Note that we don't actually trigger the CM thread at
          // this point. We do that later when we're sure that
          // the current thread has completed its logging output.
        }

        allocate_dummy_regions();

        _allocator->init_mutator_alloc_region();

        {
          size_t expand_bytes = _heap_sizing_policy->expansion_amount();
          if (expand_bytes > 0) {
            size_t bytes_before = capacity();
            // No need for an ergo logging here,
            // expansion_amount() does this when it returns a value > 0.
            double expand_ms;
            if (!expand(expand_bytes, _workers, &expand_ms)) {
              // We failed to expand the heap. Cannot do anything about it.
            }
            g1_policy()->phase_times()->record_expand_heap_time(expand_ms);
          }
        }

        // We redo the verification but now wrt to the new CSet which
        // has just got initialized after the previous CSet was freed.
        _cm->verify_no_cset_oops();

        // This timing is only used by the ergonomics to handle our pause target.
        // It is unclear why this should not include the full pause. We will
        // investigate this in CR 7178365.
        double sample_end_time_sec = os::elapsedTime();
        double pause_time_ms = (sample_end_time_sec - sample_start_time_sec) * MILLIUNITS;
        size_t total_cards_scanned = g1_policy()->phase_times()->sum_thread_work_items(G1GCPhaseTimes::ScanRS, G1GCPhaseTimes::ScanRSScannedCards);
        g1_policy()->record_collection_pause_end(pause_time_ms, total_cards_scanned, heap_used_bytes_before_gc);

        evacuation_info.set_collectionset_used_before(collection_set()->bytes_used_before());
        evacuation_info.set_bytes_copied(g1_policy()->bytes_copied_during_gc());

        if (VerifyRememberedSets) {
          log_info(gc, verify)("[Verifying RemSets after GC]");
          VerifyRegionRemSetClosure v_cl;
          heap_region_iterate(&v_cl);
        }

        _verifier->verify_after_gc();
        _verifier->check_bitmaps("GC End");

        assert(!ref_processor_stw()->discovery_enabled(), "Postcondition");
        ref_processor_stw()->verify_no_references_recorded();

        // CM reference discovery will be re-enabled if necessary.
      }

#ifdef TRACESPINNING
      ParallelTaskTerminator::print_termination_counts();
#endif

      gc_epilogue(false);
    }

    // Print the remainder of the GC log output.
    if (evacuation_failed()) {
      log_info(gc)("To-space exhausted");
    }

    g1_policy()->print_phases();
    heap_transition.print();

    // It is not yet to safe to tell the concurrent mark to
    // start as we have some optional output below. We don't want the
    // output from the concurrent mark thread interfering with this
    // logging output either.

    _hrm.verify_optional();
    _verifier->verify_region_sets_optional();

    TASKQUEUE_STATS_ONLY(print_taskqueue_stats());
    TASKQUEUE_STATS_ONLY(reset_taskqueue_stats());

    print_heap_after_gc();
    print_heap_regions();
    trace_heap_after_gc(_gc_tracer_stw);

    // We must call G1MonitoringSupport::update_sizes() in the same scoping level
    // as an active TraceMemoryManagerStats object (i.e. before the destructor for the
    // TraceMemoryManagerStats is called) so that the G1 memory pools are updated
    // before any GC notifications are raised.
    g1mm()->update_sizes();

    _gc_tracer_stw->report_evacuation_info(&evacuation_info);
    _gc_tracer_stw->report_tenuring_threshold(_g1_policy->tenuring_threshold());
    _gc_timer_stw->register_gc_end();
    _gc_tracer_stw->report_gc_end(_gc_timer_stw->gc_end(), _gc_timer_stw->time_partitions());
  }
  // It should now be safe to tell the concurrent mark thread to start
  // without its logging output interfering with the logging output
  // that came from the pause.

  if (should_start_conc_mark) {
    // CAUTION: after the doConcurrentMark() call below,
    // the concurrent marking thread(s) could be running
    // concurrently with us. Make sure that anything after
    // this point does not assume that we are the only GC thread
    // running. Note: of course, the actual marking work will
    // not start until the safepoint itself is released in
    // SuspendibleThreadSet::desynchronize().
    doConcurrentMark();
  }

  return true;
}

void G1CollectedHeap::remove_self_forwarding_pointers() {
  G1ParRemoveSelfForwardPtrsTask rsfp_task;
  workers()->run_task(&rsfp_task);
}

void G1CollectedHeap::restore_after_evac_failure() {
  double remove_self_forwards_start = os::elapsedTime();

  remove_self_forwarding_pointers();
  SharedRestorePreservedMarksTaskExecutor task_executor(workers());
  _preserved_marks_set.restore(&task_executor);

  g1_policy()->phase_times()->record_evac_fail_remove_self_forwards((os::elapsedTime() - remove_self_forwards_start) * 1000.0);
}

void G1CollectedHeap::preserve_mark_during_evac_failure(uint worker_id, oop obj, markOop m) {
  if (!_evacuation_failed) {
    _evacuation_failed = true;
  }

  _evacuation_failed_info_array[worker_id].register_copy_failure(obj->size());
  _preserved_marks_set.get(worker_id)->push_if_necessary(obj, m);
}

bool G1ParEvacuateFollowersClosure::offer_termination() {
  G1ParScanThreadState* const pss = par_scan_state();
  start_term_time();
  const bool res = terminator()->offer_termination();
  end_term_time();
  return res;
}

void G1ParEvacuateFollowersClosure::do_void() {
  G1ParScanThreadState* const pss = par_scan_state();
  pss->trim_queue();
  do {
    pss->steal_and_trim_queue(queues());
  } while (!offer_termination());
}

class G1ParTask : public AbstractGangTask {
protected:
  G1CollectedHeap*         _g1h;
  G1ParScanThreadStateSet* _pss;
  RefToScanQueueSet*       _queues;
  G1RootProcessor*         _root_processor;
  ParallelTaskTerminator   _terminator;
  uint                     _n_workers;

public:
  G1ParTask(G1CollectedHeap* g1h, G1ParScanThreadStateSet* per_thread_states, RefToScanQueueSet *task_queues, G1RootProcessor* root_processor, uint n_workers)
    : AbstractGangTask("G1 collection"),
      _g1h(g1h),
      _pss(per_thread_states),
      _queues(task_queues),
      _root_processor(root_processor),
      _terminator(n_workers, _queues),
      _n_workers(n_workers)
  {}

  void work(uint worker_id) {
    if (worker_id >= _n_workers) return;  // no work needed this round

    double start_sec = os::elapsedTime();
    _g1h->g1_policy()->phase_times()->record_time_secs(G1GCPhaseTimes::GCWorkerStart, worker_id, start_sec);

    {
      ResourceMark rm;
      HandleMark   hm;

      ReferenceProcessor*             rp = _g1h->ref_processor_stw();

      G1ParScanThreadState*           pss = _pss->state_for_worker(worker_id);
      pss->set_ref_processor(rp);

      double start_strong_roots_sec = os::elapsedTime();

      _root_processor->evacuate_roots(pss->closures(), worker_id);

      // We pass a weak code blobs closure to the remembered set scanning because we want to avoid
      // treating the nmethods visited to act as roots for concurrent marking.
      // We only want to make sure that the oops in the nmethods are adjusted with regard to the
      // objects copied by the current evacuation.
      _g1h->g1_rem_set()->oops_into_collection_set_do(pss,
                                                      pss->closures()->weak_codeblobs(),
                                                      worker_id);

      double strong_roots_sec = os::elapsedTime() - start_strong_roots_sec;

      double term_sec = 0.0;
      size_t evac_term_attempts = 0;
      {
        double start = os::elapsedTime();
        G1ParEvacuateFollowersClosure evac(_g1h, pss, _queues, &_terminator);
        evac.do_void();

        evac_term_attempts = evac.term_attempts();
        term_sec = evac.term_time();
        double elapsed_sec = os::elapsedTime() - start;
        _g1h->g1_policy()->phase_times()->add_time_secs(G1GCPhaseTimes::ObjCopy, worker_id, elapsed_sec - term_sec);
        _g1h->g1_policy()->phase_times()->record_time_secs(G1GCPhaseTimes::Termination, worker_id, term_sec);
        _g1h->g1_policy()->phase_times()->record_thread_work_item(G1GCPhaseTimes::Termination, worker_id, evac_term_attempts);
      }

      assert(pss->queue_is_empty(), "should be empty");

      if (log_is_enabled(Debug, gc, task, stats)) {
        MutexLockerEx x(ParGCRareEvent_lock, Mutex::_no_safepoint_check_flag);
        size_t lab_waste;
        size_t lab_undo_waste;
        pss->waste(lab_waste, lab_undo_waste);
        _g1h->print_termination_stats(worker_id,
                                      (os::elapsedTime() - start_sec) * 1000.0,   /* elapsed time */
                                      strong_roots_sec * 1000.0,                  /* strong roots time */
                                      term_sec * 1000.0,                          /* evac term time */
                                      evac_term_attempts,                         /* evac term attempts */
                                      lab_waste,                                  /* alloc buffer waste */
                                      lab_undo_waste                              /* undo waste */
                                      );
      }

      // Close the inner scope so that the ResourceMark and HandleMark
      // destructors are executed here and are included as part of the
      // "GC Worker Time".
    }
    _g1h->g1_policy()->phase_times()->record_time_secs(G1GCPhaseTimes::GCWorkerEnd, worker_id, os::elapsedTime());
  }
};

void G1CollectedHeap::print_termination_stats_hdr() {
  log_debug(gc, task, stats)("GC Termination Stats");
  log_debug(gc, task, stats)("     elapsed  --strong roots-- -------termination------- ------waste (KiB)------");
  log_debug(gc, task, stats)("thr     ms        ms      %%        ms      %%    attempts  total   alloc    undo");
  log_debug(gc, task, stats)("--- --------- --------- ------ --------- ------ -------- ------- ------- -------");
}

void G1CollectedHeap::print_termination_stats(uint worker_id,
                                              double elapsed_ms,
                                              double strong_roots_ms,
                                              double term_ms,
                                              size_t term_attempts,
                                              size_t alloc_buffer_waste,
                                              size_t undo_waste) const {
  log_debug(gc, task, stats)
              ("%3d %9.2f %9.2f %6.2f "
               "%9.2f %6.2f " SIZE_FORMAT_W(8) " "
               SIZE_FORMAT_W(7) " " SIZE_FORMAT_W(7) " " SIZE_FORMAT_W(7),
               worker_id, elapsed_ms, strong_roots_ms, strong_roots_ms * 100 / elapsed_ms,
               term_ms, term_ms * 100 / elapsed_ms, term_attempts,
               (alloc_buffer_waste + undo_waste) * HeapWordSize / K,
               alloc_buffer_waste * HeapWordSize / K,
               undo_waste * HeapWordSize / K);
}

class G1StringAndSymbolCleaningTask : public AbstractGangTask {
private:
  BoolObjectClosure* _is_alive;
  G1StringDedupUnlinkOrOopsDoClosure _dedup_closure;

  int _initial_string_table_size;
  int _initial_symbol_table_size;

  bool  _process_strings;
  int _strings_processed;
  int _strings_removed;

  bool  _process_symbols;
  int _symbols_processed;
  int _symbols_removed;

  bool _process_string_dedup;

public:
  G1StringAndSymbolCleaningTask(BoolObjectClosure* is_alive, bool process_strings, bool process_symbols, bool process_string_dedup) :
    AbstractGangTask("String/Symbol Unlinking"),
    _is_alive(is_alive),
    _dedup_closure(is_alive, NULL, false),
    _process_strings(process_strings), _strings_processed(0), _strings_removed(0),
    _process_symbols(process_symbols), _symbols_processed(0), _symbols_removed(0),
    _process_string_dedup(process_string_dedup) {

    _initial_string_table_size = StringTable::the_table()->table_size();
    _initial_symbol_table_size = SymbolTable::the_table()->table_size();
    if (process_strings) {
      StringTable::clear_parallel_claimed_index();
    }
    if (process_symbols) {
      SymbolTable::clear_parallel_claimed_index();
    }
  }

  ~G1StringAndSymbolCleaningTask() {
    guarantee(!_process_strings || StringTable::parallel_claimed_index() >= _initial_string_table_size,
              "claim value %d after unlink less than initial string table size %d",
              StringTable::parallel_claimed_index(), _initial_string_table_size);
    guarantee(!_process_symbols || SymbolTable::parallel_claimed_index() >= _initial_symbol_table_size,
              "claim value %d after unlink less than initial symbol table size %d",
              SymbolTable::parallel_claimed_index(), _initial_symbol_table_size);

    log_info(gc, stringtable)(
        "Cleaned string and symbol table, "
        "strings: " SIZE_FORMAT " processed, " SIZE_FORMAT " removed, "
        "symbols: " SIZE_FORMAT " processed, " SIZE_FORMAT " removed",
        strings_processed(), strings_removed(),
        symbols_processed(), symbols_removed());
  }

  void work(uint worker_id) {
    int strings_processed = 0;
    int strings_removed = 0;
    int symbols_processed = 0;
    int symbols_removed = 0;
    if (_process_strings) {
      StringTable::possibly_parallel_unlink(_is_alive, &strings_processed, &strings_removed);
      Atomic::add(strings_processed, &_strings_processed);
      Atomic::add(strings_removed, &_strings_removed);
    }
    if (_process_symbols) {
      SymbolTable::possibly_parallel_unlink(&symbols_processed, &symbols_removed);
      Atomic::add(symbols_processed, &_symbols_processed);
      Atomic::add(symbols_removed, &_symbols_removed);
    }
    if (_process_string_dedup) {
      G1StringDedup::parallel_unlink(&_dedup_closure, worker_id);
    }
  }

  size_t strings_processed() const { return (size_t)_strings_processed; }
  size_t strings_removed()   const { return (size_t)_strings_removed; }

  size_t symbols_processed() const { return (size_t)_symbols_processed; }
  size_t symbols_removed()   const { return (size_t)_symbols_removed; }
};

class G1CodeCacheUnloadingTask VALUE_OBJ_CLASS_SPEC {
private:
  static Monitor* _lock;

  BoolObjectClosure* const _is_alive;
  const bool               _unloading_occurred;
  const uint               _num_workers;

  // Variables used to claim nmethods.
  CompiledMethod* _first_nmethod;
  CompiledMethod* volatile _claimed_nmethod;

  // The list of nmethods that need to be processed by the second pass.
  CompiledMethod* volatile _postponed_list;
  volatile uint            _num_entered_barrier;

 public:
  G1CodeCacheUnloadingTask(uint num_workers, BoolObjectClosure* is_alive, bool unloading_occurred) :
      _is_alive(is_alive),
      _unloading_occurred(unloading_occurred),
      _num_workers(num_workers),
      _first_nmethod(NULL),
      _claimed_nmethod(NULL),
      _postponed_list(NULL),
      _num_entered_barrier(0)
  {
    CompiledMethod::increase_unloading_clock();
    // Get first alive nmethod
    CompiledMethodIterator iter = CompiledMethodIterator();
    if(iter.next_alive()) {
      _first_nmethod = iter.method();
    }
    _claimed_nmethod = _first_nmethod;
  }

  ~G1CodeCacheUnloadingTask() {
    CodeCache::verify_clean_inline_caches();

    CodeCache::set_needs_cache_clean(false);
    guarantee(CodeCache::scavenge_root_nmethods() == NULL, "Must be");

    CodeCache::verify_icholder_relocations();
  }

 private:
  void add_to_postponed_list(CompiledMethod* nm) {
      CompiledMethod* old;
      do {
        old = _postponed_list;
        nm->set_unloading_next(old);
      } while (Atomic::cmpxchg(nm, &_postponed_list, old) != old);
  }

  void clean_nmethod(CompiledMethod* nm) {
    bool postponed = nm->do_unloading_parallel(_is_alive, _unloading_occurred);

    if (postponed) {
      // This nmethod referred to an nmethod that has not been cleaned/unloaded yet.
      add_to_postponed_list(nm);
    }

    // Mark that this thread has been cleaned/unloaded.
    // After this call, it will be safe to ask if this nmethod was unloaded or not.
    nm->set_unloading_clock(CompiledMethod::global_unloading_clock());
  }

  void clean_nmethod_postponed(CompiledMethod* nm) {
    nm->do_unloading_parallel_postponed(_is_alive, _unloading_occurred);
  }

  static const int MaxClaimNmethods = 16;

  void claim_nmethods(CompiledMethod** claimed_nmethods, int *num_claimed_nmethods) {
    CompiledMethod* first;
    CompiledMethodIterator last;

    do {
      *num_claimed_nmethods = 0;

      first = _claimed_nmethod;
      last = CompiledMethodIterator(first);

      if (first != NULL) {

        for (int i = 0; i < MaxClaimNmethods; i++) {
          if (!last.next_alive()) {
            break;
          }
          claimed_nmethods[i] = last.method();
          (*num_claimed_nmethods)++;
        }
      }

    } while (Atomic::cmpxchg(last.method(), &_claimed_nmethod, first) != first);
  }

  CompiledMethod* claim_postponed_nmethod() {
    CompiledMethod* claim;
    CompiledMethod* next;

    do {
      claim = _postponed_list;
      if (claim == NULL) {
        return NULL;
      }

      next = claim->unloading_next();

    } while (Atomic::cmpxchg(next, &_postponed_list, claim) != claim);

    return claim;
  }

 public:
  // Mark that we're done with the first pass of nmethod cleaning.
  void barrier_mark(uint worker_id) {
    MonitorLockerEx ml(_lock, Mutex::_no_safepoint_check_flag);
    _num_entered_barrier++;
    if (_num_entered_barrier == _num_workers) {
      ml.notify_all();
    }
  }

  // See if we have to wait for the other workers to
  // finish their first-pass nmethod cleaning work.
  void barrier_wait(uint worker_id) {
    if (_num_entered_barrier < _num_workers) {
      MonitorLockerEx ml(_lock, Mutex::_no_safepoint_check_flag);
      while (_num_entered_barrier < _num_workers) {
          ml.wait(Mutex::_no_safepoint_check_flag, 0, false);
      }
    }
  }

  // Cleaning and unloading of nmethods. Some work has to be postponed
  // to the second pass, when we know which nmethods survive.
  void work_first_pass(uint worker_id) {
    // The first nmethods is claimed by the first worker.
    if (worker_id == 0 && _first_nmethod != NULL) {
      clean_nmethod(_first_nmethod);
      _first_nmethod = NULL;
    }

    int num_claimed_nmethods;
    CompiledMethod* claimed_nmethods[MaxClaimNmethods];

    while (true) {
      claim_nmethods(claimed_nmethods, &num_claimed_nmethods);

      if (num_claimed_nmethods == 0) {
        break;
      }

      for (int i = 0; i < num_claimed_nmethods; i++) {
        clean_nmethod(claimed_nmethods[i]);
      }
    }
  }

  void work_second_pass(uint worker_id) {
    CompiledMethod* nm;
    // Take care of postponed nmethods.
    while ((nm = claim_postponed_nmethod()) != NULL) {
      clean_nmethod_postponed(nm);
    }
  }
};

Monitor* G1CodeCacheUnloadingTask::_lock = new Monitor(Mutex::leaf, "Code Cache Unload lock", false, Monitor::_safepoint_check_never);

class G1KlassCleaningTask : public StackObj {
  BoolObjectClosure*                      _is_alive;
  volatile int                            _clean_klass_tree_claimed;
  ClassLoaderDataGraphKlassIteratorAtomic _klass_iterator;

 public:
  G1KlassCleaningTask(BoolObjectClosure* is_alive) :
      _is_alive(is_alive),
      _clean_klass_tree_claimed(0),
      _klass_iterator() {
  }

 private:
  bool claim_clean_klass_tree_task() {
    if (_clean_klass_tree_claimed) {
      return false;
    }

    return Atomic::cmpxchg(1, &_clean_klass_tree_claimed, 0) == 0;
  }

  InstanceKlass* claim_next_klass() {
    Klass* klass;
    do {
      klass =_klass_iterator.next_klass();
    } while (klass != NULL && !klass->is_instance_klass());

    // this can be null so don't call InstanceKlass::cast
    return static_cast<InstanceKlass*>(klass);
  }

public:

  void clean_klass(InstanceKlass* ik) {
    ik->clean_weak_instanceklass_links(_is_alive);
  }

  void work() {
    ResourceMark rm;

    // One worker will clean the subklass/sibling klass tree.
    if (claim_clean_klass_tree_task()) {
      Klass::clean_subklass_tree(_is_alive);
    }

    // All workers will help cleaning the classes,
    InstanceKlass* klass;
    while ((klass = claim_next_klass()) != NULL) {
      clean_klass(klass);
    }
  }
};

class G1ResolvedMethodCleaningTask : public StackObj {
  BoolObjectClosure* _is_alive;
  volatile int       _resolved_method_task_claimed;
public:
  G1ResolvedMethodCleaningTask(BoolObjectClosure* is_alive) :
      _is_alive(is_alive), _resolved_method_task_claimed(0) {}

  bool claim_resolved_method_task() {
    if (_resolved_method_task_claimed) {
      return false;
    }
    return Atomic::cmpxchg(1, &_resolved_method_task_claimed, 0) == 0;
  }

  // These aren't big, one thread can do it all.
  void work() {
    if (claim_resolved_method_task()) {
      ResolvedMethodTable::unlink(_is_alive);
    }
  }
};


// To minimize the remark pause times, the tasks below are done in parallel.
class G1ParallelCleaningTask : public AbstractGangTask {
private:
  G1StringAndSymbolCleaningTask _string_symbol_task;
  G1CodeCacheUnloadingTask      _code_cache_task;
  G1KlassCleaningTask           _klass_cleaning_task;
  G1ResolvedMethodCleaningTask  _resolved_method_cleaning_task;

public:
  // The constructor is run in the VMThread.
  G1ParallelCleaningTask(BoolObjectClosure* is_alive, uint num_workers, bool unloading_occurred) :
      AbstractGangTask("Parallel Cleaning"),
      _string_symbol_task(is_alive, true, true, G1StringDedup::is_enabled()),
      _code_cache_task(num_workers, is_alive, unloading_occurred),
      _klass_cleaning_task(is_alive),
      _resolved_method_cleaning_task(is_alive) {
  }

  // The parallel work done by all worker threads.
  void work(uint worker_id) {
    // Do first pass of code cache cleaning.
    _code_cache_task.work_first_pass(worker_id);

    // Let the threads mark that the first pass is done.
    _code_cache_task.barrier_mark(worker_id);

    // Clean the Strings and Symbols.
    _string_symbol_task.work(worker_id);

    // Clean unreferenced things in the ResolvedMethodTable
    _resolved_method_cleaning_task.work();

    // Wait for all workers to finish the first code cache cleaning pass.
    _code_cache_task.barrier_wait(worker_id);

    // Do the second code cache cleaning work, which realize on
    // the liveness information gathered during the first pass.
    _code_cache_task.work_second_pass(worker_id);

    // Clean all klasses that were not unloaded.
    _klass_cleaning_task.work();
  }
};


void G1CollectedHeap::complete_cleaning(BoolObjectClosure* is_alive,
                                        bool class_unloading_occurred) {
  uint n_workers = workers()->active_workers();

  G1ParallelCleaningTask g1_unlink_task(is_alive, n_workers, class_unloading_occurred);
  workers()->run_task(&g1_unlink_task);
}

void G1CollectedHeap::partial_cleaning(BoolObjectClosure* is_alive,
                                       bool process_strings,
                                       bool process_symbols,
                                       bool process_string_dedup) {
  if (!process_strings && !process_symbols && !process_string_dedup) {
    // Nothing to clean.
    return;
  }

  G1StringAndSymbolCleaningTask g1_unlink_task(is_alive, process_strings, process_symbols, process_string_dedup);
  workers()->run_task(&g1_unlink_task);

}

class G1RedirtyLoggedCardsTask : public AbstractGangTask {
 private:
  DirtyCardQueueSet* _queue;
  G1CollectedHeap* _g1h;
 public:
  G1RedirtyLoggedCardsTask(DirtyCardQueueSet* queue, G1CollectedHeap* g1h) : AbstractGangTask("Redirty Cards"),
    _queue(queue), _g1h(g1h) { }

  virtual void work(uint worker_id) {
    G1GCPhaseTimes* phase_times = _g1h->g1_policy()->phase_times();
    G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::RedirtyCards, worker_id);

    RedirtyLoggedCardTableEntryClosure cl(_g1h);
    _queue->par_apply_closure_to_all_completed_buffers(&cl);

    phase_times->record_thread_work_item(G1GCPhaseTimes::RedirtyCards, worker_id, cl.num_dirtied());
  }
};

void G1CollectedHeap::redirty_logged_cards() {
  double redirty_logged_cards_start = os::elapsedTime();

  G1RedirtyLoggedCardsTask redirty_task(&dirty_card_queue_set(), this);
  dirty_card_queue_set().reset_for_par_iteration();
  workers()->run_task(&redirty_task);

  DirtyCardQueueSet& dcq = JavaThread::dirty_card_queue_set();
  dcq.merge_bufferlists(&dirty_card_queue_set());
  assert(dirty_card_queue_set().completed_buffers_num() == 0, "All should be consumed");

  g1_policy()->phase_times()->record_redirty_logged_cards_time_ms((os::elapsedTime() - redirty_logged_cards_start) * 1000.0);
}

// Weak Reference Processing support

// An always "is_alive" closure that is used to preserve referents.
// If the object is non-null then it's alive.  Used in the preservation
// of referent objects that are pointed to by reference objects
// discovered by the CM ref processor.
class G1AlwaysAliveClosure: public BoolObjectClosure {
  G1CollectedHeap* _g1;
public:
  G1AlwaysAliveClosure(G1CollectedHeap* g1) : _g1(g1) {}
  bool do_object_b(oop p) {
    if (p != NULL) {
      return true;
    }
    return false;
  }
};

bool G1STWIsAliveClosure::do_object_b(oop p) {
  // An object is reachable if it is outside the collection set,
  // or is inside and copied.
  return !_g1->is_in_cset(p) || p->is_forwarded();
}

// Non Copying Keep Alive closure
class G1KeepAliveClosure: public OopClosure {
  G1CollectedHeap* _g1;
public:
  G1KeepAliveClosure(G1CollectedHeap* g1) : _g1(g1) {}
  void do_oop(narrowOop* p) { guarantee(false, "Not needed"); }
  void do_oop(oop* p) {
    oop obj = *p;
    assert(obj != NULL, "the caller should have filtered out NULL values");

    const InCSetState cset_state = _g1->in_cset_state(obj);
    if (!cset_state.is_in_cset_or_humongous()) {
      return;
    }
    if (cset_state.is_in_cset()) {
      assert( obj->is_forwarded(), "invariant" );
      *p = obj->forwardee();
    } else {
      assert(!obj->is_forwarded(), "invariant" );
      assert(cset_state.is_humongous(),
             "Only allowed InCSet state is IsHumongous, but is %d", cset_state.value());
      _g1->set_humongous_is_live(obj);
    }
  }
};

// Copying Keep Alive closure - can be called from both
// serial and parallel code as long as different worker
// threads utilize different G1ParScanThreadState instances
// and different queues.

class G1CopyingKeepAliveClosure: public OopClosure {
  G1CollectedHeap*         _g1h;
  OopClosure*              _copy_non_heap_obj_cl;
  G1ParScanThreadState*    _par_scan_state;

public:
  G1CopyingKeepAliveClosure(G1CollectedHeap* g1h,
                            OopClosure* non_heap_obj_cl,
                            G1ParScanThreadState* pss):
    _g1h(g1h),
    _copy_non_heap_obj_cl(non_heap_obj_cl),
    _par_scan_state(pss)
  {}

  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
  virtual void do_oop(      oop* p) { do_oop_work(p); }

  template <class T> void do_oop_work(T* p) {
    oop obj = oopDesc::load_decode_heap_oop(p);

    if (_g1h->is_in_cset_or_humongous(obj)) {
      // If the referent object has been forwarded (either copied
      // to a new location or to itself in the event of an
      // evacuation failure) then we need to update the reference
      // field and, if both reference and referent are in the G1
      // heap, update the RSet for the referent.
      //
      // If the referent has not been forwarded then we have to keep
      // it alive by policy. Therefore we have copy the referent.
      //
      // If the reference field is in the G1 heap then we can push
      // on the PSS queue. When the queue is drained (after each
      // phase of reference processing) the object and it's followers
      // will be copied, the reference field set to point to the
      // new location, and the RSet updated. Otherwise we need to
      // use the the non-heap or metadata closures directly to copy
      // the referent object and update the pointer, while avoiding
      // updating the RSet.

      if (_g1h->is_in_g1_reserved(p)) {
        _par_scan_state->push_on_queue(p);
      } else {
        assert(!Metaspace::contains((const void*)p),
               "Unexpectedly found a pointer from metadata: " PTR_FORMAT, p2i(p));
        _copy_non_heap_obj_cl->do_oop(p);
      }
    }
  }
};

// Serial drain queue closure. Called as the 'complete_gc'
// closure for each discovered list in some of the
// reference processing phases.

class G1STWDrainQueueClosure: public VoidClosure {
protected:
  G1CollectedHeap* _g1h;
  G1ParScanThreadState* _par_scan_state;

  G1ParScanThreadState*   par_scan_state() { return _par_scan_state; }

public:
  G1STWDrainQueueClosure(G1CollectedHeap* g1h, G1ParScanThreadState* pss) :
    _g1h(g1h),
    _par_scan_state(pss)
  { }

  void do_void() {
    G1ParScanThreadState* const pss = par_scan_state();
    pss->trim_queue();
  }
};

// Parallel Reference Processing closures

// Implementation of AbstractRefProcTaskExecutor for parallel reference
// processing during G1 evacuation pauses.

class G1STWRefProcTaskExecutor: public AbstractRefProcTaskExecutor {
private:
  G1CollectedHeap*          _g1h;
  G1ParScanThreadStateSet*  _pss;
  RefToScanQueueSet*        _queues;
  WorkGang*                 _workers;
  uint                      _active_workers;

public:
  G1STWRefProcTaskExecutor(G1CollectedHeap* g1h,
                           G1ParScanThreadStateSet* per_thread_states,
                           WorkGang* workers,
                           RefToScanQueueSet *task_queues,
                           uint n_workers) :
    _g1h(g1h),
    _pss(per_thread_states),
    _queues(task_queues),
    _workers(workers),
    _active_workers(n_workers)
  {
    g1h->ref_processor_stw()->set_active_mt_degree(n_workers);
  }

  // Executes the given task using concurrent marking worker threads.
  virtual void execute(ProcessTask& task);
  virtual void execute(EnqueueTask& task);
};

// Gang task for possibly parallel reference processing

class G1STWRefProcTaskProxy: public AbstractGangTask {
  typedef AbstractRefProcTaskExecutor::ProcessTask ProcessTask;
  ProcessTask&     _proc_task;
  G1CollectedHeap* _g1h;
  G1ParScanThreadStateSet* _pss;
  RefToScanQueueSet* _task_queues;
  ParallelTaskTerminator* _terminator;

public:
  G1STWRefProcTaskProxy(ProcessTask& proc_task,
                        G1CollectedHeap* g1h,
                        G1ParScanThreadStateSet* per_thread_states,
                        RefToScanQueueSet *task_queues,
                        ParallelTaskTerminator* terminator) :
    AbstractGangTask("Process reference objects in parallel"),
    _proc_task(proc_task),
    _g1h(g1h),
    _pss(per_thread_states),
    _task_queues(task_queues),
    _terminator(terminator)
  {}

  virtual void work(uint worker_id) {
    // The reference processing task executed by a single worker.
    ResourceMark rm;
    HandleMark   hm;

    G1STWIsAliveClosure is_alive(_g1h);

    G1ParScanThreadState*          pss = _pss->state_for_worker(worker_id);
    pss->set_ref_processor(NULL);

    // Keep alive closure.
    G1CopyingKeepAliveClosure keep_alive(_g1h, pss->closures()->raw_strong_oops(), pss);

    // Complete GC closure
    G1ParEvacuateFollowersClosure drain_queue(_g1h, pss, _task_queues, _terminator);

    // Call the reference processing task's work routine.
    _proc_task.work(worker_id, is_alive, keep_alive, drain_queue);

    // Note we cannot assert that the refs array is empty here as not all
    // of the processing tasks (specifically phase2 - pp2_work) execute
    // the complete_gc closure (which ordinarily would drain the queue) so
    // the queue may not be empty.
  }
};

// Driver routine for parallel reference processing.
// Creates an instance of the ref processing gang
// task and has the worker threads execute it.
void G1STWRefProcTaskExecutor::execute(ProcessTask& proc_task) {
  assert(_workers != NULL, "Need parallel worker threads.");

  ParallelTaskTerminator terminator(_active_workers, _queues);
  G1STWRefProcTaskProxy proc_task_proxy(proc_task, _g1h, _pss, _queues, &terminator);

  _workers->run_task(&proc_task_proxy);
}

// Gang task for parallel reference enqueueing.

class G1STWRefEnqueueTaskProxy: public AbstractGangTask {
  typedef AbstractRefProcTaskExecutor::EnqueueTask EnqueueTask;
  EnqueueTask& _enq_task;

public:
  G1STWRefEnqueueTaskProxy(EnqueueTask& enq_task) :
    AbstractGangTask("Enqueue reference objects in parallel"),
    _enq_task(enq_task)
  { }

  virtual void work(uint worker_id) {
    _enq_task.work(worker_id);
  }
};

// Driver routine for parallel reference enqueueing.
// Creates an instance of the ref enqueueing gang
// task and has the worker threads execute it.

void G1STWRefProcTaskExecutor::execute(EnqueueTask& enq_task) {
  assert(_workers != NULL, "Need parallel worker threads.");

  G1STWRefEnqueueTaskProxy enq_task_proxy(enq_task);

  _workers->run_task(&enq_task_proxy);
}

// End of weak reference support closures

// Abstract task used to preserve (i.e. copy) any referent objects
// that are in the collection set and are pointed to by reference
// objects discovered by the CM ref processor.

class G1ParPreserveCMReferentsTask: public AbstractGangTask {
protected:
  G1CollectedHeap*         _g1h;
  G1ParScanThreadStateSet* _pss;
  RefToScanQueueSet*       _queues;
  ParallelTaskTerminator   _terminator;
  uint                     _n_workers;

public:
  G1ParPreserveCMReferentsTask(G1CollectedHeap* g1h, G1ParScanThreadStateSet* per_thread_states, int workers, RefToScanQueueSet *task_queues) :
    AbstractGangTask("ParPreserveCMReferents"),
    _g1h(g1h),
    _pss(per_thread_states),
    _queues(task_queues),
    _terminator(workers, _queues),
    _n_workers(workers)
  {
    g1h->ref_processor_cm()->set_active_mt_degree(workers);
  }

  void work(uint worker_id) {
    G1GCParPhaseTimesTracker x(_g1h->g1_policy()->phase_times(), G1GCPhaseTimes::PreserveCMReferents, worker_id);

    ResourceMark rm;
    HandleMark   hm;

    G1ParScanThreadState*          pss = _pss->state_for_worker(worker_id);
    pss->set_ref_processor(NULL);
    assert(pss->queue_is_empty(), "both queue and overflow should be empty");

    // Is alive closure
    G1AlwaysAliveClosure always_alive(_g1h);

    // Copying keep alive closure. Applied to referent objects that need
    // to be copied.
    G1CopyingKeepAliveClosure keep_alive(_g1h, pss->closures()->raw_strong_oops(), pss);

    ReferenceProcessor* rp = _g1h->ref_processor_cm();

    uint limit = ReferenceProcessor::number_of_subclasses_of_ref() * rp->max_num_q();
    uint stride = MIN2(MAX2(_n_workers, 1U), limit);

    // limit is set using max_num_q() - which was set using ParallelGCThreads.
    // So this must be true - but assert just in case someone decides to
    // change the worker ids.
    assert(worker_id < limit, "sanity");
    assert(!rp->discovery_is_atomic(), "check this code");

    // Select discovered lists [i, i+stride, i+2*stride,...,limit)
    for (uint idx = worker_id; idx < limit; idx += stride) {
      DiscoveredList& ref_list = rp->discovered_refs()[idx];

      DiscoveredListIterator iter(ref_list, &keep_alive, &always_alive);
      while (iter.has_next()) {
        // Since discovery is not atomic for the CM ref processor, we
        // can see some null referent objects.
        iter.load_ptrs(DEBUG_ONLY(true));
        oop ref = iter.obj();

        // This will filter nulls.
        if (iter.is_referent_alive()) {
          iter.make_referent_alive();
        }
        iter.move_to_next();
      }
    }

    // Drain the queue - which may cause stealing
    G1ParEvacuateFollowersClosure drain_queue(_g1h, pss, _queues, &_terminator);
    drain_queue.do_void();
    // Allocation buffers were retired at the end of G1ParEvacuateFollowersClosure
    assert(pss->queue_is_empty(), "should be");
  }
};

void G1CollectedHeap::preserve_cm_referents(G1ParScanThreadStateSet* per_thread_states) {
  // Any reference objects, in the collection set, that were 'discovered'
  // by the CM ref processor should have already been copied (either by
  // applying the external root copy closure to the discovered lists, or
  // by following an RSet entry).
  //
  // But some of the referents, that are in the collection set, that these
  // reference objects point to may not have been copied: the STW ref
  // processor would have seen that the reference object had already
  // been 'discovered' and would have skipped discovering the reference,
  // but would not have treated the reference object as a regular oop.
  // As a result the copy closure would not have been applied to the
  // referent object.
  //
  // We need to explicitly copy these referent objects - the references
  // will be processed at the end of remarking.
  //
  // We also need to do this copying before we process the reference
  // objects discovered by the STW ref processor in case one of these
  // referents points to another object which is also referenced by an
  // object discovered by the STW ref processor.
  double preserve_cm_referents_time = 0.0;

  // To avoid spawning task when there is no work to do, check that
  // a concurrent cycle is active and that some references have been
  // discovered.
  if (concurrent_mark()->cm_thread()->during_cycle() &&
      ref_processor_cm()->has_discovered_references()) {
    double preserve_cm_referents_start = os::elapsedTime();
    uint no_of_gc_workers = workers()->active_workers();
    G1ParPreserveCMReferentsTask keep_cm_referents(this,
                                                   per_thread_states,
                                                   no_of_gc_workers,
                                                   _task_queues);
    workers()->run_task(&keep_cm_referents);
    preserve_cm_referents_time = os::elapsedTime() - preserve_cm_referents_start;
  }

  g1_policy()->phase_times()->record_preserve_cm_referents_time_ms(preserve_cm_referents_time * 1000.0);
}

// Weak Reference processing during an evacuation pause (part 1).
void G1CollectedHeap::process_discovered_references(G1ParScanThreadStateSet* per_thread_states) {
  double ref_proc_start = os::elapsedTime();

  ReferenceProcessor* rp = _ref_processor_stw;
  assert(rp->discovery_enabled(), "should have been enabled");

  // Closure to test whether a referent is alive.
  G1STWIsAliveClosure is_alive(this);

  // Even when parallel reference processing is enabled, the processing
  // of JNI refs is serial and performed serially by the current thread
  // rather than by a worker. The following PSS will be used for processing
  // JNI refs.

  // Use only a single queue for this PSS.
  G1ParScanThreadState*          pss = per_thread_states->state_for_worker(0);
  pss->set_ref_processor(NULL);
  assert(pss->queue_is_empty(), "pre-condition");

  // Keep alive closure.
  G1CopyingKeepAliveClosure keep_alive(this, pss->closures()->raw_strong_oops(), pss);

  // Serial Complete GC closure
  G1STWDrainQueueClosure drain_queue(this, pss);

  // Setup the soft refs policy...
  rp->setup_policy(false);

  ReferenceProcessorPhaseTimes* pt = g1_policy()->phase_times()->ref_phase_times();

  ReferenceProcessorStats stats;
  if (!rp->processing_is_mt()) {
    // Serial reference processing...
    stats = rp->process_discovered_references(&is_alive,
                                              &keep_alive,
                                              &drain_queue,
                                              NULL,
                                              pt);
  } else {
    uint no_of_gc_workers = workers()->active_workers();

    // Parallel reference processing
    assert(no_of_gc_workers <= rp->max_num_q(),
           "Mismatch between the number of GC workers %u and the maximum number of Reference process queues %u",
           no_of_gc_workers,  rp->max_num_q());

    G1STWRefProcTaskExecutor par_task_executor(this, per_thread_states, workers(), _task_queues, no_of_gc_workers);
    stats = rp->process_discovered_references(&is_alive,
                                              &keep_alive,
                                              &drain_queue,
                                              &par_task_executor,
                                              pt);
  }

  _gc_tracer_stw->report_gc_reference_stats(stats);

  // We have completed copying any necessary live referent objects.
  assert(pss->queue_is_empty(), "both queue and overflow should be empty");

  double ref_proc_time = os::elapsedTime() - ref_proc_start;
  g1_policy()->phase_times()->record_ref_proc_time(ref_proc_time * 1000.0);
}

// Weak Reference processing during an evacuation pause (part 2).
void G1CollectedHeap::enqueue_discovered_references(G1ParScanThreadStateSet* per_thread_states) {
  double ref_enq_start = os::elapsedTime();

  ReferenceProcessor* rp = _ref_processor_stw;
  assert(!rp->discovery_enabled(), "should have been disabled as part of processing");

  ReferenceProcessorPhaseTimes* pt = g1_policy()->phase_times()->ref_phase_times();

  // Now enqueue any remaining on the discovered lists on to
  // the pending list.
  if (!rp->processing_is_mt()) {
    // Serial reference processing...
    rp->enqueue_discovered_references(NULL, pt);
  } else {
    // Parallel reference enqueueing

    uint n_workers = workers()->active_workers();

    assert(n_workers <= rp->max_num_q(),
           "Mismatch between the number of GC workers %u and the maximum number of Reference process queues %u",
           n_workers,  rp->max_num_q());

    G1STWRefProcTaskExecutor par_task_executor(this, per_thread_states, workers(), _task_queues, n_workers);
    rp->enqueue_discovered_references(&par_task_executor, pt);
  }

  rp->verify_no_references_recorded();
  assert(!rp->discovery_enabled(), "should have been disabled");

  // If during an initial mark pause we install a pending list head which is not otherwise reachable
  // ensure that it is marked in the bitmap for concurrent marking to discover.
  if (collector_state()->during_initial_mark_pause()) {
    oop pll_head = Universe::reference_pending_list();
    if (pll_head != NULL) {
      _cm->mark_in_next_bitmap(pll_head);
    }
  }

  // FIXME
  // CM's reference processing also cleans up the string and symbol tables.
  // Should we do that here also? We could, but it is a serial operation
  // and could significantly increase the pause time.

  double ref_enq_time = os::elapsedTime() - ref_enq_start;
  g1_policy()->phase_times()->record_ref_enq_time(ref_enq_time * 1000.0);
}

void G1CollectedHeap::merge_per_thread_state_info(G1ParScanThreadStateSet* per_thread_states) {
  double merge_pss_time_start = os::elapsedTime();
  per_thread_states->flush();
  g1_policy()->phase_times()->record_merge_pss_time_ms((os::elapsedTime() - merge_pss_time_start) * 1000.0);
}

void G1CollectedHeap::pre_evacuate_collection_set() {
  _expand_heap_after_alloc_failure = true;
  _evacuation_failed = false;

  // Disable the hot card cache.
  _hot_card_cache->reset_hot_cache_claimed_index();
  _hot_card_cache->set_use_cache(false);

  g1_rem_set()->prepare_for_oops_into_collection_set_do();
  _preserved_marks_set.assert_empty();

  G1GCPhaseTimes* phase_times = g1_policy()->phase_times();

  // InitialMark needs claim bits to keep track of the marked-through CLDs.
  if (collector_state()->during_initial_mark_pause()) {
    double start_clear_claimed_marks = os::elapsedTime();

    ClassLoaderDataGraph::clear_claimed_marks();

    double recorded_clear_claimed_marks_time_ms = (os::elapsedTime() - start_clear_claimed_marks) * 1000.0;
    phase_times->record_clear_claimed_marks_time_ms(recorded_clear_claimed_marks_time_ms);
  }
}

void G1CollectedHeap::evacuate_collection_set(EvacuationInfo& evacuation_info, G1ParScanThreadStateSet* per_thread_states) {
  // Should G1EvacuationFailureALot be in effect for this GC?
  NOT_PRODUCT(set_evacuation_failure_alot_for_current_gc();)

  assert(dirty_card_queue_set().completed_buffers_num() == 0, "Should be empty");

  G1GCPhaseTimes* phase_times = g1_policy()->phase_times();

  double start_par_time_sec = os::elapsedTime();
  double end_par_time_sec;

  {
    const uint n_workers = workers()->active_workers();
    G1RootProcessor root_processor(this, n_workers);
    G1ParTask g1_par_task(this, per_thread_states, _task_queues, &root_processor, n_workers);

    print_termination_stats_hdr();

    workers()->run_task(&g1_par_task);
    end_par_time_sec = os::elapsedTime();

    // Closing the inner scope will execute the destructor
    // for the G1RootProcessor object. We record the current
    // elapsed time before closing the scope so that time
    // taken for the destructor is NOT included in the
    // reported parallel time.
  }

  double par_time_ms = (end_par_time_sec - start_par_time_sec) * 1000.0;
  phase_times->record_par_time(par_time_ms);

  double code_root_fixup_time_ms =
        (os::elapsedTime() - end_par_time_sec) * 1000.0;
  phase_times->record_code_root_fixup_time(code_root_fixup_time_ms);
}

void G1CollectedHeap::post_evacuate_collection_set(EvacuationInfo& evacuation_info, G1ParScanThreadStateSet* per_thread_states) {
  // Process any discovered reference objects - we have
  // to do this _before_ we retire the GC alloc regions
  // as we may have to copy some 'reachable' referent
  // objects (and their reachable sub-graphs) that were
  // not copied during the pause.
  if (g1_policy()->should_process_references()) {
    preserve_cm_referents(per_thread_states);
    process_discovered_references(per_thread_states);
  } else {
    ref_processor_stw()->verify_no_references_recorded();
  }

  G1STWIsAliveClosure is_alive(this);
  G1KeepAliveClosure keep_alive(this);

  {
    double start = os::elapsedTime();

    WeakProcessor::weak_oops_do(&is_alive, &keep_alive);

    double time_ms = (os::elapsedTime() - start) * 1000.0;
    g1_policy()->phase_times()->record_ref_proc_time(time_ms);
  }

  if (G1StringDedup::is_enabled()) {
    double fixup_start = os::elapsedTime();

    G1StringDedup::unlink_or_oops_do(&is_alive, &keep_alive, true, g1_policy()->phase_times());

    double fixup_time_ms = (os::elapsedTime() - fixup_start) * 1000.0;
    g1_policy()->phase_times()->record_string_dedup_fixup_time(fixup_time_ms);
  }

  g1_rem_set()->cleanup_after_oops_into_collection_set_do();

  if (evacuation_failed()) {
    restore_after_evac_failure();

    // Reset the G1EvacuationFailureALot counters and flags
    // Note: the values are reset only when an actual
    // evacuation failure occurs.
    NOT_PRODUCT(reset_evacuation_should_fail();)
  }

  _preserved_marks_set.assert_empty();

  // Enqueue any remaining references remaining on the STW
  // reference processor's discovered lists. We need to do
  // this after the card table is cleaned (and verified) as
  // the act of enqueueing entries on to the pending list
  // will log these updates (and dirty their associated
  // cards). We need these updates logged to update any
  // RSets.
  if (g1_policy()->should_process_references()) {
    enqueue_discovered_references(per_thread_states);
  } else {
    g1_policy()->phase_times()->record_ref_enq_time(0);
  }

  _allocator->release_gc_alloc_regions(evacuation_info);

  merge_per_thread_state_info(per_thread_states);

  // Reset and re-enable the hot card cache.
  // Note the counts for the cards in the regions in the
  // collection set are reset when the collection set is freed.
  _hot_card_cache->reset_hot_cache();
  _hot_card_cache->set_use_cache(true);

  purge_code_root_memory();

  redirty_logged_cards();
#if COMPILER2_OR_JVMCI
  double start = os::elapsedTime();
  DerivedPointerTable::update_pointers();
  g1_policy()->phase_times()->record_derived_pointer_table_update_time((os::elapsedTime() - start) * 1000.0);
#endif
  g1_policy()->print_age_table();
}

void G1CollectedHeap::record_obj_copy_mem_stats() {
  g1_policy()->add_bytes_allocated_in_old_since_last_gc(_old_evac_stats.allocated() * HeapWordSize);

  _gc_tracer_stw->report_evacuation_statistics(create_g1_evac_summary(&_survivor_evac_stats),
                                               create_g1_evac_summary(&_old_evac_stats));
}

void G1CollectedHeap::free_region(HeapRegion* hr,
                                  FreeRegionList* free_list,
                                  bool skip_remset,
                                  bool skip_hot_card_cache,
                                  bool locked) {
  assert(!hr->is_free(), "the region should not be free");
  assert(!hr->is_empty(), "the region should not be empty");
  assert(_hrm.is_available(hr->hrm_index()), "region should be committed");
  assert(free_list != NULL, "pre-condition");

  if (G1VerifyBitmaps) {
    MemRegion mr(hr->bottom(), hr->end());
    concurrent_mark()->clear_range_in_prev_bitmap(mr);
  }

  // Clear the card counts for this region.
  // Note: we only need to do this if the region is not young
  // (since we don't refine cards in young regions).
  if (!skip_hot_card_cache && !hr->is_young()) {
    _hot_card_cache->reset_card_counts(hr);
  }
  hr->hr_clear(skip_remset, true /* clear_space */, locked /* locked */);
  free_list->add_ordered(hr);
}

void G1CollectedHeap::free_humongous_region(HeapRegion* hr,
                                            FreeRegionList* free_list,
                                            bool skip_remset) {
  assert(hr->is_humongous(), "this is only for humongous regions");
  assert(free_list != NULL, "pre-condition");
  hr->clear_humongous();
  free_region(hr, free_list, skip_remset);
}

void G1CollectedHeap::remove_from_old_sets(const uint old_regions_removed,
                                           const uint humongous_regions_removed) {
  if (old_regions_removed > 0 || humongous_regions_removed > 0) {
    MutexLockerEx x(OldSets_lock, Mutex::_no_safepoint_check_flag);
    _old_set.bulk_remove(old_regions_removed);
    _humongous_set.bulk_remove(humongous_regions_removed);
  }

}

void G1CollectedHeap::prepend_to_freelist(FreeRegionList* list) {
  assert(list != NULL, "list can't be null");
  if (!list->is_empty()) {
    MutexLockerEx x(FreeList_lock, Mutex::_no_safepoint_check_flag);
    _hrm.insert_list_into_free_list(list);
  }
}

void G1CollectedHeap::decrement_summary_bytes(size_t bytes) {
  decrease_used(bytes);
}

class G1ParScrubRemSetTask: public AbstractGangTask {
protected:
  G1RemSet* _g1rs;
  HeapRegionClaimer _hrclaimer;

public:
  G1ParScrubRemSetTask(G1RemSet* g1_rs, uint num_workers) :
    AbstractGangTask("G1 ScrubRS"),
    _g1rs(g1_rs),
    _hrclaimer(num_workers) {
  }

  void work(uint worker_id) {
    _g1rs->scrub(worker_id, &_hrclaimer);
  }
};

void G1CollectedHeap::scrub_rem_set() {
  uint num_workers = workers()->active_workers();
  G1ParScrubRemSetTask g1_par_scrub_rs_task(g1_rem_set(), num_workers);
  workers()->run_task(&g1_par_scrub_rs_task);
}

class G1FreeCollectionSetTask : public AbstractGangTask {
private:

  // Closure applied to all regions in the collection set to do work that needs to
  // be done serially in a single thread.
  class G1SerialFreeCollectionSetClosure : public HeapRegionClosure {
  private:
    EvacuationInfo* _evacuation_info;
    const size_t* _surviving_young_words;

    // Bytes used in successfully evacuated regions before the evacuation.
    size_t _before_used_bytes;
    // Bytes used in unsucessfully evacuated regions before the evacuation
    size_t _after_used_bytes;

    size_t _bytes_allocated_in_old_since_last_gc;

    size_t _failure_used_words;
    size_t _failure_waste_words;

    FreeRegionList _local_free_list;
  public:
    G1SerialFreeCollectionSetClosure(EvacuationInfo* evacuation_info, const size_t* surviving_young_words) :
      HeapRegionClosure(),
      _evacuation_info(evacuation_info),
      _surviving_young_words(surviving_young_words),
      _before_used_bytes(0),
      _after_used_bytes(0),
      _bytes_allocated_in_old_since_last_gc(0),
      _failure_used_words(0),
      _failure_waste_words(0),
      _local_free_list("Local Region List for CSet Freeing") {
    }

    virtual bool doHeapRegion(HeapRegion* r) {
      G1CollectedHeap* g1h = G1CollectedHeap::heap();

      assert(r->in_collection_set(), "Region %u should be in collection set.", r->hrm_index());
      g1h->clear_in_cset(r);

      if (r->is_young()) {
        assert(r->young_index_in_cset() != -1 && (uint)r->young_index_in_cset() < g1h->collection_set()->young_region_length(),
               "Young index %d is wrong for region %u of type %s with %u young regions",
               r->young_index_in_cset(),
               r->hrm_index(),
               r->get_type_str(),
               g1h->collection_set()->young_region_length());
        size_t words_survived = _surviving_young_words[r->young_index_in_cset()];
        r->record_surv_words_in_group(words_survived);
      }

      if (!r->evacuation_failed()) {
        assert(r->not_empty(), "Region %u is an empty region in the collection set.", r->hrm_index());
        _before_used_bytes += r->used();
        g1h->free_region(r,
                         &_local_free_list,
                         true, /* skip_remset */
                         true, /* skip_hot_card_cache */
                         true  /* locked */);
      } else {
        r->uninstall_surv_rate_group();
        r->set_young_index_in_cset(-1);
        r->set_evacuation_failed(false);
        // When moving a young gen region to old gen, we "allocate" that whole region
        // there. This is in addition to any already evacuated objects. Notify the
        // policy about that.
        // Old gen regions do not cause an additional allocation: both the objects
        // still in the region and the ones already moved are accounted for elsewhere.
        if (r->is_young()) {
          _bytes_allocated_in_old_since_last_gc += HeapRegion::GrainBytes;
        }
        // The region is now considered to be old.
        r->set_old();
        // Do some allocation statistics accounting. Regions that failed evacuation
        // are always made old, so there is no need to update anything in the young
        // gen statistics, but we need to update old gen statistics.
        size_t used_words = r->marked_bytes() / HeapWordSize;

        _failure_used_words += used_words;
        _failure_waste_words += HeapRegion::GrainWords - used_words;

        g1h->old_set_add(r);
        _after_used_bytes += r->used();
      }
      return false;
    }

    void complete_work() {
      G1CollectedHeap* g1h = G1CollectedHeap::heap();

      _evacuation_info->set_regions_freed(_local_free_list.length());
      _evacuation_info->increment_collectionset_used_after(_after_used_bytes);

      g1h->prepend_to_freelist(&_local_free_list);
      g1h->decrement_summary_bytes(_before_used_bytes);

      G1Policy* policy = g1h->g1_policy();
      policy->add_bytes_allocated_in_old_since_last_gc(_bytes_allocated_in_old_since_last_gc);

      g1h->alloc_buffer_stats(InCSetState::Old)->add_failure_used_and_waste(_failure_used_words, _failure_waste_words);
    }
  };

  G1CollectionSet* _collection_set;
  G1SerialFreeCollectionSetClosure _cl;
  const size_t* _surviving_young_words;

  size_t _rs_lengths;

  volatile jint _serial_work_claim;

  struct WorkItem {
    uint region_idx;
    bool is_young;
    bool evacuation_failed;

    WorkItem(HeapRegion* r) {
      region_idx = r->hrm_index();
      is_young = r->is_young();
      evacuation_failed = r->evacuation_failed();
    }
  };

  volatile size_t _parallel_work_claim;
  size_t _num_work_items;
  WorkItem* _work_items;

  void do_serial_work() {
    // Need to grab the lock to be allowed to modify the old region list.
    MutexLockerEx x(OldSets_lock, Mutex::_no_safepoint_check_flag);
    _collection_set->iterate(&_cl);
  }

  void do_parallel_work_for_region(uint region_idx, bool is_young, bool evacuation_failed) {
    G1CollectedHeap* g1h = G1CollectedHeap::heap();

    HeapRegion* r = g1h->region_at(region_idx);
    assert(!g1h->is_on_master_free_list(r), "sanity");

    Atomic::add(r->rem_set()->occupied_locked(), &_rs_lengths);

    if (!is_young) {
      g1h->_hot_card_cache->reset_card_counts(r);
    }

    if (!evacuation_failed) {
      r->rem_set()->clear_locked();
    }
  }

  class G1PrepareFreeCollectionSetClosure : public HeapRegionClosure {
  private:
    size_t _cur_idx;
    WorkItem* _work_items;
  public:
    G1PrepareFreeCollectionSetClosure(WorkItem* work_items) : HeapRegionClosure(), _cur_idx(0), _work_items(work_items) { }

    virtual bool doHeapRegion(HeapRegion* r) {
      _work_items[_cur_idx++] = WorkItem(r);
      return false;
    }
  };

  void prepare_work() {
    G1PrepareFreeCollectionSetClosure cl(_work_items);
    _collection_set->iterate(&cl);
  }

  void complete_work() {
    _cl.complete_work();

    G1Policy* policy = G1CollectedHeap::heap()->g1_policy();
    policy->record_max_rs_lengths(_rs_lengths);
    policy->cset_regions_freed();
  }
public:
  G1FreeCollectionSetTask(G1CollectionSet* collection_set, EvacuationInfo* evacuation_info, const size_t* surviving_young_words) :
    AbstractGangTask("G1 Free Collection Set"),
    _cl(evacuation_info, surviving_young_words),
    _collection_set(collection_set),
    _surviving_young_words(surviving_young_words),
    _serial_work_claim(0),
    _rs_lengths(0),
    _parallel_work_claim(0),
    _num_work_items(collection_set->region_length()),
    _work_items(NEW_C_HEAP_ARRAY(WorkItem, _num_work_items, mtGC)) {
    prepare_work();
  }

  ~G1FreeCollectionSetTask() {
    complete_work();
    FREE_C_HEAP_ARRAY(WorkItem, _work_items);
  }

  // Chunk size for work distribution. The chosen value has been determined experimentally
  // to be a good tradeoff between overhead and achievable parallelism.
  static uint chunk_size() { return 32; }

  virtual void work(uint worker_id) {
    G1GCPhaseTimes* timer = G1CollectedHeap::heap()->g1_policy()->phase_times();

    // Claim serial work.
    if (_serial_work_claim == 0) {
      jint value = Atomic::add(1, &_serial_work_claim) - 1;
      if (value == 0) {
        double serial_time = os::elapsedTime();
        do_serial_work();
        timer->record_serial_free_cset_time_ms((os::elapsedTime() - serial_time) * 1000.0);
      }
    }

    // Start parallel work.
    double young_time = 0.0;
    bool has_young_time = false;
    double non_young_time = 0.0;
    bool has_non_young_time = false;

    while (true) {
      size_t end = Atomic::add(chunk_size(), &_parallel_work_claim);
      size_t cur = end - chunk_size();

      if (cur >= _num_work_items) {
        break;
      }

      double start_time = os::elapsedTime();

      end = MIN2(end, _num_work_items);

      for (; cur < end; cur++) {
        bool is_young = _work_items[cur].is_young;

        do_parallel_work_for_region(_work_items[cur].region_idx, is_young, _work_items[cur].evacuation_failed);

        double end_time = os::elapsedTime();
        double time_taken = end_time - start_time;
        if (is_young) {
          young_time += time_taken;
          has_young_time = true;
        } else {
          non_young_time += time_taken;
          has_non_young_time = true;
        }
        start_time = end_time;
      }
    }

    if (has_young_time) {
      timer->record_time_secs(G1GCPhaseTimes::YoungFreeCSet, worker_id, young_time);
    }
    if (has_non_young_time) {
      timer->record_time_secs(G1GCPhaseTimes::NonYoungFreeCSet, worker_id, young_time);
    }
  }
};

void G1CollectedHeap::free_collection_set(G1CollectionSet* collection_set, EvacuationInfo& evacuation_info, const size_t* surviving_young_words) {
  _eden.clear();

  double free_cset_start_time = os::elapsedTime();

  {
    uint const num_chunks = MAX2(_collection_set.region_length() / G1FreeCollectionSetTask::chunk_size(), 1U);
    uint const num_workers = MIN2(workers()->active_workers(), num_chunks);

    G1FreeCollectionSetTask cl(collection_set, &evacuation_info, surviving_young_words);

    log_debug(gc, ergo)("Running %s using %u workers for collection set length %u",
                        cl.name(),
                        num_workers,
                        _collection_set.region_length());
    workers()->run_task(&cl, num_workers);
  }
  g1_policy()->phase_times()->record_total_free_cset_time_ms((os::elapsedTime() - free_cset_start_time) * 1000.0);

  collection_set->clear();
}

class G1FreeHumongousRegionClosure : public HeapRegionClosure {
 private:
  FreeRegionList* _free_region_list;
  HeapRegionSet* _proxy_set;
  uint _humongous_objects_reclaimed;
  uint _humongous_regions_reclaimed;
  size_t _freed_bytes;
 public:

  G1FreeHumongousRegionClosure(FreeRegionList* free_region_list) :
    _free_region_list(free_region_list), _humongous_objects_reclaimed(0), _humongous_regions_reclaimed(0), _freed_bytes(0) {
  }

  virtual bool doHeapRegion(HeapRegion* r) {
    if (!r->is_starts_humongous()) {
      return false;
    }

    G1CollectedHeap* g1h = G1CollectedHeap::heap();

    oop obj = (oop)r->bottom();
    G1CMBitMap* next_bitmap = g1h->concurrent_mark()->next_mark_bitmap();

    // The following checks whether the humongous object is live are sufficient.
    // The main additional check (in addition to having a reference from the roots
    // or the young gen) is whether the humongous object has a remembered set entry.
    //
    // A humongous object cannot be live if there is no remembered set for it
    // because:
    // - there can be no references from within humongous starts regions referencing
    // the object because we never allocate other objects into them.
    // (I.e. there are no intra-region references that may be missed by the
    // remembered set)
    // - as soon there is a remembered set entry to the humongous starts region
    // (i.e. it has "escaped" to an old object) this remembered set entry will stay
    // until the end of a concurrent mark.
    //
    // It is not required to check whether the object has been found dead by marking
    // or not, in fact it would prevent reclamation within a concurrent cycle, as
    // all objects allocated during that time are considered live.
    // SATB marking is even more conservative than the remembered set.
    // So if at this point in the collection there is no remembered set entry,
    // nobody has a reference to it.
    // At the start of collection we flush all refinement logs, and remembered sets
    // are completely up-to-date wrt to references to the humongous object.
    //
    // Other implementation considerations:
    // - never consider object arrays at this time because they would pose
    // considerable effort for cleaning up the the remembered sets. This is
    // required because stale remembered sets might reference locations that
    // are currently allocated into.
    uint region_idx = r->hrm_index();
    if (!g1h->is_humongous_reclaim_candidate(region_idx) ||
        !r->rem_set()->is_empty()) {
      log_debug(gc, humongous)("Live humongous region %u object size " SIZE_FORMAT " start " PTR_FORMAT "  with remset " SIZE_FORMAT " code roots " SIZE_FORMAT " is marked %d reclaim candidate %d type array %d",
                               region_idx,
                               (size_t)obj->size() * HeapWordSize,
                               p2i(r->bottom()),
                               r->rem_set()->occupied(),
                               r->rem_set()->strong_code_roots_list_length(),
                               next_bitmap->is_marked(r->bottom()),
                               g1h->is_humongous_reclaim_candidate(region_idx),
                               obj->is_typeArray()
                              );
      return false;
    }

    guarantee(obj->is_typeArray(),
              "Only eagerly reclaiming type arrays is supported, but the object "
              PTR_FORMAT " is not.", p2i(r->bottom()));

    log_debug(gc, humongous)("Dead humongous region %u object size " SIZE_FORMAT " start " PTR_FORMAT " with remset " SIZE_FORMAT " code roots " SIZE_FORMAT " is marked %d reclaim candidate %d type array %d",
                             region_idx,
                             (size_t)obj->size() * HeapWordSize,
                             p2i(r->bottom()),
                             r->rem_set()->occupied(),
                             r->rem_set()->strong_code_roots_list_length(),
                             next_bitmap->is_marked(r->bottom()),
                             g1h->is_humongous_reclaim_candidate(region_idx),
                             obj->is_typeArray()
                            );

    // Need to clear mark bit of the humongous object if already set.
    if (next_bitmap->is_marked(r->bottom())) {
      next_bitmap->clear(r->bottom());
    }
    _humongous_objects_reclaimed++;
    do {
      HeapRegion* next = g1h->next_region_in_humongous(r);
      _freed_bytes += r->used();
      r->set_containing_set(NULL);
      _humongous_regions_reclaimed++;
      g1h->free_humongous_region(r, _free_region_list, false /* skip_remset */ );
      r = next;
    } while (r != NULL);

    return false;
  }

  uint humongous_objects_reclaimed() {
    return _humongous_objects_reclaimed;
  }

  uint humongous_regions_reclaimed() {
    return _humongous_regions_reclaimed;
  }

  size_t bytes_freed() const {
    return _freed_bytes;
  }
};

void G1CollectedHeap::eagerly_reclaim_humongous_regions() {
  assert_at_safepoint(true);

  if (!G1EagerReclaimHumongousObjects ||
      (!_has_humongous_reclaim_candidates && !log_is_enabled(Debug, gc, humongous))) {
    g1_policy()->phase_times()->record_fast_reclaim_humongous_time_ms(0.0, 0);
    return;
  }

  double start_time = os::elapsedTime();

  FreeRegionList local_cleanup_list("Local Humongous Cleanup List");

  G1FreeHumongousRegionClosure cl(&local_cleanup_list);
  heap_region_iterate(&cl);

  remove_from_old_sets(0, cl.humongous_regions_reclaimed());

  G1HRPrinter* hrp = hr_printer();
  if (hrp->is_active()) {
    FreeRegionListIterator iter(&local_cleanup_list);
    while (iter.more_available()) {
      HeapRegion* hr = iter.get_next();
      hrp->cleanup(hr);
    }
  }

  prepend_to_freelist(&local_cleanup_list);
  decrement_summary_bytes(cl.bytes_freed());

  g1_policy()->phase_times()->record_fast_reclaim_humongous_time_ms((os::elapsedTime() - start_time) * 1000.0,
                                                                    cl.humongous_objects_reclaimed());
}

class G1AbandonCollectionSetClosure : public HeapRegionClosure {
public:
  virtual bool doHeapRegion(HeapRegion* r) {
    assert(r->in_collection_set(), "Region %u must have been in collection set", r->hrm_index());
    G1CollectedHeap::heap()->clear_in_cset(r);
    r->set_young_index_in_cset(-1);
    return false;
  }
};

void G1CollectedHeap::abandon_collection_set(G1CollectionSet* collection_set) {
  G1AbandonCollectionSetClosure cl;
  collection_set->iterate(&cl);

  collection_set->clear();
  collection_set->stop_incremental_building();
}

void G1CollectedHeap::set_free_regions_coming() {
  log_develop_trace(gc, freelist)("G1ConcRegionFreeing [cm thread] : setting free regions coming");

  assert(!free_regions_coming(), "pre-condition");
  _free_regions_coming = true;
}

void G1CollectedHeap::reset_free_regions_coming() {
  assert(free_regions_coming(), "pre-condition");

  {
    MutexLockerEx x(SecondaryFreeList_lock, Mutex::_no_safepoint_check_flag);
    _free_regions_coming = false;
    SecondaryFreeList_lock->notify_all();
  }

  log_develop_trace(gc, freelist)("G1ConcRegionFreeing [cm thread] : reset free regions coming");
}

void G1CollectedHeap::wait_while_free_regions_coming() {
  // Most of the time we won't have to wait, so let's do a quick test
  // first before we take the lock.
  if (!free_regions_coming()) {
    return;
  }

  log_develop_trace(gc, freelist)("G1ConcRegionFreeing [other] : waiting for free regions");

  {
    MutexLockerEx x(SecondaryFreeList_lock, Mutex::_no_safepoint_check_flag);
    while (free_regions_coming()) {
      SecondaryFreeList_lock->wait(Mutex::_no_safepoint_check_flag);
    }
  }

  log_develop_trace(gc, freelist)("G1ConcRegionFreeing [other] : done waiting for free regions");
}

bool G1CollectedHeap::is_old_gc_alloc_region(HeapRegion* hr) {
  return _allocator->is_retained_old_region(hr);
}

void G1CollectedHeap::set_region_short_lived_locked(HeapRegion* hr) {
  _eden.add(hr);
  _g1_policy->set_region_eden(hr);
}

#ifdef ASSERT

class NoYoungRegionsClosure: public HeapRegionClosure {
private:
  bool _success;
public:
  NoYoungRegionsClosure() : _success(true) { }
  bool doHeapRegion(HeapRegion* r) {
    if (r->is_young()) {
      log_error(gc, verify)("Region [" PTR_FORMAT ", " PTR_FORMAT ") tagged as young",
                            p2i(r->bottom()), p2i(r->end()));
      _success = false;
    }
    return false;
  }
  bool success() { return _success; }
};

bool G1CollectedHeap::check_young_list_empty() {
  bool ret = (young_regions_count() == 0);

  NoYoungRegionsClosure closure;
  heap_region_iterate(&closure);
  ret = ret && closure.success();

  return ret;
}

#endif // ASSERT

class TearDownRegionSetsClosure : public HeapRegionClosure {
private:
  HeapRegionSet *_old_set;

public:
  TearDownRegionSetsClosure(HeapRegionSet* old_set) : _old_set(old_set) { }

  bool doHeapRegion(HeapRegion* r) {
    if (r->is_old()) {
      _old_set->remove(r);
    } else if(r->is_young()) {
      r->uninstall_surv_rate_group();
    } else {
      // We ignore free regions, we'll empty the free list afterwards.
      // We ignore humongous regions, we're not tearing down the
      // humongous regions set.
      assert(r->is_free() || r->is_humongous(),
             "it cannot be another type");
    }
    return false;
  }

  ~TearDownRegionSetsClosure() {
    assert(_old_set->is_empty(), "post-condition");
  }
};

void G1CollectedHeap::tear_down_region_sets(bool free_list_only) {
  assert_at_safepoint(true /* should_be_vm_thread */);

  if (!free_list_only) {
    TearDownRegionSetsClosure cl(&_old_set);
    heap_region_iterate(&cl);

    // Note that emptying the _young_list is postponed and instead done as
    // the first step when rebuilding the regions sets again. The reason for
    // this is that during a full GC string deduplication needs to know if
    // a collected region was young or old when the full GC was initiated.
  }
  _hrm.remove_all_free_regions();
}

void G1CollectedHeap::increase_used(size_t bytes) {
  _summary_bytes_used += bytes;
}

void G1CollectedHeap::decrease_used(size_t bytes) {
  assert(_summary_bytes_used >= bytes,
         "invariant: _summary_bytes_used: " SIZE_FORMAT " should be >= bytes: " SIZE_FORMAT,
         _summary_bytes_used, bytes);
  _summary_bytes_used -= bytes;
}

void G1CollectedHeap::set_used(size_t bytes) {
  _summary_bytes_used = bytes;
}

class RebuildRegionSetsClosure : public HeapRegionClosure {
private:
  bool            _free_list_only;
  HeapRegionSet*   _old_set;
  HeapRegionManager*   _hrm;
  size_t          _total_used;

public:
  RebuildRegionSetsClosure(bool free_list_only,
                           HeapRegionSet* old_set, HeapRegionManager* hrm) :
    _free_list_only(free_list_only),
    _old_set(old_set), _hrm(hrm), _total_used(0) {
    assert(_hrm->num_free_regions() == 0, "pre-condition");
    if (!free_list_only) {
      assert(_old_set->is_empty(), "pre-condition");
    }
  }

  bool doHeapRegion(HeapRegion* r) {
    if (r->is_empty()) {
      // Add free regions to the free list
      r->set_free();
      r->set_allocation_context(AllocationContext::system());
      _hrm->insert_into_free_list(r);
    } else if (!_free_list_only) {

      if (r->is_humongous()) {
        // We ignore humongous regions. We left the humongous set unchanged.
      } else {
        assert(r->is_young() || r->is_free() || r->is_old(), "invariant");
        // We now move all (non-humongous, non-old) regions to old gen, and register them as such.
        r->move_to_old();
        _old_set->add(r);
      }
      _total_used += r->used();
    }

    return false;
  }

  size_t total_used() {
    return _total_used;
  }
};

void G1CollectedHeap::rebuild_region_sets(bool free_list_only) {
  assert_at_safepoint(true /* should_be_vm_thread */);

  if (!free_list_only) {
    _eden.clear();
    _survivor.clear();
  }

  RebuildRegionSetsClosure cl(free_list_only, &_old_set, &_hrm);
  heap_region_iterate(&cl);

  if (!free_list_only) {
    set_used(cl.total_used());
    if (_archive_allocator != NULL) {
      _archive_allocator->clear_used();
    }
  }
  assert(used_unlocked() == recalculate_used(),
         "inconsistent used_unlocked(), "
         "value: " SIZE_FORMAT " recalculated: " SIZE_FORMAT,
         used_unlocked(), recalculate_used());
}

bool G1CollectedHeap::is_in_closed_subset(const void* p) const {
  HeapRegion* hr = heap_region_containing(p);
  return hr->is_in(p);
}

// Methods for the mutator alloc region

HeapRegion* G1CollectedHeap::new_mutator_alloc_region(size_t word_size,
                                                      bool force) {
  assert_heap_locked_or_at_safepoint(true /* should_be_vm_thread */);
  bool should_allocate = g1_policy()->should_allocate_mutator_region();
  if (force || should_allocate) {
    HeapRegion* new_alloc_region = new_region(word_size,
                                              false /* is_old */,
                                              false /* do_expand */);
    if (new_alloc_region != NULL) {
      set_region_short_lived_locked(new_alloc_region);
      _hr_printer.alloc(new_alloc_region, !should_allocate);
      _verifier->check_bitmaps("Mutator Region Allocation", new_alloc_region);
      return new_alloc_region;
    }
  }
  return NULL;
}

void G1CollectedHeap::retire_mutator_alloc_region(HeapRegion* alloc_region,
                                                  size_t allocated_bytes) {
  assert_heap_locked_or_at_safepoint(true /* should_be_vm_thread */);
  assert(alloc_region->is_eden(), "all mutator alloc regions should be eden");

  collection_set()->add_eden_region(alloc_region);
  increase_used(allocated_bytes);
  _hr_printer.retire(alloc_region);
  // We update the eden sizes here, when the region is retired,
  // instead of when it's allocated, since this is the point that its
  // used space has been recored in _summary_bytes_used.
  g1mm()->update_eden_size();
}

// Methods for the GC alloc regions

bool G1CollectedHeap::has_more_regions(InCSetState dest) {
  if (dest.is_old()) {
    return true;
  } else {
    return survivor_regions_count() < g1_policy()->max_survivor_regions();
  }
}

HeapRegion* G1CollectedHeap::new_gc_alloc_region(size_t word_size, InCSetState dest) {
  assert(FreeList_lock->owned_by_self(), "pre-condition");

  if (!has_more_regions(dest)) {
    return NULL;
  }

  const bool is_survivor = dest.is_young();

  HeapRegion* new_alloc_region = new_region(word_size,
                                            !is_survivor,
                                            true /* do_expand */);
  if (new_alloc_region != NULL) {
    // We really only need to do this for old regions given that we
    // should never scan survivors. But it doesn't hurt to do it
    // for survivors too.
    new_alloc_region->record_timestamp();
    if (is_survivor) {
      new_alloc_region->set_survivor();
      _survivor.add(new_alloc_region);
      _verifier->check_bitmaps("Survivor Region Allocation", new_alloc_region);
    } else {
      new_alloc_region->set_old();
      _verifier->check_bitmaps("Old Region Allocation", new_alloc_region);
    }
    _hr_printer.alloc(new_alloc_region);
    bool during_im = collector_state()->during_initial_mark_pause();
    new_alloc_region->note_start_of_copying(during_im);
    return new_alloc_region;
  }
  return NULL;
}

void G1CollectedHeap::retire_gc_alloc_region(HeapRegion* alloc_region,
                                             size_t allocated_bytes,
                                             InCSetState dest) {
  bool during_im = collector_state()->during_initial_mark_pause();
  alloc_region->note_end_of_copying(during_im);
  g1_policy()->record_bytes_copied_during_gc(allocated_bytes);
  if (dest.is_old()) {
    _old_set.add(alloc_region);
  }
  _hr_printer.retire(alloc_region);
}

HeapRegion* G1CollectedHeap::alloc_highest_free_region() {
  bool expanded = false;
  uint index = _hrm.find_highest_free(&expanded);

  if (index != G1_NO_HRM_INDEX) {
    if (expanded) {
      log_debug(gc, ergo, heap)("Attempt heap expansion (requested address range outside heap bounds). region size: " SIZE_FORMAT "B",
                                HeapRegion::GrainWords * HeapWordSize);
    }
    _hrm.allocate_free_regions_starting_at(index, 1);
    return region_at(index);
  }
  return NULL;
}

// Optimized nmethod scanning

class RegisterNMethodOopClosure: public OopClosure {
  G1CollectedHeap* _g1h;
  nmethod* _nm;

  template <class T> void do_oop_work(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      HeapRegion* hr = _g1h->heap_region_containing(obj);
      assert(!hr->is_continues_humongous(),
             "trying to add code root " PTR_FORMAT " in continuation of humongous region " HR_FORMAT
             " starting at " HR_FORMAT,
             p2i(_nm), HR_FORMAT_PARAMS(hr), HR_FORMAT_PARAMS(hr->humongous_start_region()));

      // HeapRegion::add_strong_code_root_locked() avoids adding duplicate entries.
      hr->add_strong_code_root_locked(_nm);
    }
  }

public:
  RegisterNMethodOopClosure(G1CollectedHeap* g1h, nmethod* nm) :
    _g1h(g1h), _nm(nm) {}

  void do_oop(oop* p)       { do_oop_work(p); }
  void do_oop(narrowOop* p) { do_oop_work(p); }
};

class UnregisterNMethodOopClosure: public OopClosure {
  G1CollectedHeap* _g1h;
  nmethod* _nm;

  template <class T> void do_oop_work(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      HeapRegion* hr = _g1h->heap_region_containing(obj);
      assert(!hr->is_continues_humongous(),
             "trying to remove code root " PTR_FORMAT " in continuation of humongous region " HR_FORMAT
             " starting at " HR_FORMAT,
             p2i(_nm), HR_FORMAT_PARAMS(hr), HR_FORMAT_PARAMS(hr->humongous_start_region()));

      hr->remove_strong_code_root(_nm);
    }
  }

public:
  UnregisterNMethodOopClosure(G1CollectedHeap* g1h, nmethod* nm) :
    _g1h(g1h), _nm(nm) {}

  void do_oop(oop* p)       { do_oop_work(p); }
  void do_oop(narrowOop* p) { do_oop_work(p); }
};

// Returns true if the reference points to an object that
// can move in an incremental collection.
bool G1CollectedHeap::is_scavengable(oop obj) {
  HeapRegion* hr = heap_region_containing(obj);
  return !hr->is_pinned();
}

void G1CollectedHeap::register_nmethod(nmethod* nm) {
  guarantee(nm != NULL, "sanity");
  RegisterNMethodOopClosure reg_cl(this, nm);
  nm->oops_do(&reg_cl);
}

void G1CollectedHeap::unregister_nmethod(nmethod* nm) {
  guarantee(nm != NULL, "sanity");
  UnregisterNMethodOopClosure reg_cl(this, nm);
  nm->oops_do(&reg_cl, true);
}

void G1CollectedHeap::purge_code_root_memory() {
  double purge_start = os::elapsedTime();
  G1CodeRootSet::purge();
  double purge_time_ms = (os::elapsedTime() - purge_start) * 1000.0;
  g1_policy()->phase_times()->record_strong_code_root_purge_time(purge_time_ms);
}

class RebuildStrongCodeRootClosure: public CodeBlobClosure {
  G1CollectedHeap* _g1h;

public:
  RebuildStrongCodeRootClosure(G1CollectedHeap* g1h) :
    _g1h(g1h) {}

  void do_code_blob(CodeBlob* cb) {
    nmethod* nm = (cb != NULL) ? cb->as_nmethod_or_null() : NULL;
    if (nm == NULL) {
      return;
    }

    if (ScavengeRootsInCode) {
      _g1h->register_nmethod(nm);
    }
  }
};

void G1CollectedHeap::rebuild_strong_code_roots() {
  RebuildStrongCodeRootClosure blob_cl(this);
  CodeCache::blobs_do(&blob_cl);
}
