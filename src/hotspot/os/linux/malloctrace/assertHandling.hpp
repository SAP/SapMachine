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

#ifndef OS_LINUX_MALLOCTRACE_ASSERTHANDLING_HPP
#define OS_LINUX_MALLOCTRACE_ASSERTHANDLING_HPP

#include "malloctrace/mallocTrace.hpp"
#include "utilities/globalDefinitions.hpp"

#ifdef HAVE_GLIBC_MALLOC_HOOKS

namespace sap {

// Asserts in the malloctrace code need a bit of extra attention.
// We must prevent the assert handler itself deadlocking. Therefore,
// before executing the assert, we:
// - must prevent recursive assert from the malloc tracer
// - manually disable the lock to prevent recursive locking (since error reporting
//    never rolls back the stack this is okay)
// - disable malloc hooks

#ifdef ASSERT

bool prepare_assert();

#define malloctrace_assert(cond, ...)                                                         \
do {                                                                                          \
  if (!(cond) && prepare_assert()) {                                                          \
    report_vm_error(__FILE__, __LINE__, "malloctrace_assert(" #cond ") failed", __VA_ARGS__); \
  }                                                                                           \
} while (0)
#else
#define malloctrace_assert(cond, ...)
#endif

} // namespace sap

#endif // __GLIBC__

#endif // OS_LINUX_MALLOCTRACE_ASSERTHANDLING_HPP
