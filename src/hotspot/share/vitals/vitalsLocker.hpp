/*
 * Copyright (c) 2021, 2022 SAP SE. All rights reserved.
  *
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

#ifndef HOTSPOT_SHARE_VITALS_VITALSLOCKER_HPP
#define HOTSPOT_SHARE_VITALS_VITALSLOCKER_HPP

// SapMachine  2021-10-14: I need a simple critical section. I don't
// need hotspot mutex error checking here, and I want to be independent of
// upstream changes to hotspot mutexes.

#ifdef _WIN32
  #include <windows.h>
#else
  #include <pthread.h>
#endif

namespace sapmachine_vitals {

class Lock {
  const char* const _name;
#ifdef _WIN32
  CRITICAL_SECTION _lock;
#else
  pthread_mutex_t _lock;
#endif

public:
  Lock(const char* name);
  void lock();
  void unlock();
};

class AutoLock {
  Lock* const _lock;
public:
  AutoLock(Lock* lock)
    : _lock(lock)
  {
    _lock->lock();
  }
  ~AutoLock() {
    _lock->unlock();
  }
};

};

#endif /* HOTSPOT_SHARE_VITALS_VITALS_HPP */
