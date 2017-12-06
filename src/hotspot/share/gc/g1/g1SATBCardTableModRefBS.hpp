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

#ifndef SHARE_VM_GC_G1_G1SATBCARDTABLEMODREFBS_HPP
#define SHARE_VM_GC_G1_G1SATBCARDTABLEMODREFBS_HPP

#include "gc/g1/g1RegionToSpaceMapper.hpp"
#include "gc/shared/cardTableModRefBS.hpp"
#include "memory/memRegion.hpp"
#include "oops/oop.hpp"
#include "utilities/macros.hpp"

class DirtyCardQueueSet;
class G1SATBCardTableLoggingModRefBS;

// This barrier is specialized to use a logging barrier to support
// snapshot-at-the-beginning marking.

class G1SATBCardTableModRefBS: public CardTableModRefBS {
  friend class VMStructs;
protected:
  enum G1CardValues {
    g1_young_gen = CT_MR_BS_last_reserved << 1
  };

  G1SATBCardTableModRefBS(MemRegion whole_heap, const BarrierSet::FakeRtti& fake_rtti);
  ~G1SATBCardTableModRefBS() { }

public:
  static int g1_young_card_val()   { return g1_young_gen; }

  // Add "pre_val" to a set of objects that may have been disconnected from the
  // pre-marking object graph.
  static void enqueue(oop pre_val);

  static void enqueue_if_weak(DecoratorSet decorators, oop value);

  template <class T> void write_ref_array_pre_work(T* dst, int count);
  virtual void write_ref_array_pre(oop* dst, int count, bool dest_uninitialized);
  virtual void write_ref_array_pre(narrowOop* dst, int count, bool dest_uninitialized);

  template <DecoratorSet decorators, typename T>
  void write_ref_field_pre(T* field);

/*
   Claimed and deferred bits are used together in G1 during the evacuation
   pause. These bits can have the following state transitions:
   1. The claimed bit can be put over any other card state. Except that
      the "dirty -> dirty and claimed" transition is checked for in
      G1 code and is not used.
   2. Deferred bit can be set only if the previous state of the card
      was either clean or claimed. mark_card_deferred() is wait-free.
      We do not care if the operation is be successful because if
      it does not it will only result in duplicate entry in the update
      buffer because of the "cache-miss". So it's not worth spinning.
 */

  bool is_card_claimed(size_t card_index) {
    jbyte val = _byte_map[card_index];
    return (val & (clean_card_mask_val() | claimed_card_val())) == claimed_card_val();
  }

  inline void set_card_claimed(size_t card_index);

  void verify_g1_young_region(MemRegion mr) PRODUCT_RETURN;
  void g1_mark_as_young(const MemRegion& mr);

  bool mark_card_deferred(size_t card_index);

  bool is_card_deferred(size_t card_index) {
    jbyte val = _byte_map[card_index];
    return (val & (clean_card_mask_val() | deferred_card_val())) == deferred_card_val();
  }
};

template<>
struct BarrierSet::GetName<G1SATBCardTableModRefBS> {
  static const BarrierSet::Name value = BarrierSet::G1SATBCT;
};

template<>
struct BarrierSet::GetType<BarrierSet::G1SATBCT> {
  typedef G1SATBCardTableModRefBS type;
};

class G1SATBCardTableLoggingModRefBSChangedListener : public G1MappingChangedListener {
 private:
  G1SATBCardTableLoggingModRefBS* _card_table;
 public:
  G1SATBCardTableLoggingModRefBSChangedListener() : _card_table(NULL) { }

  void set_card_table(G1SATBCardTableLoggingModRefBS* card_table) { _card_table = card_table; }

  virtual void on_commit(uint start_idx, size_t num_regions, bool zero_filled);
};

// Adds card-table logging to the post-barrier.
// Usual invariant: all dirty cards are logged in the DirtyCardQueueSet.
class G1SATBCardTableLoggingModRefBS: public G1SATBCardTableModRefBS {
  friend class G1SATBCardTableLoggingModRefBSChangedListener;
 private:
  G1SATBCardTableLoggingModRefBSChangedListener _listener;
  DirtyCardQueueSet& _dcqs;

 public:
  static size_t compute_size(size_t mem_region_size_in_words) {
    size_t number_of_slots = (mem_region_size_in_words / card_size_in_words);
    return ReservedSpace::allocation_align_size_up(number_of_slots);
  }

  // Returns how many bytes of the heap a single byte of the Card Table corresponds to.
  static size_t heap_map_factor() {
    return CardTableModRefBS::card_size;
  }

  G1SATBCardTableLoggingModRefBS(MemRegion whole_heap);

  virtual void initialize() { }
  virtual void initialize(G1RegionToSpaceMapper* mapper);

  virtual void resize_covered_region(MemRegion new_region) { ShouldNotReachHere(); }

  // NB: if you do a whole-heap invalidation, the "usual invariant" defined
  // above no longer applies.
  void invalidate(MemRegion mr);

  void write_region_work(MemRegion mr)    { invalidate(mr); }
  void write_ref_array_work(MemRegion mr) { invalidate(mr); }

  template <DecoratorSet decorators, typename T>
  void write_ref_field_post(T* field, oop new_val);
  void write_ref_field_post_slow(volatile jbyte* byte);

  // Callbacks for runtime accesses.
  template <DecoratorSet decorators, typename BarrierSetT = G1SATBCardTableLoggingModRefBS>
  class AccessBarrier: public ModRefBarrierSet::AccessBarrier<decorators, BarrierSetT> {
    typedef ModRefBarrierSet::AccessBarrier<decorators, BarrierSetT> ModRef;
    typedef BarrierSet::AccessBarrier<decorators, BarrierSetT> Raw;

  public:
    // Needed for loads on non-heap weak references
    template <typename T>
    static oop oop_load_not_in_heap(T* addr);

    // Needed for non-heap stores
    template <typename T>
    static void oop_store_not_in_heap(T* addr, oop new_value);

    // Needed for weak references
    static oop oop_load_in_heap_at(oop base, ptrdiff_t offset);

    // Defensive: will catch weak oops at addresses in heap
    template <typename T>
    static oop oop_load_in_heap(T* addr);
  };
};

template<>
struct BarrierSet::GetName<G1SATBCardTableLoggingModRefBS> {
  static const BarrierSet::Name value = BarrierSet::G1SATBCTLogging;
};

template<>
struct BarrierSet::GetType<BarrierSet::G1SATBCTLogging> {
  typedef G1SATBCardTableLoggingModRefBS type;
};

#endif // SHARE_VM_GC_G1_G1SATBCARDTABLEMODREFBS_HPP
