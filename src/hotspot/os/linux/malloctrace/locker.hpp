/*
 * Copyright (c) 2021 SAP SE. All rights reserved.
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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

#ifndef OS_LINUX_MALLOCTRACE_LOCKER_HPP
#define OS_LINUX_MALLOCTRACE_LOCKER_HPP

#include "malloctrace/assertHandling.hpp"
#include "malloctrace/mallocTrace.hpp"
#include "utilities/globalDefinitions.hpp"
#include <pthread.h>

#ifdef HAVE_GLIBC_MALLOC_HOOKS

class outputStream;

namespace sap {

/////// A simple native lock using pthread mutexes

class Locker {
  static pthread_mutex_t _pthread_mutex;
  bool _locked;

  bool lock() {
    malloctrace_assert(!_locked, "already locked");
    if (::pthread_mutex_lock(&_pthread_mutex) != 0) {
      malloctrace_assert(false, "MALLOCTRACE lock failed");
      return false;
    }
    return true;
  }

public:

  // Manually unlock is public since we need it in case of asserts
  // (see malloctrace_assert)
  static void unlock() {
    ::pthread_mutex_unlock(&_pthread_mutex);
  }

  Locker() : _locked(false) {
    _locked = lock();
  }

  ~Locker() {
    if (_locked) {
      unlock();
    }
  }

};

} // namespace sap

#endif // HAVE_GLIBC_MALLOC_HOOKS

#endif // OS_LINUX_MALLOCTRACE_LOCKER_HPP
