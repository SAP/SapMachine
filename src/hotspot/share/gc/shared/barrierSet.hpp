/*
 * Copyright (c) 2000, 2018, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_SHARED_BARRIERSET_HPP
#define SHARE_VM_GC_SHARED_BARRIERSET_HPP

#include "gc/shared/barrierSetConfig.hpp"
#include "memory/memRegion.hpp"
#include "oops/access.hpp"
#include "oops/accessBackend.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/fakeRttiSupport.hpp"
#include "utilities/macros.hpp"

class BarrierSetAssembler;
class BarrierSetC1;
class BarrierSetC2;
class JavaThread;

// This class provides the interface between a barrier implementation and
// the rest of the system.

class BarrierSet: public CHeapObj<mtGC> {
  friend class VMStructs;

  static BarrierSet* _barrier_set;

public:
  enum Name {
#define BARRIER_SET_DECLARE_BS_ENUM(bs_name) bs_name ,
    FOR_EACH_BARRIER_SET_DO(BARRIER_SET_DECLARE_BS_ENUM)
#undef BARRIER_SET_DECLARE_BS_ENUM
    UnknownBS
  };

protected:
  // Fake RTTI support.  For a derived class T to participate
  // - T must have a corresponding Name entry.
  // - GetName<T> must be specialized to return the corresponding Name
  //   entry.
  // - If T is a base class, the constructor must have a FakeRtti
  //   parameter and pass it up to its base class, with the tag set
  //   augmented with the corresponding Name entry.
  // - If T is a concrete class, the constructor must create a
  //   FakeRtti object whose tag set includes the corresponding Name
  //   entry, and pass it up to its base class.
  typedef FakeRttiSupport<BarrierSet, Name> FakeRtti;

private:
  FakeRtti _fake_rtti;
  BarrierSetAssembler* _barrier_set_assembler;
  BarrierSetC1* _barrier_set_c1;
  BarrierSetC2* _barrier_set_c2;

public:
  // Metafunction mapping a class derived from BarrierSet to the
  // corresponding Name enum tag.
  template<typename T> struct GetName;

  // Metafunction mapping a Name enum type to the corresponding
  // lass derived from BarrierSet.
  template<BarrierSet::Name T> struct GetType;

  // Note: This is not presently the Name corresponding to the
  // concrete class of this object.
  BarrierSet::Name kind() const { return _fake_rtti.concrete_tag(); }

  // Test whether this object is of the type corresponding to bsn.
  bool is_a(BarrierSet::Name bsn) const { return _fake_rtti.has_tag(bsn); }

  // End of fake RTTI support.

protected:
  BarrierSet(BarrierSetAssembler* barrier_set_assembler,
             BarrierSetC1* barrier_set_c1,
             BarrierSetC2* barrier_set_c2,
             const FakeRtti& fake_rtti) :
    _fake_rtti(fake_rtti),
    _barrier_set_assembler(barrier_set_assembler),
    _barrier_set_c1(barrier_set_c1),
    _barrier_set_c2(barrier_set_c2) {}
  ~BarrierSet() { }

  template <class BarrierSetAssemblerT>
  static BarrierSetAssembler* make_barrier_set_assembler() {
    return NOT_ZERO(new BarrierSetAssemblerT()) ZERO_ONLY(NULL);
  }

  template <class BarrierSetC1T>
  static BarrierSetC1* make_barrier_set_c1() {
    return COMPILER1_PRESENT(new BarrierSetC1T()) NOT_COMPILER1(NULL);
  }

  template <class BarrierSetC2T>
  static BarrierSetC2* make_barrier_set_c2() {
    return COMPILER2_PRESENT(new BarrierSetC2T()) NOT_COMPILER2(NULL);
  }

public:
  // Support for optimizing compilers to call the barrier set on slow path allocations
  // that did not enter a TLAB. Used for e.g. ReduceInitialCardMarks.
  // The allocation is safe to use iff it returns true. If not, the slow-path allocation
  // is redone until it succeeds. This can e.g. prevent allocations from the slow path
  // to be in old.
  virtual void on_slowpath_allocation_exit(JavaThread* thread, oop new_obj) {}
  virtual void on_thread_create(Thread* thread) {}
  virtual void on_thread_destroy(Thread* thread) {}
  virtual void on_thread_attach(JavaThread* thread) {}
  virtual void on_thread_detach(JavaThread* thread) {}
  virtual void make_parsable(JavaThread* thread) {}

public:
  // Print a description of the memory for the barrier set
  virtual void print_on(outputStream* st) const = 0;

  static BarrierSet* barrier_set() { return _barrier_set; }
  static void set_barrier_set(BarrierSet* barrier_set);

  BarrierSetAssembler* barrier_set_assembler() {
    assert(_barrier_set_assembler != NULL, "should be set");
    return _barrier_set_assembler;
  }

  BarrierSetC1* barrier_set_c1() {
    assert(_barrier_set_c1 != NULL, "should be set");
    return _barrier_set_c1;
  }

  BarrierSetC2* barrier_set_c2() {
    assert(_barrier_set_c2 != NULL, "should be set");
    return _barrier_set_c2;
  }

  // The AccessBarrier of a BarrierSet subclass is called by the Access API
  // (cf. oops/access.hpp) to perform decorated accesses. GC implementations
  // may override these default access operations by declaring an
  // AccessBarrier class in its BarrierSet. Its accessors will then be
  // automatically resolved at runtime.
  //
  // In order to register a new FooBarrierSet::AccessBarrier with the Access API,
  // the following steps should be taken:
  // 1) Provide an enum "name" for the BarrierSet in barrierSetConfig.hpp
  // 2) Make sure the barrier set headers are included from barrierSetConfig.inline.hpp
  // 3) Provide specializations for BarrierSet::GetName and BarrierSet::GetType.
  template <DecoratorSet decorators, typename BarrierSetT>
  class AccessBarrier: protected RawAccessBarrier<decorators> {
  private:
    typedef RawAccessBarrier<decorators> Raw;

  public:
    // Primitive heap accesses. These accessors get resolved when
    // IN_HEAP is set (e.g. when using the HeapAccess API), it is
    // not an oop_* overload, and the barrier strength is AS_NORMAL.
    template <typename T>
    static T load_in_heap(T* addr) {
      return Raw::template load<T>(addr);
    }

    template <typename T>
    static T load_in_heap_at(oop base, ptrdiff_t offset) {
      return Raw::template load_at<T>(base, offset);
    }

    template <typename T>
    static void store_in_heap(T* addr, T value) {
      Raw::store(addr, value);
    }

    template <typename T>
    static void store_in_heap_at(oop base, ptrdiff_t offset, T value) {
      Raw::store_at(base, offset, value);
    }

    template <typename T>
    static T atomic_cmpxchg_in_heap(T new_value, T* addr, T compare_value) {
      return Raw::atomic_cmpxchg(new_value, addr, compare_value);
    }

    template <typename T>
    static T atomic_cmpxchg_in_heap_at(T new_value, oop base, ptrdiff_t offset, T compare_value) {
      return Raw::oop_atomic_cmpxchg_at(new_value, base, offset, compare_value);
    }

    template <typename T>
    static T atomic_xchg_in_heap(T new_value, T* addr) {
      return Raw::atomic_xchg(new_value, addr);
    }

    template <typename T>
    static T atomic_xchg_in_heap_at(T new_value, oop base, ptrdiff_t offset) {
      return Raw::atomic_xchg_at(new_value, base, offset);
    }

    template <typename T>
    static void arraycopy_in_heap(arrayOop src_obj, size_t src_offset_in_bytes, T* src_raw,
                                  arrayOop dst_obj, size_t dst_offset_in_bytes, T* dst_raw,
                                  size_t length) {
      Raw::arraycopy(src_obj, src_offset_in_bytes, src_raw,
                     dst_obj, dst_offset_in_bytes, dst_raw,
                     length);
    }

    // Heap oop accesses. These accessors get resolved when
    // IN_HEAP is set (e.g. when using the HeapAccess API), it is
    // an oop_* overload, and the barrier strength is AS_NORMAL.
    template <typename T>
    static oop oop_load_in_heap(T* addr) {
      return Raw::template oop_load<oop>(addr);
    }

    static oop oop_load_in_heap_at(oop base, ptrdiff_t offset) {
      return Raw::template oop_load_at<oop>(base, offset);
    }

    template <typename T>
    static void oop_store_in_heap(T* addr, oop value) {
      Raw::oop_store(addr, value);
    }

    static void oop_store_in_heap_at(oop base, ptrdiff_t offset, oop value) {
      Raw::oop_store_at(base, offset, value);
    }

    template <typename T>
    static oop oop_atomic_cmpxchg_in_heap(oop new_value, T* addr, oop compare_value) {
      return Raw::oop_atomic_cmpxchg(new_value, addr, compare_value);
    }

    static oop oop_atomic_cmpxchg_in_heap_at(oop new_value, oop base, ptrdiff_t offset, oop compare_value) {
      return Raw::oop_atomic_cmpxchg_at(new_value, base, offset, compare_value);
    }

    template <typename T>
    static oop oop_atomic_xchg_in_heap(oop new_value, T* addr) {
      return Raw::oop_atomic_xchg(new_value, addr);
    }

    static oop oop_atomic_xchg_in_heap_at(oop new_value, oop base, ptrdiff_t offset) {
      return Raw::oop_atomic_xchg_at(new_value, base, offset);
    }

    template <typename T>
    static bool oop_arraycopy_in_heap(arrayOop src_obj, size_t src_offset_in_bytes, T* src_raw,
                                      arrayOop dst_obj, size_t dst_offset_in_bytes, T* dst_raw,
                                      size_t length) {
      return Raw::oop_arraycopy(src_obj, src_offset_in_bytes, src_raw,
                                dst_obj, dst_offset_in_bytes, dst_raw,
                                length);
    }

    // Off-heap oop accesses. These accessors get resolved when
    // IN_HEAP is not set (e.g. when using the NativeAccess API), it is
    // an oop* overload, and the barrier strength is AS_NORMAL.
    template <typename T>
    static oop oop_load_not_in_heap(T* addr) {
      return Raw::template oop_load<oop>(addr);
    }

    template <typename T>
    static void oop_store_not_in_heap(T* addr, oop value) {
      Raw::oop_store(addr, value);
    }

    template <typename T>
    static oop oop_atomic_cmpxchg_not_in_heap(oop new_value, T* addr, oop compare_value) {
      return Raw::oop_atomic_cmpxchg(new_value, addr, compare_value);
    }

    template <typename T>
    static oop oop_atomic_xchg_not_in_heap(oop new_value, T* addr) {
      return Raw::oop_atomic_xchg(new_value, addr);
    }

    // Clone barrier support
    static void clone_in_heap(oop src, oop dst, size_t size) {
      Raw::clone(src, dst, size);
    }

    static oop resolve(oop obj) {
      return Raw::resolve(obj);
    }

    static bool equals(oop o1, oop o2) {
      return Raw::equals(o1, o2);
    }
  };
};

template<typename T>
inline T* barrier_set_cast(BarrierSet* bs) {
  assert(bs->is_a(BarrierSet::GetName<T>::value), "wrong type of barrier set");
  return static_cast<T*>(bs);
}

#endif // SHARE_VM_GC_SHARED_BARRIERSET_HPP
