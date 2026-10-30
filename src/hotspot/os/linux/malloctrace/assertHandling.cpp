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

#include "precompiled.hpp"
#include "malloctrace/assertHandling.hpp"
#include "malloctrace/locker.hpp"
#include "malloctrace/mallocTrace.hpp"
#include "runtime/atomic.hpp"

#ifdef HAVE_GLIBC_MALLOC_HOOKS

namespace sap {

static volatile bool g_asserting = false;

bool prepare_assert() {

  // Ignore all but the first assert
  if (Atomic::cmpxchg(&g_asserting, false, true) != false) {
    ::printf("Ignoring secondary assert in malloc trace...\n");
    return false;
  }

  // manually disable lock.
  Locker::unlock();

  // disable hooks (if this asserts too,
  // the assert is just ignored, see above)
  MallocTracer::disable();

  return true;
}

} // namespace sap

#endif // HAVE_GLIBC_MALLOC_HOOKS
