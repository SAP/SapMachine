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

#ifndef OS_LINUX_MALLOCTRACE_SITETABLE_HPP
#define OS_LINUX_MALLOCTRACE_SITETABLE_HPP

#include "malloctrace/assertHandling.hpp"
#include "malloctrace/mallocTrace.hpp"
#include "utilities/globalDefinitions.hpp"

#ifdef HAVE_GLIBC_MALLOC_HOOKS

class outputStream;

namespace sap {

////////////////////////////////////////////////////
// We currently support two ways to get a stack trace:
// - using backtrace(3)
// - using an NMT-like callstack walker
// I am not sure yet which is better. Have to experiment.

enum capture_method_t {
  nmt_like = 0, using_backtrace = 1
};

///// Stack ////////////////////
// simple structure holding a fixed-sized native stack

struct Stack {
  static const int num_frames = 16;
  address _frames[num_frames];

  unsigned calculate_hash() const {
    uintptr_t hash = 0;
    for (int i = 0; i < num_frames; i++) {
      hash += (uintptr_t)_frames[i];
    }
    return hash;
  }

  void reset() {
    ::memset(_frames, 0, sizeof(_frames));
  }

  void copy_to(Stack* other) const {
    ::memcpy(other->_frames, _frames, sizeof(_frames));
  }

  bool equals(const Stack* other) const {
    return ::memcmp(_frames, other->_frames, sizeof(_frames)) == 0;
  }

  void print_on(outputStream* st) const;

  static bool capture_stack(Stack* stack, bool use_backtrace);

};

///// Site ////////////////////
// Stack + invocation counters

struct Site {
  Stack stack;
  uint64_t invocations;
  uint64_t invocations_delta;     // delta since last printing
  uint32_t min_alloc_size;        // min and max allocation size
  uint32_t max_alloc_size;        //  from that call site
                                  // (note: can be zero: we also trace zero-sized allocs since malloc(0)
                                  //  could also be a leak)
};

///// SiteTable ////////////////////
// A hashmap containing all captured malloc call sites.
// This map is kept very simple. We never remove entries, just
// reset the table as a whole. Space for the nodes is pre-allocated when
// the table is created to prevent malloc calls disturbing the statistics
// run.
class SiteTable {

  static const int _max_entries = 32 * K;

  struct Node {
    Node* next;
    Site site;
  };

  // We preallocate all nodes in this table to avoid
  // swamping the VM with internal malloc calls while the
  // trace is running.
  class NodeHeap {
    Node _nodes[SiteTable::_max_entries];
    int _used;
  public:
    NodeHeap() : _used(0) {
      ::memset(_nodes, 0, sizeof(_nodes));
    }
    Node* get_node() {
      Node* n = NULL;
      if (_used < SiteTable::_max_entries) {
        n = _nodes + _used;
        _used ++;
      }
      return n;
    }
    void reset() {
      ::memset(_nodes, 0, sizeof(_nodes));
      _used = 0;
    }
  };

  NodeHeap _nodeheap;
  const static int table_size = 8171; //prime
  Node* _table[table_size];

  unsigned _size;        // Number of entries
  uint64_t _invocations; // invocations (including lost)
  uint64_t _lost;        // lost adds due to table full
  uint64_t _collisions;  // hash collisions

  static unsigned slot_for_stack(const Stack* stack) {
    unsigned hash = stack->calculate_hash();
    malloctrace_assert(hash != 0, "sanity");
    return hash % table_size;
  }

public:

  SiteTable();

  void add_site(const Stack* stack, uint32_t alloc_size) {
    _invocations ++;

    const unsigned slot = slot_for_stack(stack);

    // Find entry
    for (Node* p = _table[slot]; p != NULL; p = p->next) {
      if (p->site.stack.equals(stack)) {
        // Call site already presented in table
        p->site.invocations ++;
        p->site.invocations_delta ++;
        p->site.max_alloc_size = MAX2(p->site.max_alloc_size, alloc_size);
        p->site.min_alloc_size = MIN2(p->site.min_alloc_size, alloc_size);
        return;
      } else {
        _collisions ++;
      }
    }

    Node* n = _nodeheap.get_node();
    if (n == NULL) { // hashtable too full, reject.
      assert(_size == max_entries(), "sanity");
      _lost ++;
      return;
    }
    n->site.invocations = n->site.invocations_delta = 1;
    n->site.max_alloc_size = n->site.min_alloc_size = alloc_size;
    stack->copy_to(&(n->site.stack));
    n->next = _table[slot];
    _table[slot] = n;
    _size ++;
  }

  void print_table(outputStream* st, bool raw) const;
  void print_stats(outputStream* st) const;
  void reset_deltas();
  void reset();
  DEBUG_ONLY(void verify() const;)

  // create a table from c-heap
  static SiteTable* create();

  // Maximum number of entries the table can hold.
  static unsigned max_entries() { return _max_entries; }

  // Number of entries currently in the table.
  unsigned size() const         { return _size; }

  // Number of invocations.
  uint64_t invocations() const  { return _invocations; }

  // Number of invocations lost because table was full.
  uint64_t lost() const         { return _lost; }

};

} // namespace sap

#endif // HAVE_GLIBC_MALLOC_HOOKS

#endif // OS_LINUX_MALLOCTRACE_SITETABLE_HPP
