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

#ifndef SHARE_GC_G1_G1FULLCOLLECTOR_HPP
#define SHARE_GC_G1_G1FULLCOLLECTOR_HPP

#include "gc/g1/g1FullGCCompactionPoint.hpp"
#include "gc/g1/g1FullGCMarker.hpp"
#include "gc/g1/g1FullGCOopClosures.hpp"
#include "gc/g1/g1FullGCScope.hpp"
#include "gc/shared/preservedMarks.hpp"
#include "gc/shared/referenceProcessor.hpp"
#include "gc/shared/taskqueue.hpp"
#include "memory/allocation.hpp"

class AbstractGangTask;
class G1CMBitMap;
class G1FullGCMarker;
class G1FullGCScope;
class G1FullGCCompactionPoint;
class GCMemoryManager;
class ReferenceProcessor;

// The G1FullCollector holds data associated with the current Full GC.
class G1FullCollector : StackObj {
  G1CollectedHeap*          _heap;
  G1FullGCScope             _scope;
  uint                      _num_workers;
  G1FullGCMarker**          _markers;
  G1FullGCCompactionPoint** _compaction_points;
  OopQueueSet               _oop_queue_set;
  ObjArrayTaskQueueSet      _array_queue_set;
  PreservedMarksSet         _preserved_marks_set;
  G1FullGCCompactionPoint   _serial_compaction_point;
  G1IsAliveClosure          _is_alive;
  ReferenceProcessorIsAliveMutator _is_alive_mutator;

public:
  G1FullCollector(G1CollectedHeap* heap, GCMemoryManager* memory_manager, bool explicit_gc, bool clear_soft_refs);
  ~G1FullCollector();

  void prepare_collection();
  void collect();
  void complete_collection();

  G1FullGCScope*           scope() { return &_scope; }
  uint                     workers() { return _num_workers; }
  G1FullGCMarker*          marker(uint id) { return _markers[id]; }
  G1FullGCCompactionPoint* compaction_point(uint id) { return _compaction_points[id]; }
  OopQueueSet*             oop_queue_set() { return &_oop_queue_set; }
  ObjArrayTaskQueueSet*    array_queue_set() { return &_array_queue_set; }
  PreservedMarksSet*       preserved_mark_set() { return &_preserved_marks_set; }
  G1FullGCCompactionPoint* serial_compaction_point() { return &_serial_compaction_point; }
  G1CMBitMap*              mark_bitmap();
  ReferenceProcessor*      reference_processor();

private:
  void phase1_mark_live_objects();
  void phase2_prepare_compaction();
  void phase3_adjust_pointers();
  void phase4_do_compaction();

  void restore_marks();
  void verify_after_marking();

  void run_task(AbstractGangTask* task);

  // Prepare compaction extension support.
  void prepare_compaction_ext();
  void prepare_compaction_common();
};


#endif // SHARE_GC_G1_G1FULLCOLLECTOR_HPP
