/*
 * Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_SERIAL_SERIALHEAP_HPP
#define SHARE_VM_GC_SERIAL_SERIALHEAP_HPP

#include "gc/shared/genCollectedHeap.hpp"
#include "utilities/growableArray.hpp"

class GenCollectorPolicy;
class GCMemoryManager;
class MemoryPool;

class SerialHeap : public GenCollectedHeap {
private:
  MemoryPool* _eden_pool;
  MemoryPool* _survivor_pool;
  MemoryPool* _old_pool;

  virtual void initialize_serviceability();

protected:
  virtual void check_gen_kinds();

public:
  SerialHeap(GenCollectorPolicy* policy);

  virtual Name kind() const {
    return CollectedHeap::SerialHeap;
  }

  virtual const char* name() const {
    return "Serial";
  }

  virtual GrowableArray<GCMemoryManager*> memory_managers();
  virtual GrowableArray<MemoryPool*> memory_pools();

  // override
  virtual bool is_in_closed_subset(const void* p) const {
    return is_in(p);
  }

  virtual bool card_mark_must_follow_store() const {
    return false;
  }
};

#endif // SHARE_VM_GC_CMS_CMSHEAP_HPP
