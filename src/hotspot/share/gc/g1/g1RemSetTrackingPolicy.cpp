/*
 * Copyright (c) 2018, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/collectionSetChooser.hpp"
#include "gc/g1/g1RemSetTrackingPolicy.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#include "runtime/safepoint.hpp"

bool G1RemSetTrackingPolicy::is_interesting_humongous_region(HeapRegion* r) const {
  return r->is_humongous() && oop(r->humongous_start_region()->bottom())->is_typeArray();
}

bool G1RemSetTrackingPolicy::needs_scan_for_rebuild(HeapRegion* r) const {
  // All non-free, non-young, non-closed archive regions need to be scanned for references;
  // At every gc we gather references to other regions in young, and closed archive
  // regions by definition do not have references going outside the closed archive.
  // Free regions trivially do not need scanning because they do not contain live
  // objects.
  return !(r->is_young() || r->is_closed_archive() || r->is_free());
}

void G1RemSetTrackingPolicy::update_at_allocate(HeapRegion* r) {
  if (r->is_young()) {
    // Always collect remembered set for young regions.
    r->rem_set()->set_state_complete();
  } else if (r->is_humongous()) {
    // Collect remembered sets for humongous regions by default to allow eager reclaim.
    r->rem_set()->set_state_complete();
  } else if (r->is_archive()) {
    // Archive regions never move ever. So never build remembered sets for them.
    r->rem_set()->set_state_empty();
  } else if (r->is_old()) {
    // By default, do not create remembered set for new old regions.
    r->rem_set()->set_state_empty();
  } else {
    guarantee(false, "Unhandled region %u with heap region type %s", r->hrm_index(), r->get_type_str());
  }
}

void G1RemSetTrackingPolicy::update_at_free(HeapRegion* r) {
  /* nothing to do */
}

bool G1RemSetTrackingPolicy::update_before_rebuild(HeapRegion* r, size_t live_bytes) {
  assert(SafepointSynchronize::is_at_safepoint(), "should be at safepoint");

  bool selected_for_rebuild = false;

  // Only consider updating the remembered set for old gen regions - excluding archive regions
  // which never move (but are "Old" regions).
  if (r->is_old_or_humongous() && !r->is_archive()) {
    size_t between_ntams_and_top = (r->top() - r->next_top_at_mark_start()) * HeapWordSize;
    size_t total_live_bytes = live_bytes + between_ntams_and_top;
    // Completely free regions after rebuild are of no interest wrt rebuilding the
    // remembered set.
    assert(!r->rem_set()->is_updating(), "Remembered set of region %u is updating before rebuild", r->hrm_index());
    // To be of interest for rebuilding the remembered set the following must apply:
    // - They must contain some live data in them.
    // - We always try to update the remembered sets of humongous regions containing
    // type arrays if they are empty as they might have been reset after full gc.
    // - Only need to rebuild non-complete remembered sets.
    // - Otherwise only add those old gen regions which occupancy is low enough that there
    // is a chance that we will ever evacuate them in the mixed gcs.
    if ((total_live_bytes > 0) &&
        (is_interesting_humongous_region(r) || CollectionSetChooser::region_occupancy_low_enough_for_evac(total_live_bytes)) &&
        !r->rem_set()->is_tracked()) {

      r->rem_set()->set_state_updating();
      selected_for_rebuild = true;
    }
    log_trace(gc, remset, tracking)("Before rebuild region %u "
                                    "(ntams: " PTR_FORMAT ") "
                                    "total_live_bytes " SIZE_FORMAT " "
                                    "selected %s "
                                    "(live_bytes " SIZE_FORMAT " "
                                    "next_marked " SIZE_FORMAT " "
                                    "marked " SIZE_FORMAT " "
                                    "type %s)",
                                    r->hrm_index(),
                                    p2i(r->next_top_at_mark_start()),
                                    total_live_bytes,
                                    BOOL_TO_STR(selected_for_rebuild),
                                    live_bytes,
                                    r->next_marked_bytes(),
                                    r->marked_bytes(),
                                    r->get_type_str());
  }

  return selected_for_rebuild;
}

void G1RemSetTrackingPolicy::update_after_rebuild(HeapRegion* r) {
  assert(SafepointSynchronize::is_at_safepoint(), "should be at safepoint");

  if (r->is_old_or_humongous()) {
    if (r->rem_set()->is_updating()) {
      r->rem_set()->set_state_complete();
    }
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    // We can drop remembered sets of humongous regions that have a too large remembered set:
    // We will never try to eagerly reclaim or move them anyway until the next concurrent
    // cycle as e.g. remembered set entries will always be added.
    if (r->is_starts_humongous() && !g1h->is_potential_eager_reclaim_candidate(r)) {
      // Handle HC regions with the HS region.
      uint const size_in_regions = (uint)g1h->humongous_obj_size_in_regions(oop(r->bottom())->size());
      uint const region_idx = r->hrm_index();
      for (uint j = region_idx; j < (region_idx + size_in_regions); j++) {
        HeapRegion* const cur = g1h->region_at(j);
        assert(!cur->is_continues_humongous() || cur->rem_set()->is_empty(),
               "Continues humongous region %u remset should be empty", j);
        cur->rem_set()->clear_locked(true /* only_cardset */);
      }
    }
    G1ConcurrentMark* cm = G1CollectedHeap::heap()->concurrent_mark();
    log_trace(gc, remset, tracking)("After rebuild region %u "
                                    "(ntams " PTR_FORMAT " "
                                    "liveness " SIZE_FORMAT " "
                                    "next_marked_bytes " SIZE_FORMAT " "
                                    "remset occ " SIZE_FORMAT " "
                                    "size " SIZE_FORMAT ")",
                                    r->hrm_index(),
                                    p2i(r->next_top_at_mark_start()),
                                    cm->liveness(r->hrm_index()) * HeapWordSize,
                                    r->next_marked_bytes(),
                                    r->rem_set()->occupied_locked(),
                                    r->rem_set()->mem_size());
  }
}

