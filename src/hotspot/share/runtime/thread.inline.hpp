/*
 * Copyright (c) 2012, 2017, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_RUNTIME_THREAD_INLINE_HPP
#define SHARE_VM_RUNTIME_THREAD_INLINE_HPP

#define SHARE_VM_RUNTIME_THREAD_INLINE_HPP_SCOPE

#include "runtime/atomic.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/thread.hpp"

#undef SHARE_VM_RUNTIME_THREAD_INLINE_HPP_SCOPE

inline void Thread::set_suspend_flag(SuspendFlags f) {
  assert(sizeof(jint) == sizeof(_suspend_flags), "size mismatch");
  uint32_t flags;
  do {
    flags = _suspend_flags;
  }
  while (Atomic::cmpxchg((jint)(flags | f),
                         (volatile jint*)&_suspend_flags,
                         (jint)flags) != (jint)flags);
}
inline void Thread::clear_suspend_flag(SuspendFlags f) {
  assert(sizeof(jint) == sizeof(_suspend_flags), "size mismatch");
  uint32_t flags;
  do {
    flags = _suspend_flags;
  }
  while (Atomic::cmpxchg((jint)(flags & ~f),
                         (volatile jint*)&_suspend_flags,
                         (jint)flags) != (jint)flags);
}

inline void Thread::set_has_async_exception() {
  set_suspend_flag(_has_async_exception);
}
inline void Thread::clear_has_async_exception() {
  clear_suspend_flag(_has_async_exception);
}
inline void Thread::set_critical_native_unlock() {
  set_suspend_flag(_critical_native_unlock);
}
inline void Thread::clear_critical_native_unlock() {
  clear_suspend_flag(_critical_native_unlock);
}
inline void Thread::set_trace_flag() {
  set_suspend_flag(_trace_flag);
}
inline void Thread::clear_trace_flag() {
  clear_suspend_flag(_trace_flag);
}

inline jlong Thread::cooked_allocated_bytes() {
  jlong allocated_bytes = OrderAccess::load_acquire(&_allocated_bytes);
  if (UseTLAB) {
    size_t used_bytes = tlab().used_bytes();
    if (used_bytes <= ThreadLocalAllocBuffer::max_size_in_bytes()) {
      // Comparing used_bytes with the maximum allowed size will ensure
      // that we don't add the used bytes from a semi-initialized TLAB
      // ending up with incorrect values. There is still a race between
      // incrementing _allocated_bytes and clearing the TLAB, that might
      // cause double counting in rare cases.
      return allocated_bytes + used_bytes;
    }
  }
  return allocated_bytes;
}

inline void JavaThread::set_ext_suspended() {
  set_suspend_flag (_ext_suspended);
}
inline void JavaThread::clear_ext_suspended() {
  clear_suspend_flag(_ext_suspended);
}

inline void JavaThread::set_external_suspend() {
  set_suspend_flag(_external_suspend);
}
inline void JavaThread::clear_external_suspend() {
  clear_suspend_flag(_external_suspend);
}

inline void JavaThread::set_deopt_suspend() {
  set_suspend_flag(_deopt_suspend);
}
inline void JavaThread::clear_deopt_suspend() {
  clear_suspend_flag(_deopt_suspend);
}

inline void JavaThread::set_pending_async_exception(oop e) {
  _pending_async_exception = e;
  _special_runtime_exit_condition = _async_exception;
  set_has_async_exception();
}

#if defined(PPC64) || defined (AARCH64)
inline JavaThreadState JavaThread::thread_state() const    {
  return (JavaThreadState) OrderAccess::load_acquire((volatile jint*)&_thread_state);
}

inline void JavaThread::set_thread_state(JavaThreadState s) {
  OrderAccess::release_store((volatile jint*)&_thread_state, (jint)s);
}
#endif

inline void JavaThread::set_done_attaching_via_jni() {
  _jni_attach_state = _attached_via_jni;
  OrderAccess::fence();
}

inline bool JavaThread::stack_guard_zone_unused() {
  return _stack_guard_state == stack_guard_unused;
}

inline bool JavaThread::stack_yellow_reserved_zone_disabled() {
  return _stack_guard_state == stack_guard_yellow_reserved_disabled;
}

inline bool JavaThread::stack_reserved_zone_disabled() {
  return _stack_guard_state == stack_guard_reserved_disabled;
}

inline size_t JavaThread::stack_available(address cur_sp) {
  // This code assumes java stacks grow down
  address low_addr; // Limit on the address for deepest stack depth
  if (_stack_guard_state == stack_guard_unused) {
    low_addr = stack_end();
  } else {
    low_addr = stack_reserved_zone_base();
  }
  return cur_sp > low_addr ? cur_sp - low_addr : 0;
}

inline bool JavaThread::stack_guards_enabled() {
#ifdef ASSERT
  if (os::uses_stack_guard_pages() &&
      !(DisablePrimordialThreadGuardPages && os::is_primordial_thread())) {
    assert(_stack_guard_state != stack_guard_unused, "guard pages must be in use");
  }
#endif
  return _stack_guard_state == stack_guard_enabled;
}

// The release make sure this store is done after storing the handshake
// operation or global state
inline void JavaThread::set_polling_page(void* poll_value) {
  OrderAccess::release_store(polling_page_addr(), poll_value);
}

// The aqcquire make sure reading of polling page is done before
// the reading the handshake operation or the global state
inline volatile void* JavaThread::get_polling_page() {
  return OrderAccess::load_acquire(polling_page_addr());
}

#endif // SHARE_VM_RUNTIME_THREAD_INLINE_HPP
