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
#include "gc/g1/dirtyCardQueue.hpp"
#include "gc/g1/g1BlockOffsetTable.inline.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1ConcurrentRefine.hpp"
#include "gc/g1/g1FromCardCache.hpp"
#include "gc/g1/g1GCPhaseTimes.hpp"
#include "gc/g1/g1HotCardCache.hpp"
#include "gc/g1/g1OopClosures.inline.hpp"
#include "gc/g1/g1RemSet.hpp"
#include "gc/g1/g1SATBCardTableModRefBS.inline.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/g1/heapRegionManager.inline.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "memory/iterator.hpp"
#include "memory/resourceArea.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/align.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/intHisto.hpp"
#include "utilities/stack.inline.hpp"

// Collects information about the overall remembered set scan progress during an evacuation.
class G1RemSetScanState : public CHeapObj<mtGC> {
private:
  class G1ClearCardTableTask : public AbstractGangTask {
    G1CollectedHeap* _g1h;
    uint* _dirty_region_list;
    size_t _num_dirty_regions;
    size_t _chunk_length;

    size_t volatile _cur_dirty_regions;
  public:
    G1ClearCardTableTask(G1CollectedHeap* g1h,
                         uint* dirty_region_list,
                         size_t num_dirty_regions,
                         size_t chunk_length) :
      AbstractGangTask("G1 Clear Card Table Task"),
      _g1h(g1h),
      _dirty_region_list(dirty_region_list),
      _num_dirty_regions(num_dirty_regions),
      _chunk_length(chunk_length),
      _cur_dirty_regions(0) {

      assert(chunk_length > 0, "must be");
    }

    static size_t chunk_size() { return M; }

    void work(uint worker_id) {
      G1SATBCardTableModRefBS* ct_bs = _g1h->g1_barrier_set();

      while (_cur_dirty_regions < _num_dirty_regions) {
        size_t next = Atomic::add(_chunk_length, &_cur_dirty_regions) - _chunk_length;
        size_t max = MIN2(next + _chunk_length, _num_dirty_regions);

        for (size_t i = next; i < max; i++) {
          HeapRegion* r = _g1h->region_at(_dirty_region_list[i]);
          if (!r->is_survivor()) {
            ct_bs->clear(MemRegion(r->bottom(), r->end()));
          }
        }
      }
    }
  };

  size_t _max_regions;

  // Scan progress for the remembered set of a single region. Transitions from
  // Unclaimed -> Claimed -> Complete.
  // At each of the transitions the thread that does the transition needs to perform
  // some special action once. This is the reason for the extra "Claimed" state.
  typedef jint G1RemsetIterState;

  static const G1RemsetIterState Unclaimed = 0; // The remembered set has not been scanned yet.
  static const G1RemsetIterState Claimed = 1;   // The remembered set is currently being scanned.
  static const G1RemsetIterState Complete = 2;  // The remembered set has been completely scanned.

  G1RemsetIterState volatile* _iter_states;
  // The current location where the next thread should continue scanning in a region's
  // remembered set.
  size_t volatile* _iter_claims;

  // Temporary buffer holding the regions we used to store remembered set scan duplicate
  // information. These are also called "dirty". Valid entries are from [0.._cur_dirty_region)
  uint* _dirty_region_buffer;

  typedef jbyte IsDirtyRegionState;
  static const IsDirtyRegionState Clean = 0;
  static const IsDirtyRegionState Dirty = 1;
  // Holds a flag for every region whether it is in the _dirty_region_buffer already
  // to avoid duplicates. Uses jbyte since there are no atomic instructions for bools.
  IsDirtyRegionState* _in_dirty_region_buffer;
  size_t _cur_dirty_region;

  // Creates a snapshot of the current _top values at the start of collection to
  // filter out card marks that we do not want to scan.
  class G1ResetScanTopClosure : public HeapRegionClosure {
  private:
    HeapWord** _scan_top;
  public:
    G1ResetScanTopClosure(HeapWord** scan_top) : _scan_top(scan_top) { }

    virtual bool doHeapRegion(HeapRegion* r) {
      uint hrm_index = r->hrm_index();
      if (!r->in_collection_set() && r->is_old_or_humongous()) {
        _scan_top[hrm_index] = r->top();
      } else {
        _scan_top[hrm_index] = r->bottom();
      }
      return false;
    }
  };

  // For each region, contains the maximum top() value to be used during this garbage
  // collection. Subsumes common checks like filtering out everything but old and
  // humongous regions outside the collection set.
  // This is valid because we are not interested in scanning stray remembered set
  // entries from free or archive regions.
  HeapWord** _scan_top;
public:
  G1RemSetScanState() :
    _max_regions(0),
    _iter_states(NULL),
    _iter_claims(NULL),
    _dirty_region_buffer(NULL),
    _in_dirty_region_buffer(NULL),
    _cur_dirty_region(0),
    _scan_top(NULL) {
  }

  ~G1RemSetScanState() {
    if (_iter_states != NULL) {
      FREE_C_HEAP_ARRAY(G1RemsetIterState, _iter_states);
    }
    if (_iter_claims != NULL) {
      FREE_C_HEAP_ARRAY(size_t, _iter_claims);
    }
    if (_dirty_region_buffer != NULL) {
      FREE_C_HEAP_ARRAY(uint, _dirty_region_buffer);
    }
    if (_in_dirty_region_buffer != NULL) {
      FREE_C_HEAP_ARRAY(IsDirtyRegionState, _in_dirty_region_buffer);
    }
    if (_scan_top != NULL) {
      FREE_C_HEAP_ARRAY(HeapWord*, _scan_top);
    }
  }

  void initialize(uint max_regions) {
    assert(_iter_states == NULL, "Must not be initialized twice");
    assert(_iter_claims == NULL, "Must not be initialized twice");
    _max_regions = max_regions;
    _iter_states = NEW_C_HEAP_ARRAY(G1RemsetIterState, max_regions, mtGC);
    _iter_claims = NEW_C_HEAP_ARRAY(size_t, max_regions, mtGC);
    _dirty_region_buffer = NEW_C_HEAP_ARRAY(uint, max_regions, mtGC);
    _in_dirty_region_buffer = NEW_C_HEAP_ARRAY(IsDirtyRegionState, max_regions, mtGC);
    _scan_top = NEW_C_HEAP_ARRAY(HeapWord*, max_regions, mtGC);
  }

  void reset() {
    for (uint i = 0; i < _max_regions; i++) {
      _iter_states[i] = Unclaimed;
    }

    G1ResetScanTopClosure cl(_scan_top);
    G1CollectedHeap::heap()->heap_region_iterate(&cl);

    memset((void*)_iter_claims, 0, _max_regions * sizeof(size_t));
    memset(_in_dirty_region_buffer, Clean, _max_regions * sizeof(IsDirtyRegionState));
    _cur_dirty_region = 0;
  }

  // Attempt to claim the remembered set of the region for iteration. Returns true
  // if this call caused the transition from Unclaimed to Claimed.
  inline bool claim_iter(uint region) {
    assert(region < _max_regions, "Tried to access invalid region %u", region);
    if (_iter_states[region] != Unclaimed) {
      return false;
    }
    jint res = Atomic::cmpxchg(Claimed, (jint*)(&_iter_states[region]), Unclaimed);
    return (res == Unclaimed);
  }

  // Try to atomically sets the iteration state to "complete". Returns true for the
  // thread that caused the transition.
  inline bool set_iter_complete(uint region) {
    if (iter_is_complete(region)) {
      return false;
    }
    jint res = Atomic::cmpxchg(Complete, (jint*)(&_iter_states[region]), Claimed);
    return (res == Claimed);
  }

  // Returns true if the region's iteration is complete.
  inline bool iter_is_complete(uint region) const {
    assert(region < _max_regions, "Tried to access invalid region %u", region);
    return _iter_states[region] == Complete;
  }

  // The current position within the remembered set of the given region.
  inline size_t iter_claimed(uint region) const {
    assert(region < _max_regions, "Tried to access invalid region %u", region);
    return _iter_claims[region];
  }

  // Claim the next block of cards within the remembered set of the region with
  // step size.
  inline size_t iter_claimed_next(uint region, size_t step) {
    return Atomic::add(step, &_iter_claims[region]) - step;
  }

  void add_dirty_region(uint region) {
    if (_in_dirty_region_buffer[region] == Dirty) {
      return;
    }

    bool marked_as_dirty = Atomic::cmpxchg(Dirty, &_in_dirty_region_buffer[region], Clean) == Clean;
    if (marked_as_dirty) {
      size_t allocated = Atomic::add(1u, &_cur_dirty_region) - 1;
      _dirty_region_buffer[allocated] = region;
    }
  }

  HeapWord* scan_top(uint region_idx) const {
    return _scan_top[region_idx];
  }

  // Clear the card table of "dirty" regions.
  void clear_card_table(WorkGang* workers) {
    if (_cur_dirty_region == 0) {
      return;
    }

    size_t const num_chunks = align_up(_cur_dirty_region * HeapRegion::CardsPerRegion, G1ClearCardTableTask::chunk_size()) / G1ClearCardTableTask::chunk_size();
    uint const num_workers = (uint)MIN2(num_chunks, (size_t)workers->active_workers());
    size_t const chunk_length = G1ClearCardTableTask::chunk_size() / HeapRegion::CardsPerRegion;

    // Iterate over the dirty cards region list.
    G1ClearCardTableTask cl(G1CollectedHeap::heap(), _dirty_region_buffer, _cur_dirty_region, chunk_length);

    log_debug(gc, ergo)("Running %s using %u workers for " SIZE_FORMAT " "
                        "units of work for " SIZE_FORMAT " regions.",
                        cl.name(), num_workers, num_chunks, _cur_dirty_region);
    workers->run_task(&cl, num_workers);

#ifndef PRODUCT
    // Need to synchronize with concurrent cleanup since it needs to
    // finish its card table clearing before we can verify.
    G1CollectedHeap::heap()->wait_while_free_regions_coming();
    G1CollectedHeap::heap()->verifier()->verify_card_table_cleanup();
#endif
  }
};

G1RemSet::G1RemSet(G1CollectedHeap* g1,
                   CardTableModRefBS* ct_bs,
                   G1HotCardCache* hot_card_cache) :
  _g1(g1),
  _scan_state(new G1RemSetScanState()),
  _num_conc_refined_cards(0),
  _ct_bs(ct_bs),
  _g1p(_g1->g1_policy()),
  _hot_card_cache(hot_card_cache),
  _prev_period_summary() {
}

G1RemSet::~G1RemSet() {
  if (_scan_state != NULL) {
    delete _scan_state;
  }
}

uint G1RemSet::num_par_rem_sets() {
  return MAX2(DirtyCardQueueSet::num_par_ids() + G1ConcurrentRefine::thread_num(), ParallelGCThreads);
}

void G1RemSet::initialize(size_t capacity, uint max_regions) {
  G1FromCardCache::initialize(num_par_rem_sets(), max_regions);
  _scan_state->initialize(max_regions);
  {
    GCTraceTime(Debug, gc, marking)("Initialize Card Live Data");
    _card_live_data.initialize(capacity, max_regions);
  }
  if (G1PretouchAuxiliaryMemory) {
    GCTraceTime(Debug, gc, marking)("Pre-Touch Card Live Data");
    _card_live_data.pretouch();
  }
}

G1ScanRSForRegionClosure::G1ScanRSForRegionClosure(G1RemSetScanState* scan_state,
                                                   G1ScanObjsDuringScanRSClosure* scan_obj_on_card,
                                                   CodeBlobClosure* code_root_cl,
                                                   uint worker_i) :
  _scan_state(scan_state),
  _scan_objs_on_card_cl(scan_obj_on_card),
  _code_root_cl(code_root_cl),
  _strong_code_root_scan_time_sec(0.0),
  _cards_claimed(0),
  _cards_scanned(0),
  _cards_skipped(0),
  _worker_i(worker_i) {
  _g1h = G1CollectedHeap::heap();
  _bot = _g1h->bot();
  _ct_bs = _g1h->g1_barrier_set();
}

void G1ScanRSForRegionClosure::scan_card(MemRegion mr, uint region_idx_for_card) {
  HeapRegion* const card_region = _g1h->region_at(region_idx_for_card);
  _scan_objs_on_card_cl->set_region(card_region);
  card_region->oops_on_card_seq_iterate_careful<true>(mr, _scan_objs_on_card_cl);
  _cards_scanned++;
}

void G1ScanRSForRegionClosure::scan_strong_code_roots(HeapRegion* r) {
  double scan_start = os::elapsedTime();
  r->strong_code_roots_do(_code_root_cl);
  _strong_code_root_scan_time_sec += (os::elapsedTime() - scan_start);
}

void G1ScanRSForRegionClosure::claim_card(size_t card_index, const uint region_idx_for_card){
  _ct_bs->set_card_claimed(card_index);
  _scan_state->add_dirty_region(region_idx_for_card);
}

bool G1ScanRSForRegionClosure::doHeapRegion(HeapRegion* r) {
  assert(r->in_collection_set(), "should only be called on elements of CS.");
  uint region_idx = r->hrm_index();

  if (_scan_state->iter_is_complete(region_idx)) {
    return false;
  }
  if (_scan_state->claim_iter(region_idx)) {
    // If we ever free the collection set concurrently, we should also
    // clear the card table concurrently therefore we won't need to
    // add regions of the collection set to the dirty cards region.
    _scan_state->add_dirty_region(region_idx);
  }

  // We claim cards in blocks so as to reduce the contention.
  size_t const block_size = G1RSetScanBlockSize;

  HeapRegionRemSetIterator iter(r->rem_set());
  size_t card_index;

  size_t claimed_card_block = _scan_state->iter_claimed_next(region_idx, block_size);
  for (size_t current_card = 0; iter.has_next(card_index); current_card++) {
    if (current_card >= claimed_card_block + block_size) {
      claimed_card_block = _scan_state->iter_claimed_next(region_idx, block_size);
    }
    if (current_card < claimed_card_block) {
      _cards_skipped++;
      continue;
    }
    _cards_claimed++;

    // If the card is dirty, then G1 will scan it during Update RS.
    if (_ct_bs->is_card_claimed(card_index) || _ct_bs->is_card_dirty(card_index)) {
      continue;
    }

    HeapWord* const card_start = _g1h->bot()->address_for_index(card_index);
    uint const region_idx_for_card = _g1h->addr_to_region(card_start);

    assert(_g1h->region_at(region_idx_for_card)->is_in_reserved(card_start),
           "Card start " PTR_FORMAT " to scan outside of region %u", p2i(card_start), _g1h->region_at(region_idx_for_card)->hrm_index());
    HeapWord* const top = _scan_state->scan_top(region_idx_for_card);
    if (card_start >= top) {
      continue;
    }

    // We claim lazily (so races are possible but they're benign), which reduces the
    // number of duplicate scans (the rsets of the regions in the cset can intersect).
    // Claim the card after checking bounds above: the remembered set may contain
    // random cards into current survivor, and we would then have an incorrectly
    // claimed card in survivor space. Card table clear does not reset the card table
    // of survivor space regions.
    claim_card(card_index, region_idx_for_card);

    MemRegion const mr(card_start, MIN2(card_start + BOTConstants::N_words, top));

    scan_card(mr, region_idx_for_card);
  }
  if (_scan_state->set_iter_complete(region_idx)) {
    // Scan the strong code root list attached to the current region
    scan_strong_code_roots(r);
  }
  return false;
}

void G1RemSet::scan_rem_set(G1ParScanThreadState* pss,
                            CodeBlobClosure* heap_region_codeblobs,
                            uint worker_i) {
  double rs_time_start = os::elapsedTime();

  G1ScanObjsDuringScanRSClosure scan_cl(_g1, pss);
  G1ScanRSForRegionClosure cl(_scan_state, &scan_cl, heap_region_codeblobs, worker_i);
  _g1->collection_set_iterate_from(&cl, worker_i);

  double scan_rs_time_sec = (os::elapsedTime() - rs_time_start) -
                             cl.strong_code_root_scan_time_sec();

  G1GCPhaseTimes* p = _g1p->phase_times();

  p->record_time_secs(G1GCPhaseTimes::ScanRS, worker_i, scan_rs_time_sec);
  p->record_thread_work_item(G1GCPhaseTimes::ScanRS, worker_i, cl.cards_scanned(), G1GCPhaseTimes::ScanRSScannedCards);
  p->record_thread_work_item(G1GCPhaseTimes::ScanRS, worker_i, cl.cards_claimed(), G1GCPhaseTimes::ScanRSClaimedCards);
  p->record_thread_work_item(G1GCPhaseTimes::ScanRS, worker_i, cl.cards_skipped(), G1GCPhaseTimes::ScanRSSkippedCards);

  p->record_time_secs(G1GCPhaseTimes::CodeRoots, worker_i, cl.strong_code_root_scan_time_sec());
}

// Closure used for updating rem sets. Only called during an evacuation pause.
class G1RefineCardClosure: public CardTableEntryClosure {
  G1RemSet* _g1rs;
  G1ScanObjsDuringUpdateRSClosure* _update_rs_cl;

  size_t _cards_scanned;
  size_t _cards_skipped;
public:
  G1RefineCardClosure(G1CollectedHeap* g1h, G1ScanObjsDuringUpdateRSClosure* update_rs_cl) :
    _g1rs(g1h->g1_rem_set()), _update_rs_cl(update_rs_cl), _cards_scanned(0), _cards_skipped(0)
  {}

  bool do_card_ptr(jbyte* card_ptr, uint worker_i) {
    // The only time we care about recording cards that
    // contain references that point into the collection set
    // is during RSet updating within an evacuation pause.
    // In this case worker_i should be the id of a GC worker thread.
    assert(SafepointSynchronize::is_at_safepoint(), "not during an evacuation pause");

    bool card_scanned = _g1rs->refine_card_during_gc(card_ptr, _update_rs_cl);

    if (card_scanned) {
      _cards_scanned++;
    } else {
      _cards_skipped++;
    }
    return true;
  }

  size_t cards_scanned() const { return _cards_scanned; }
  size_t cards_skipped() const { return _cards_skipped; }
};

void G1RemSet::update_rem_set(G1ParScanThreadState* pss, uint worker_i) {
  G1ScanObjsDuringUpdateRSClosure update_rs_cl(_g1, pss, worker_i);
  G1RefineCardClosure refine_card_cl(_g1, &update_rs_cl);

  G1GCParPhaseTimesTracker x(_g1p->phase_times(), G1GCPhaseTimes::UpdateRS, worker_i);
  if (G1HotCardCache::default_use_cache()) {
    // Apply the closure to the entries of the hot card cache.
    G1GCParPhaseTimesTracker y(_g1p->phase_times(), G1GCPhaseTimes::ScanHCC, worker_i);
    _g1->iterate_hcc_closure(&refine_card_cl, worker_i);
  }
  // Apply the closure to all remaining log entries.
  _g1->iterate_dirty_card_closure(&refine_card_cl, worker_i);

  G1GCPhaseTimes* p = _g1p->phase_times();
  p->record_thread_work_item(G1GCPhaseTimes::UpdateRS, worker_i, refine_card_cl.cards_scanned(), G1GCPhaseTimes::UpdateRSScannedCards);
  p->record_thread_work_item(G1GCPhaseTimes::UpdateRS, worker_i, refine_card_cl.cards_skipped(), G1GCPhaseTimes::UpdateRSSkippedCards);
}

void G1RemSet::cleanupHRRS() {
  HeapRegionRemSet::cleanup();
}

void G1RemSet::oops_into_collection_set_do(G1ParScanThreadState* pss,
                                           CodeBlobClosure* heap_region_codeblobs,
                                           uint worker_i) {
  update_rem_set(pss, worker_i);
  scan_rem_set(pss, heap_region_codeblobs, worker_i);;
}

void G1RemSet::prepare_for_oops_into_collection_set_do() {
  DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
  dcqs.concatenate_logs();

  _scan_state->reset();
}

void G1RemSet::cleanup_after_oops_into_collection_set_do() {
  G1GCPhaseTimes* phase_times = _g1->g1_policy()->phase_times();

  // Set all cards back to clean.
  double start = os::elapsedTime();
  _scan_state->clear_card_table(_g1->workers());
  phase_times->record_clear_ct_time((os::elapsedTime() - start) * 1000.0);
}

class G1ScrubRSClosure: public HeapRegionClosure {
  G1CollectedHeap* _g1h;
  G1CardLiveData* _live_data;
public:
  G1ScrubRSClosure(G1CardLiveData* live_data) :
    _g1h(G1CollectedHeap::heap()),
    _live_data(live_data) { }

  bool doHeapRegion(HeapRegion* r) {
    if (!r->is_continues_humongous()) {
      r->rem_set()->scrub(_live_data);
    }
    return false;
  }
};

void G1RemSet::scrub(uint worker_num, HeapRegionClaimer *hrclaimer) {
  G1ScrubRSClosure scrub_cl(&_card_live_data);
  _g1->heap_region_par_iterate_from_worker_offset(&scrub_cl, hrclaimer, worker_num);
}

inline void check_card_ptr(jbyte* card_ptr, CardTableModRefBS* ct_bs) {
#ifdef ASSERT
  G1CollectedHeap* g1 = G1CollectedHeap::heap();
  assert(g1->is_in_exact(ct_bs->addr_for(card_ptr)),
         "Card at " PTR_FORMAT " index " SIZE_FORMAT " representing heap at " PTR_FORMAT " (%u) must be in committed heap",
         p2i(card_ptr),
         ct_bs->index_for(ct_bs->addr_for(card_ptr)),
         p2i(ct_bs->addr_for(card_ptr)),
         g1->addr_to_region(ct_bs->addr_for(card_ptr)));
#endif
}

void G1RemSet::refine_card_concurrently(jbyte* card_ptr,
                                        uint worker_i) {
  assert(!_g1->is_gc_active(), "Only call concurrently");

  check_card_ptr(card_ptr, _ct_bs);

  // If the card is no longer dirty, nothing to do.
  if (*card_ptr != CardTableModRefBS::dirty_card_val()) {
    return;
  }

  // Construct the region representing the card.
  HeapWord* start = _ct_bs->addr_for(card_ptr);
  // And find the region containing it.
  HeapRegion* r = _g1->heap_region_containing(start);

  // This check is needed for some uncommon cases where we should
  // ignore the card.
  //
  // The region could be young.  Cards for young regions are
  // distinctly marked (set to g1_young_gen), so the post-barrier will
  // filter them out.  However, that marking is performed
  // concurrently.  A write to a young object could occur before the
  // card has been marked young, slipping past the filter.
  //
  // The card could be stale, because the region has been freed since
  // the card was recorded. In this case the region type could be
  // anything.  If (still) free or (reallocated) young, just ignore
  // it.  If (reallocated) old or humongous, the later card trimming
  // and additional checks in iteration may detect staleness.  At
  // worst, we end up processing a stale card unnecessarily.
  //
  // In the normal (non-stale) case, the synchronization between the
  // enqueueing of the card and processing it here will have ensured
  // we see the up-to-date region type here.
  if (!r->is_old_or_humongous()) {
    return;
  }

  // While we are processing RSet buffers during the collection, we
  // actually don't want to scan any cards on the collection set,
  // since we don't want to update remembered sets with entries that
  // point into the collection set, given that live objects from the
  // collection set are about to move and such entries will be stale
  // very soon. This change also deals with a reliability issue which
  // involves scanning a card in the collection set and coming across
  // an array that was being chunked and looking malformed. Note,
  // however, that if evacuation fails, we have to scan any objects
  // that were not moved and create any missing entries.
  if (r->in_collection_set()) {
    return;
  }

  // The result from the hot card cache insert call is either:
  //   * pointer to the current card
  //     (implying that the current card is not 'hot'),
  //   * null
  //     (meaning we had inserted the card ptr into the "hot" card cache,
  //     which had some headroom),
  //   * a pointer to a "hot" card that was evicted from the "hot" cache.
  //

  if (_hot_card_cache->use_cache()) {
    assert(!SafepointSynchronize::is_at_safepoint(), "sanity");

    const jbyte* orig_card_ptr = card_ptr;
    card_ptr = _hot_card_cache->insert(card_ptr);
    if (card_ptr == NULL) {
      // There was no eviction. Nothing to do.
      return;
    } else if (card_ptr != orig_card_ptr) {
      // Original card was inserted and an old card was evicted.
      start = _ct_bs->addr_for(card_ptr);
      r = _g1->heap_region_containing(start);

      // Check whether the region formerly in the cache should be
      // ignored, as discussed earlier for the original card.  The
      // region could have been freed while in the cache.  The cset is
      // not relevant here, since we're in concurrent phase.
      if (!r->is_old_or_humongous()) {
        return;
      }
    } // Else we still have the original card.
  }

  // Trim the region designated by the card to what's been allocated
  // in the region.  The card could be stale, or the card could cover
  // (part of) an object at the end of the allocated space and extend
  // beyond the end of allocation.

  // Non-humongous objects are only allocated in the old-gen during
  // GC, so if region is old then top is stable.  Humongous object
  // allocation sets top last; if top has not yet been set, this is
  // a stale card and we'll end up with an empty intersection.  If
  // this is not a stale card, the synchronization between the
  // enqueuing of the card and processing it here will have ensured
  // we see the up-to-date top here.
  HeapWord* scan_limit = r->top();

  if (scan_limit <= start) {
    // If the trimmed region is empty, the card must be stale.
    return;
  }

  // Okay to clean and process the card now.  There are still some
  // stale card cases that may be detected by iteration and dealt with
  // as iteration failure.
  *const_cast<volatile jbyte*>(card_ptr) = CardTableModRefBS::clean_card_val();

  // This fence serves two purposes.  First, the card must be cleaned
  // before processing the contents.  Second, we can't proceed with
  // processing until after the read of top, for synchronization with
  // possibly concurrent humongous object allocation.  It's okay that
  // reading top and reading type were racy wrto each other.  We need
  // both set, in any order, to proceed.
  OrderAccess::fence();

  // Don't use addr_for(card_ptr + 1) which can ask for
  // a card beyond the heap.
  HeapWord* end = start + CardTableModRefBS::card_size_in_words;
  MemRegion dirty_region(start, MIN2(scan_limit, end));
  assert(!dirty_region.is_empty(), "sanity");

  G1ConcurrentRefineOopClosure conc_refine_cl(_g1, worker_i);

  bool card_processed =
    r->oops_on_card_seq_iterate_careful<false>(dirty_region, &conc_refine_cl);

  // If unable to process the card then we encountered an unparsable
  // part of the heap (e.g. a partially allocated object) while
  // processing a stale card.  Despite the card being stale, redirty
  // and re-enqueue, because we've already cleaned the card.  Without
  // this we could incorrectly discard a non-stale card.
  if (!card_processed) {
    // The card might have gotten re-dirtied and re-enqueued while we
    // worked.  (In fact, it's pretty likely.)
    if (*card_ptr != CardTableModRefBS::dirty_card_val()) {
      *card_ptr = CardTableModRefBS::dirty_card_val();
      MutexLockerEx x(Shared_DirtyCardQ_lock,
                      Mutex::_no_safepoint_check_flag);
      DirtyCardQueue* sdcq =
        JavaThread::dirty_card_queue_set().shared_dirty_card_queue();
      sdcq->enqueue(card_ptr);
    }
  } else {
    _num_conc_refined_cards++; // Unsynchronized update, only used for logging.
  }
}

bool G1RemSet::refine_card_during_gc(jbyte* card_ptr,
                                     G1ScanObjsDuringUpdateRSClosure* update_rs_cl) {
  assert(_g1->is_gc_active(), "Only call during GC");

  check_card_ptr(card_ptr, _ct_bs);

  // If the card is no longer dirty, nothing to do. This covers cards that were already
  // scanned as parts of the remembered sets.
  if (*card_ptr != CardTableModRefBS::dirty_card_val()) {
    return false;
  }

  // We claim lazily (so races are possible but they're benign), which reduces the
  // number of potential duplicate scans (multiple threads may enqueue the same card twice).
  *card_ptr = CardTableModRefBS::clean_card_val() | CardTableModRefBS::claimed_card_val();

  // Construct the region representing the card.
  HeapWord* card_start = _ct_bs->addr_for(card_ptr);
  // And find the region containing it.
  uint const card_region_idx = _g1->addr_to_region(card_start);

  _scan_state->add_dirty_region(card_region_idx);
  HeapWord* scan_limit = _scan_state->scan_top(card_region_idx);
  if (scan_limit <= card_start) {
    // If the card starts above the area in the region containing objects to scan, skip it.
    return false;
  }

  // Don't use addr_for(card_ptr + 1) which can ask for
  // a card beyond the heap.
  HeapWord* card_end = card_start + CardTableModRefBS::card_size_in_words;
  MemRegion dirty_region(card_start, MIN2(scan_limit, card_end));
  assert(!dirty_region.is_empty(), "sanity");

  HeapRegion* const card_region = _g1->region_at(card_region_idx);
  update_rs_cl->set_region(card_region);
  bool card_processed = card_region->oops_on_card_seq_iterate_careful<true>(dirty_region, update_rs_cl);
  assert(card_processed, "must be");
  return true;
}

void G1RemSet::print_periodic_summary_info(const char* header, uint period_count) {
  if ((G1SummarizeRSetStatsPeriod > 0) && log_is_enabled(Trace, gc, remset) &&
      (period_count % G1SummarizeRSetStatsPeriod == 0)) {

    G1RemSetSummary current(this);
    _prev_period_summary.subtract_from(&current);

    Log(gc, remset) log;
    log.trace("%s", header);
    ResourceMark rm;
    LogStream ls(log.trace());
    _prev_period_summary.print_on(&ls);

    _prev_period_summary.set(&current);
  }
}

void G1RemSet::print_summary_info() {
  Log(gc, remset, exit) log;
  if (log.is_trace()) {
    log.trace(" Cumulative RS summary");
    G1RemSetSummary current(this);
    ResourceMark rm;
    LogStream ls(log.trace());
    current.print_on(&ls);
  }
}

void G1RemSet::create_card_live_data(WorkGang* workers, G1CMBitMap* mark_bitmap) {
  _card_live_data.create(workers, mark_bitmap);
}

void G1RemSet::finalize_card_live_data(WorkGang* workers, G1CMBitMap* mark_bitmap) {
  _card_live_data.finalize(workers, mark_bitmap);
}

void G1RemSet::verify_card_live_data(WorkGang* workers, G1CMBitMap* bitmap) {
  _card_live_data.verify(workers, bitmap);
}

void G1RemSet::clear_card_live_data(WorkGang* workers) {
  _card_live_data.clear(workers);
}

#ifdef ASSERT
void G1RemSet::verify_card_live_data_is_clear() {
  _card_live_data.verify_is_clear();
}
#endif
