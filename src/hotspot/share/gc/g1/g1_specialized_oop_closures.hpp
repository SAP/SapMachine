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

#ifndef SHARE_VM_GC_G1_G1_SPECIALIZED_OOP_CLOSURES_HPP
#define SHARE_VM_GC_G1_G1_SPECIALIZED_OOP_CLOSURES_HPP

// The following OopClosure types get specialized versions of
// "oop_oop_iterate" that invoke the closures' do_oop methods
// non-virtually, using a mechanism defined in this file.  Extend these
// macros in the obvious way to add specializations for new closures.

// Forward declarations.

class G1ScanEvacuatedObjClosure;

class G1ScanObjsDuringUpdateRSClosure;
class G1ScanObjsDuringScanRSClosure;
class G1ConcurrentRefineOopClosure;

class G1CMOopClosure;
class G1RootRegionScanClosure;

class G1MarkAndPushClosure;
class G1AdjustAndRebuildClosure;

#define SPECIALIZED_OOP_OOP_ITERATE_CLOSURES_G1(f) \
      f(G1ScanEvacuatedObjClosure,_nv)             \
      f(G1ScanObjsDuringUpdateRSClosure,_nv)       \
      f(G1ScanObjsDuringScanRSClosure,_nv)         \
      f(G1ConcurrentRefineOopClosure,_nv)          \
      f(G1CMOopClosure,_nv)                        \
      f(G1RootRegionScanClosure,_nv)

#define SPECIALIZED_OOP_OOP_ITERATE_CLOSURES_G1FULL(f) \
      f(G1MarkAndPushClosure,_nv)                      \
      f(G1AdjustAndRebuildClosure,_nv)

#endif // SHARE_VM_GC_G1_G1_SPECIALIZED_OOP_CLOSURES_HPP
