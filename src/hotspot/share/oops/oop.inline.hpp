/*
 * Copyright (c) 1997, 2017, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_OOPS_OOP_INLINE_HPP
#define SHARE_VM_OOPS_OOP_INLINE_HPP

#include "gc/shared/ageTable.hpp"
#include "gc/shared/collectedHeap.inline.hpp"
#include "gc/shared/genCollectedHeap.hpp"
#include "gc/shared/generation.hpp"
#include "oops/access.inline.hpp"
#include "oops/arrayKlass.hpp"
#include "oops/arrayOop.hpp"
#include "oops/klass.inline.hpp"
#include "oops/markOop.inline.hpp"
#include "oops/oop.hpp"
#include "runtime/atomic.hpp"
#include "runtime/orderAccess.inline.hpp"
#include "runtime/os.hpp"
#include "utilities/align.hpp"
#include "utilities/macros.hpp"

// Implementation of all inlined member functions defined in oop.hpp
// We need a separate file to avoid circular references

void oopDesc::release_set_mark(markOop m) {
  OrderAccess::release_store(&_mark, m);
}

markOop oopDesc::cas_set_mark(markOop new_mark, markOop old_mark) {
  return Atomic::cmpxchg(new_mark, &_mark, old_mark);
}

void oopDesc::init_mark() {
  set_mark(markOopDesc::prototype_for_object(this));
}

Klass* oopDesc::klass() const {
  if (UseCompressedClassPointers) {
    return Klass::decode_klass_not_null(_metadata._compressed_klass);
  } else {
    return _metadata._klass;
  }
}

Klass* oopDesc::klass_or_null() const volatile {
  if (UseCompressedClassPointers) {
    return Klass::decode_klass(_metadata._compressed_klass);
  } else {
    return _metadata._klass;
  }
}

Klass* oopDesc::klass_or_null_acquire() const volatile {
  if (UseCompressedClassPointers) {
    // Workaround for non-const load_acquire parameter.
    const volatile narrowKlass* addr = &_metadata._compressed_klass;
    volatile narrowKlass* xaddr = const_cast<volatile narrowKlass*>(addr);
    return Klass::decode_klass(OrderAccess::load_acquire(xaddr));
  } else {
    return OrderAccess::load_acquire(&_metadata._klass);
  }
}

Klass** oopDesc::klass_addr() {
  // Only used internally and with CMS and will not work with
  // UseCompressedOops
  assert(!UseCompressedClassPointers, "only supported with uncompressed klass pointers");
  return (Klass**) &_metadata._klass;
}

narrowKlass* oopDesc::compressed_klass_addr() {
  assert(UseCompressedClassPointers, "only called by compressed klass pointers");
  return &_metadata._compressed_klass;
}

#define CHECK_SET_KLASS(k)                                                \
  do {                                                                    \
    assert(Universe::is_bootstrapping() || k != NULL, "NULL Klass");      \
    assert(Universe::is_bootstrapping() || k->is_klass(), "not a Klass"); \
  } while (0)

void oopDesc::set_klass(Klass* k) {
  CHECK_SET_KLASS(k);
  if (UseCompressedClassPointers) {
    *compressed_klass_addr() = Klass::encode_klass_not_null(k);
  } else {
    *klass_addr() = k;
  }
}

void oopDesc::release_set_klass(Klass* k) {
  CHECK_SET_KLASS(k);
  if (UseCompressedClassPointers) {
    OrderAccess::release_store(compressed_klass_addr(),
                               Klass::encode_klass_not_null(k));
  } else {
    OrderAccess::release_store(klass_addr(), k);
  }
}

#undef CHECK_SET_KLASS

int oopDesc::klass_gap() const {
  return *(int*)(((intptr_t)this) + klass_gap_offset_in_bytes());
}

void oopDesc::set_klass_gap(int v) {
  if (UseCompressedClassPointers) {
    *(int*)(((intptr_t)this) + klass_gap_offset_in_bytes()) = v;
  }
}

void oopDesc::set_klass_to_list_ptr(oop k) {
  // This is only to be used during GC, for from-space objects, so no
  // barrier is needed.
  if (UseCompressedClassPointers) {
    _metadata._compressed_klass = (narrowKlass)encode_heap_oop(k);  // may be null (parnew overflow handling)
  } else {
    _metadata._klass = (Klass*)(address)k;
  }
}

oop oopDesc::list_ptr_from_klass() {
  // This is only to be used during GC, for from-space objects.
  if (UseCompressedClassPointers) {
    return decode_heap_oop((narrowOop)_metadata._compressed_klass);
  } else {
    // Special case for GC
    return (oop)(address)_metadata._klass;
  }
}

bool oopDesc::is_a(Klass* k) const {
  return klass()->is_subtype_of(k);
}

int oopDesc::size()  {
  return size_given_klass(klass());
}

int oopDesc::size_given_klass(Klass* klass)  {
  int lh = klass->layout_helper();
  int s;

  // lh is now a value computed at class initialization that may hint
  // at the size.  For instances, this is positive and equal to the
  // size.  For arrays, this is negative and provides log2 of the
  // array element size.  For other oops, it is zero and thus requires
  // a virtual call.
  //
  // We go to all this trouble because the size computation is at the
  // heart of phase 2 of mark-compaction, and called for every object,
  // alive or dead.  So the speed here is equal in importance to the
  // speed of allocation.

  if (lh > Klass::_lh_neutral_value) {
    if (!Klass::layout_helper_needs_slow_path(lh)) {
      s = lh >> LogHeapWordSize;  // deliver size scaled by wordSize
    } else {
      s = klass->oop_size(this);
    }
  } else if (lh <= Klass::_lh_neutral_value) {
    // The most common case is instances; fall through if so.
    if (lh < Klass::_lh_neutral_value) {
      // Second most common case is arrays.  We have to fetch the
      // length of the array, shift (multiply) it appropriately,
      // up to wordSize, add the header, and align to object size.
      size_t size_in_bytes;
      size_t array_length = (size_t) ((arrayOop)this)->length();
      size_in_bytes = array_length << Klass::layout_helper_log2_element_size(lh);
      size_in_bytes += Klass::layout_helper_header_size(lh);

      // This code could be simplified, but by keeping array_header_in_bytes
      // in units of bytes and doing it this way we can round up just once,
      // skipping the intermediate round to HeapWordSize.
      s = (int)(align_up(size_in_bytes, MinObjAlignmentInBytes) / HeapWordSize);

      // ParNew (used by CMS), UseParallelGC and UseG1GC can change the length field
      // of an "old copy" of an object array in the young gen so it indicates
      // the grey portion of an already copied array. This will cause the first
      // disjunct below to fail if the two comparands are computed across such
      // a concurrent change.
      // ParNew also runs with promotion labs (which look like int
      // filler arrays) which are subject to changing their declared size
      // when finally retiring a PLAB; this also can cause the first disjunct
      // to fail for another worker thread that is concurrently walking the block
      // offset table. Both these invariant failures are benign for their
      // current uses; we relax the assertion checking to cover these two cases below:
      //     is_objArray() && is_forwarded()   // covers first scenario above
      //  || is_typeArray()                    // covers second scenario above
      // If and when UseParallelGC uses the same obj array oop stealing/chunking
      // technique, we will need to suitably modify the assertion.
      assert((s == klass->oop_size(this)) ||
             (Universe::heap()->is_gc_active() &&
              ((is_typeArray() && UseConcMarkSweepGC) ||
               (is_objArray()  && is_forwarded() && (UseConcMarkSweepGC || UseParallelGC || UseG1GC)))),
             "wrong array object size");
    } else {
      // Must be zero, so bite the bullet and take the virtual call.
      s = klass->oop_size(this);
    }
  }

  assert(s > 0, "Oop size must be greater than zero, not %d", s);
  assert(is_object_aligned(s), "Oop size is not properly aligned: %d", s);
  return s;
}

bool oopDesc::is_instance()  const { return klass()->is_instance_klass();  }
bool oopDesc::is_array()     const { return klass()->is_array_klass();     }
bool oopDesc::is_objArray()  const { return klass()->is_objArray_klass();  }
bool oopDesc::is_typeArray() const { return klass()->is_typeArray_klass(); }

void*      oopDesc::field_base(int offset)          const { return (void*)&((char*)this)[offset]; }

jbyte*     oopDesc::byte_field_addr(int offset)     const { return (jbyte*)    field_base(offset); }
jchar*     oopDesc::char_field_addr(int offset)     const { return (jchar*)    field_base(offset); }
jboolean*  oopDesc::bool_field_addr(int offset)     const { return (jboolean*) field_base(offset); }
jint*      oopDesc::int_field_addr(int offset)      const { return (jint*)     field_base(offset); }
jshort*    oopDesc::short_field_addr(int offset)    const { return (jshort*)   field_base(offset); }
jlong*     oopDesc::long_field_addr(int offset)     const { return (jlong*)    field_base(offset); }
jfloat*    oopDesc::float_field_addr(int offset)    const { return (jfloat*)   field_base(offset); }
jdouble*   oopDesc::double_field_addr(int offset)   const { return (jdouble*)  field_base(offset); }
Metadata** oopDesc::metadata_field_addr(int offset) const { return (Metadata**)field_base(offset); }

template <class T> T* oopDesc::obj_field_addr(int offset) const { return (T*)  field_base(offset); }
address*   oopDesc::address_field_addr(int offset)  const { return (address*)  field_base(offset); }


// Functions for getting and setting oops within instance objects.
// If the oops are compressed, the type passed to these overloaded functions
// is narrowOop.  All functions are overloaded so they can be called by
// template functions without conditionals (the compiler instantiates via
// the right type and inlines the appopriate code).

// Algorithm for encoding and decoding oops from 64 bit pointers to 32 bit
// offset from the heap base.  Saving the check for null can save instructions
// in inner GC loops so these are separated.

inline bool check_obj_alignment(oop obj) {
  return (cast_from_oop<intptr_t>(obj) & MinObjAlignmentInBytesMask) == 0;
}

oop oopDesc::decode_heap_oop_not_null(narrowOop v) {
  assert(!is_null(v), "narrow oop value can never be zero");
  address base = Universe::narrow_oop_base();
  int    shift = Universe::narrow_oop_shift();
  oop result = (oop)(void*)((uintptr_t)base + ((uintptr_t)v << shift));
  assert(check_obj_alignment(result), "address not aligned: " INTPTR_FORMAT, p2i((void*) result));
  return result;
}

oop oopDesc::decode_heap_oop(narrowOop v) {
  return is_null(v) ? (oop)NULL : decode_heap_oop_not_null(v);
}

narrowOop oopDesc::encode_heap_oop_not_null(oop v) {
  assert(!is_null(v), "oop value can never be zero");
  assert(check_obj_alignment(v), "Address not aligned");
  assert(Universe::heap()->is_in_reserved(v), "Address not in heap");
  address base = Universe::narrow_oop_base();
  int    shift = Universe::narrow_oop_shift();
  uint64_t  pd = (uint64_t)(pointer_delta((void*)v, (void*)base, 1));
  assert(OopEncodingHeapMax > pd, "change encoding max if new encoding");
  uint64_t result = pd >> shift;
  assert((result & CONST64(0xffffffff00000000)) == 0, "narrow oop overflow");
  assert(decode_heap_oop(result) == v, "reversibility");
  return (narrowOop)result;
}

narrowOop oopDesc::encode_heap_oop(oop v) {
  return (is_null(v)) ? (narrowOop)0 : encode_heap_oop_not_null(v);
}

narrowOop oopDesc::load_heap_oop(narrowOop* p) { return *p; }
oop       oopDesc::load_heap_oop(oop* p)       { return *p; }

void oopDesc::store_heap_oop(narrowOop* p, narrowOop v) { *p = v; }
void oopDesc::store_heap_oop(oop* p, oop v)             { *p = v; }

// Load and decode an oop out of the Java heap into a wide oop.
oop oopDesc::load_decode_heap_oop_not_null(narrowOop* p) {
  return decode_heap_oop_not_null(load_heap_oop(p));
}

// Load and decode an oop out of the heap accepting null
oop oopDesc::load_decode_heap_oop(narrowOop* p) {
  return decode_heap_oop(load_heap_oop(p));
}

oop oopDesc::load_decode_heap_oop_not_null(oop* p) { return *p; }
oop oopDesc::load_decode_heap_oop(oop* p)          { return *p; }

void oopDesc::encode_store_heap_oop_not_null(oop* p, oop v) { *p = v; }
void oopDesc::encode_store_heap_oop(oop* p, oop v)          { *p = v; }

// Encode and store a heap oop.
void oopDesc::encode_store_heap_oop_not_null(narrowOop* p, oop v) {
  *p = encode_heap_oop_not_null(v);
}

// Encode and store a heap oop allowing for null.
void oopDesc::encode_store_heap_oop(narrowOop* p, oop v) {
  *p = encode_heap_oop(v);
}

inline oop  oopDesc::obj_field(int offset) const                    { return HeapAccess<>::oop_load_at(as_oop(), offset);  }
inline void oopDesc::obj_field_put(int offset, oop value)           { HeapAccess<>::oop_store_at(as_oop(), offset, value); }

inline jbyte oopDesc::byte_field(int offset) const                  { return HeapAccess<>::load_at(as_oop(), offset);  }
inline void  oopDesc::byte_field_put(int offset, jbyte value)       { HeapAccess<>::store_at(as_oop(), offset, value); }

inline jchar oopDesc::char_field(int offset) const                  { return HeapAccess<>::load_at(as_oop(), offset);  }
inline void  oopDesc::char_field_put(int offset, jchar value)       { HeapAccess<>::store_at(as_oop(), offset, value); }

inline jboolean oopDesc::bool_field(int offset) const               { return HeapAccess<>::load_at(as_oop(), offset);                }
inline void     oopDesc::bool_field_put(int offset, jboolean value) { HeapAccess<>::store_at(as_oop(), offset, jboolean(value & 1)); }

inline jshort oopDesc::short_field(int offset) const                { return HeapAccess<>::load_at(as_oop(), offset);  }
inline void   oopDesc::short_field_put(int offset, jshort value)    { HeapAccess<>::store_at(as_oop(), offset, value); }

inline jint oopDesc::int_field(int offset) const                    { return HeapAccess<>::load_at(as_oop(), offset);  }
inline void oopDesc::int_field_put(int offset, jint value)          { HeapAccess<>::store_at(as_oop(), offset, value); }

inline jlong oopDesc::long_field(int offset) const                  { return HeapAccess<>::load_at(as_oop(), offset);  }
inline void  oopDesc::long_field_put(int offset, jlong value)       { HeapAccess<>::store_at(as_oop(), offset, value); }

inline jfloat oopDesc::float_field(int offset) const                { return HeapAccess<>::load_at(as_oop(), offset);  }
inline void   oopDesc::float_field_put(int offset, jfloat value)    { HeapAccess<>::store_at(as_oop(), offset, value); }

inline jdouble oopDesc::double_field(int offset) const              { return HeapAccess<>::load_at(as_oop(), offset);  }
inline void    oopDesc::double_field_put(int offset, jdouble value) { HeapAccess<>::store_at(as_oop(), offset, value); }

bool oopDesc::is_locked() const {
  return mark()->is_locked();
}

bool oopDesc::is_unlocked() const {
  return mark()->is_unlocked();
}

bool oopDesc::has_bias_pattern() const {
  return mark()->has_bias_pattern();
}

// Used only for markSweep, scavenging
bool oopDesc::is_gc_marked() const {
  return mark()->is_marked();
}

bool oopDesc::is_scavengable() const {
  return Universe::heap()->is_scavengable(oop(const_cast<oopDesc*>(this)));
}

// Used by scavengers
bool oopDesc::is_forwarded() const {
  // The extra heap check is needed since the obj might be locked, in which case the
  // mark would point to a stack location and have the sentinel bit cleared
  return mark()->is_marked();
}

// Used by scavengers
void oopDesc::forward_to(oop p) {
  assert(check_obj_alignment(p),
         "forwarding to something not aligned");
  assert(Universe::heap()->is_in_reserved(p),
         "forwarding to something not in heap");
  assert(!is_archive_object(oop(this)) &&
         !is_archive_object(p),
         "forwarding archive object");
  markOop m = markOopDesc::encode_pointer_as_mark(p);
  assert(m->decode_pointer() == p, "encoding must be reversable");
  set_mark(m);
}

// Used by parallel scavengers
bool oopDesc::cas_forward_to(oop p, markOop compare) {
  assert(check_obj_alignment(p),
         "forwarding to something not aligned");
  assert(Universe::heap()->is_in_reserved(p),
         "forwarding to something not in heap");
  markOop m = markOopDesc::encode_pointer_as_mark(p);
  assert(m->decode_pointer() == p, "encoding must be reversable");
  return cas_set_mark(m, compare) == compare;
}

#if INCLUDE_ALL_GCS
oop oopDesc::forward_to_atomic(oop p) {
  markOop oldMark = mark();
  markOop forwardPtrMark = markOopDesc::encode_pointer_as_mark(p);
  markOop curMark;

  assert(forwardPtrMark->decode_pointer() == p, "encoding must be reversable");
  assert(sizeof(markOop) == sizeof(intptr_t), "CAS below requires this.");

  while (!oldMark->is_marked()) {
    curMark = Atomic::cmpxchg(forwardPtrMark, &_mark, oldMark);
    assert(is_forwarded(), "object should have been forwarded");
    if (curMark == oldMark) {
      return NULL;
    }
    // If the CAS was unsuccessful then curMark->is_marked()
    // should return true as another thread has CAS'd in another
    // forwarding pointer.
    oldMark = curMark;
  }
  return forwardee();
}
#endif

// Note that the forwardee is not the same thing as the displaced_mark.
// The forwardee is used when copying during scavenge and mark-sweep.
// It does need to clear the low two locking- and GC-related bits.
oop oopDesc::forwardee() const {
  return (oop) mark()->decode_pointer();
}

// The following method needs to be MT safe.
uint oopDesc::age() const {
  assert(!is_forwarded(), "Attempt to read age from forwarded mark");
  if (has_displaced_mark()) {
    return displaced_mark()->age();
  } else {
    return mark()->age();
  }
}

void oopDesc::incr_age() {
  assert(!is_forwarded(), "Attempt to increment age of forwarded mark");
  if (has_displaced_mark()) {
    set_displaced_mark(displaced_mark()->incr_age());
  } else {
    set_mark(mark()->incr_age());
  }
}

#if INCLUDE_ALL_GCS
void oopDesc::pc_follow_contents(ParCompactionManager* cm) {
  klass()->oop_pc_follow_contents(this, cm);
}

void oopDesc::pc_update_contents(ParCompactionManager* cm) {
  Klass* k = klass();
  if (!k->is_typeArray_klass()) {
    // It might contain oops beyond the header, so take the virtual call.
    k->oop_pc_update_pointers(this, cm);
  }
  // Else skip it.  The TypeArrayKlass in the header never needs scavenging.
}

void oopDesc::ps_push_contents(PSPromotionManager* pm) {
  Klass* k = klass();
  if (!k->is_typeArray_klass()) {
    // It might contain oops beyond the header, so take the virtual call.
    k->oop_ps_push_contents(this, pm);
  }
  // Else skip it.  The TypeArrayKlass in the header never needs scavenging.
}
#endif // INCLUDE_ALL_GCS

#define OOP_ITERATE_DEFN(OopClosureType, nv_suffix)                 \
                                                                    \
void oopDesc::oop_iterate(OopClosureType* blk) {                    \
  klass()->oop_oop_iterate##nv_suffix(this, blk);                   \
}                                                                   \
                                                                    \
void oopDesc::oop_iterate(OopClosureType* blk, MemRegion mr) {      \
  klass()->oop_oop_iterate_bounded##nv_suffix(this, blk, mr);       \
}

#define OOP_ITERATE_SIZE_DEFN(OopClosureType, nv_suffix)            \
                                                                    \
int oopDesc::oop_iterate_size(OopClosureType* blk) {                \
  Klass* k = klass();                                               \
  int size = size_given_klass(k);                                   \
  k->oop_oop_iterate##nv_suffix(this, blk);                         \
  return size;                                                      \
}                                                                   \
                                                                    \
int oopDesc::oop_iterate_size(OopClosureType* blk, MemRegion mr) {  \
  Klass* k = klass();                                               \
  int size = size_given_klass(k);                                   \
  k->oop_oop_iterate_bounded##nv_suffix(this, blk, mr);             \
  return size;                                                      \
}

int oopDesc::oop_iterate_no_header(OopClosure* blk) {
  // The NoHeaderExtendedOopClosure wraps the OopClosure and proxies all
  // the do_oop calls, but turns off all other features in ExtendedOopClosure.
  NoHeaderExtendedOopClosure cl(blk);
  return oop_iterate_size(&cl);
}

int oopDesc::oop_iterate_no_header(OopClosure* blk, MemRegion mr) {
  NoHeaderExtendedOopClosure cl(blk);
  return oop_iterate_size(&cl, mr);
}

#if INCLUDE_ALL_GCS
#define OOP_ITERATE_BACKWARDS_DEFN(OopClosureType, nv_suffix)       \
                                                                    \
inline void oopDesc::oop_iterate_backwards(OopClosureType* blk) {   \
  klass()->oop_oop_iterate_backwards##nv_suffix(this, blk);         \
}
#else
#define OOP_ITERATE_BACKWARDS_DEFN(OopClosureType, nv_suffix)
#endif // INCLUDE_ALL_GCS

#define ALL_OOPDESC_OOP_ITERATE(OopClosureType, nv_suffix)  \
  OOP_ITERATE_DEFN(OopClosureType, nv_suffix)               \
  OOP_ITERATE_SIZE_DEFN(OopClosureType, nv_suffix)          \
  OOP_ITERATE_BACKWARDS_DEFN(OopClosureType, nv_suffix)

ALL_OOP_OOP_ITERATE_CLOSURES_1(ALL_OOPDESC_OOP_ITERATE)
ALL_OOP_OOP_ITERATE_CLOSURES_2(ALL_OOPDESC_OOP_ITERATE)

intptr_t oopDesc::identity_hash() {
  // Fast case; if the object is unlocked and the hash value is set, no locking is needed
  // Note: The mark must be read into local variable to avoid concurrent updates.
  markOop mrk = mark();
  if (mrk->is_unlocked() && !mrk->has_no_hash()) {
    return mrk->hash();
  } else if (mrk->is_marked()) {
    return mrk->hash();
  } else {
    return slow_identity_hash();
  }
}

bool oopDesc::has_displaced_mark() const {
  return mark()->has_displaced_mark_helper();
}

markOop oopDesc::displaced_mark() const {
  return mark()->displaced_mark_helper();
}

void oopDesc::set_displaced_mark(markOop m) {
  mark()->set_displaced_mark_helper(m);
}

#endif // SHARE_VM_OOPS_OOP_INLINE_HPP
