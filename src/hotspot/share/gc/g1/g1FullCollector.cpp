/*
 * Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
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
#include "code/codeCache.hpp"
#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1CollectorPolicy.hpp"
#include "gc/g1/g1FullCollector.hpp"
#include "gc/g1/g1FullGCAdjustTask.hpp"
#include "gc/g1/g1FullGCCompactTask.hpp"
#include "gc/g1/g1FullGCMarker.inline.hpp"
#include "gc/g1/g1FullGCMarkTask.hpp"
#include "gc/g1/g1FullGCPrepareTask.hpp"
#include "gc/g1/g1FullGCReferenceProcessorExecutor.hpp"
#include "gc/g1/g1FullGCScope.hpp"
#include "gc/g1/g1OopClosures.hpp"
#include "gc/g1/g1StringDedup.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/preservedMarks.hpp"
#include "gc/shared/referenceProcessor.hpp"
#include "gc/shared/weakProcessor.hpp"
#include "logging/log.hpp"
#include "runtime/biasedLocking.hpp"
#include "utilities/debug.hpp"

static void clear_and_activate_derived_pointers() {
#if COMPILER2_OR_JVMCI
  DerivedPointerTable::clear();
#endif
}

static void deactivate_derived_pointers() {
#if COMPILER2_OR_JVMCI
  DerivedPointerTable::set_active(false);
#endif
}

static void update_derived_pointers() {
#if COMPILER2_OR_JVMCI
  DerivedPointerTable::update_pointers();
#endif
}

G1FullCollector::G1FullCollector(G1FullGCScope* scope,
                                 ReferenceProcessor* reference_processor,
                                 G1CMBitMap* bitmap,
                                 uint workers) :
    _scope(scope),
    _num_workers(workers),
    _mark_bitmap(bitmap),
    _oop_queue_set(_num_workers),
    _array_queue_set(_num_workers),
    _preserved_marks_set(true),
    _reference_processor(reference_processor),
    _serial_compaction_point(),
    _is_alive(_mark_bitmap),
    _is_alive_mutator(_reference_processor, &_is_alive) {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at a safepoint");

  _preserved_marks_set.init(_num_workers);
  _markers = NEW_C_HEAP_ARRAY(G1FullGCMarker*, _num_workers, mtGC);
  _compaction_points = NEW_C_HEAP_ARRAY(G1FullGCCompactionPoint*, _num_workers, mtGC);
  for (uint i = 0; i < _num_workers; i++) {
    _markers[i] = new G1FullGCMarker(i, _preserved_marks_set.get(i), mark_bitmap());
    _compaction_points[i] = new G1FullGCCompactionPoint();
    _oop_queue_set.register_queue(i, marker(i)->oop_stack());
    _array_queue_set.register_queue(i, marker(i)->objarray_stack());
  }
}

G1FullCollector::~G1FullCollector() {
  for (uint i = 0; i < _num_workers; i++) {
    delete _markers[i];
    delete _compaction_points[i];
  }
  FREE_C_HEAP_ARRAY(G1FullGCMarker*, _markers);
  FREE_C_HEAP_ARRAY(G1FullGCCompactionPoint*, _compaction_points);
}

void G1FullCollector::prepare_collection() {
  _reference_processor->enable_discovery();
  _reference_processor->setup_policy(scope()->should_clear_soft_refs());

  // When collecting the permanent generation Method*s may be moving,
  // so we either have to flush all bcp data or convert it into bci.
  CodeCache::gc_prologue();

  // We should save the marks of the currently locked biased monitors.
  // The marking doesn't preserve the marks of biased objects.
  BiasedLocking::preserve_marks();

  // Clear and activate derived pointer collection.
  clear_and_activate_derived_pointers();
}

void G1FullCollector::collect() {
  phase1_mark_live_objects();
  verify_after_marking();

  // Don't add any more derived pointers during later phases
  deactivate_derived_pointers();

  phase2_prepare_compaction();

  phase3_adjust_pointers();

  phase4_do_compaction();
}

void G1FullCollector::complete_collection() {
  // Restore all marks.
  restore_marks();

  // When the pointers have been adjusted and moved, we can
  // update the derived pointer table.
  update_derived_pointers();

  BiasedLocking::restore_marks();
  CodeCache::gc_epilogue();
  JvmtiExport::gc_epilogue();
}

void G1FullCollector::phase1_mark_live_objects() {
  // Recursively traverse all live objects and mark them.
  GCTraceTime(Info, gc, phases) info("Phase 1: Mark live objects", scope()->timer());

  // Do the actual marking.
  G1FullGCMarkTask marking_task(this);
  run_task(&marking_task);

  // Process references discovered during marking.
  G1FullGCReferenceProcessingExecutor reference_processing(this);
  reference_processing.execute(scope()->timer(), scope()->tracer());

  // Weak oops cleanup.
  {
    GCTraceTime(Debug, gc, phases) trace("Phase 1: Weak Processing", scope()->timer());
    WeakProcessor::weak_oops_do(&_is_alive, &do_nothing_cl);
  }

  // Class unloading and cleanup.
  if (ClassUnloading) {
    GCTraceTime(Debug, gc, phases) debug("Phase 1: Class Unloading and Cleanup", scope()->timer());
    // Unload classes and purge the SystemDictionary.
    bool purged_class = SystemDictionary::do_unloading(&_is_alive, scope()->timer());
    G1CollectedHeap::heap()->complete_cleaning(&_is_alive, purged_class);
  } else {
    GCTraceTime(Debug, gc, phases) debug("Phase 1: String and Symbol Tables Cleanup", scope()->timer());
    // If no class unloading just clean out strings and symbols.
    G1CollectedHeap::heap()->partial_cleaning(&_is_alive, true, true, G1StringDedup::is_enabled());
  }

  scope()->tracer()->report_object_count_after_gc(&_is_alive);
}

void G1FullCollector::prepare_compaction_common() {
  G1FullGCPrepareTask task(this);
  run_task(&task);

  // To avoid OOM when there is memory left.
  if (!task.has_freed_regions()) {
    task.prepare_serial_compaction();
  }
}

void G1FullCollector::phase2_prepare_compaction() {
  GCTraceTime(Info, gc, phases) info("Phase 2: Prepare for compaction", scope()->timer());
  prepare_compaction_ext(); // Will call prepare_compaction_common() above.
}

void G1FullCollector::phase3_adjust_pointers() {
  // Adjust the pointers to reflect the new locations
  GCTraceTime(Info, gc, phases) info("Phase 3: Adjust pointers and remembered sets", scope()->timer());

  G1FullGCAdjustTask task(this);
  run_task(&task);
}

void G1FullCollector::phase4_do_compaction() {
  // Compact the heap using the compaction queues created in phase 2.
  GCTraceTime(Info, gc, phases) info("Phase 4: Compact heap", scope()->timer());
  G1FullGCCompactTask task(this);
  run_task(&task);

  // Serial compact to avoid OOM when very few free regions.
  if (serial_compaction_point()->has_regions()) {
    task.serial_compaction();
  }
}

void G1FullCollector::restore_marks() {
  SharedRestorePreservedMarksTaskExecutor task_executor(G1CollectedHeap::heap()->workers());
  _preserved_marks_set.restore(&task_executor);
  _preserved_marks_set.reclaim();
}

void G1FullCollector::run_task(AbstractGangTask* task) {
  G1CollectedHeap::heap()->workers()->run_task(task, _num_workers);
}

void G1FullCollector::verify_after_marking() {
  if (!VerifyDuringGC) {
    //Only do verification if VerifyDuringGC is set.
    return;
  }

  HandleMark hm;  // handle scope
#if COMPILER2_OR_JVMCI
  DerivedPointerTableDeactivate dpt_deact;
#endif
  G1CollectedHeap::heap()->prepare_for_verify();
  // Note: we can verify only the heap here. When an object is
  // marked, the previous value of the mark word (including
  // identity hash values, ages, etc) is preserved, and the mark
  // word is set to markOop::marked_value - effectively removing
  // any hash values from the mark word. These hash values are
  // used when verifying the dictionaries and so removing them
  // from the mark word can make verification of the dictionaries
  // fail. At the end of the GC, the original mark word values
  // (including hash values) are restored to the appropriate
  // objects.
  GCTraceTime(Info, gc, verify)("During GC (full)");
  G1CollectedHeap::heap()->verify(VerifyOption_G1UseFullMarking);
}
