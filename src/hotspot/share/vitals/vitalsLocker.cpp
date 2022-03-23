/*
 * Copyright (c) 2019, 2022 SAP SE. All rights reserved.
 * Copyright (c) 2019, 2022, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "vitals/vitalsLocker.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/debug.hpp"

#ifndef _WIN32
#include <errno.h>
#endif

namespace sapmachine_vitals {

#ifdef _WIN32

Lock::Lock(const char* name) : _name(name) {
  ::InitializeCriticalSection(&_lock);
}

void Lock::lock() {
  ::EnterCriticalSection(&_lock);
}

void Lock::unlock() {
  ::LeaveCriticalSection(&_lock);
}

#else

Lock::Lock(const char* name) : _name(name), _lock(PTHREAD_MUTEX_INITIALIZER) {}

void Lock::lock() {
  int rc = ::pthread_mutex_lock(&_lock);
  assert(rc == 0, "%s: failed to grab lock (%d).", _name, errno);
}

void Lock::unlock() {
  int rc = ::pthread_mutex_unlock(&_lock);
  assert(rc == 0, "%s: failed to release lock (%d).", _name, errno);
}

#endif


}; // namespace sapmachine_vitals
