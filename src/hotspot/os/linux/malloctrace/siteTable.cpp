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
#include "code/codeBlob.hpp"
#include "code/codeCache.hpp"
#include "malloctrace/assertHandling.hpp"
#include "malloctrace/mallocTrace.hpp"
#include "malloctrace/siteTable.hpp"
#include "runtime/frame.inline.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"

#include <malloc.h>

#ifdef HAVE_GLIBC_MALLOC_HOOKS

// Pulled from JDK17 (see os::print_function_and_library_name()).
// Note: Abridged version, does not handle function descriptors, which only concerns ppc64.
// But since these are real code pointers, not function descriptors, this should be fine.
static bool print_function_and_library_name(outputStream* st,
                                            address addr,
                                            char* buf, int buflen,
                                            bool shorten_paths,
                                            bool demangle,
                                            bool strip_arguments) {
  // If no scratch buffer given, allocate one here on stack.
  // (used during error handling; its a coin toss, really, if on-stack allocation
  //  is worse than (raw) C-heap allocation in that case).
  char* p = buf;
  if (p == NULL) {
    p = (char*)::alloca(O_BUFLEN);
    buflen = O_BUFLEN;
  }
  int offset = 0;
  bool have_function_name = os::dll_address_to_function_name(addr, p, buflen,
                                                             &offset, demangle);
  bool is_function_descriptor = false;

  if (have_function_name) {
    // Print function name, optionally demangled
    if (demangle && strip_arguments) {
      char* args_start = strchr(p, '(');
      if (args_start != NULL) {
        *args_start = '\0';
      }
    }
    // Print offset. Omit printing if offset is zero, which makes the output
    // more readable if we print function pointers.
    if (offset == 0) {
      st->print("%s", p);
    } else {
      st->print("%s+%d", p, offset);
    }
  } else {
    st->print(PTR_FORMAT, p2i(addr));
  }
  offset = 0;

  const bool have_library_name = os::dll_address_to_library_name(addr, p, buflen, &offset);
  if (have_library_name) {
    // Cut path parts
    if (shorten_paths) {
      char* p2 = strrchr(p, os::file_separator()[0]);
      if (p2 != NULL) {
        p = p2 + 1;
      }
    }
    st->print(" in %s", p);
    if (!have_function_name) { // Omit offset if we already printed the function offset
      st->print("+%d", offset);
    }
  }

  return have_function_name || have_library_name;
}
// End: print_function_and_library_name picked from JDK17

namespace sap {

/////// Wrapper for the glibc backtrace(3) function;
// (we need to load it dynamically since it is not always guaranteed to be there.)

class BackTraceWrapper {
  typedef int (*backtrace_fun_t) (void **buffer, int size);
  backtrace_fun_t _fun = NULL;
  static backtrace_fun_t load_symbol() {
    ::dlerror(); // clear state
    void* sym = ::dlsym(RTLD_DEFAULT, "backtrace");
    if (sym != NULL && ::dlerror() == NULL) {
      return (backtrace_fun_t)sym;
    }
    return NULL;
  }
public:
  BackTraceWrapper() : _fun(load_symbol()) {}

  // Capture a stack using backtrace(3); return true on success.
  bool capture(Stack* stack) const {
    if (_fun == NULL) {
      return false;
    }
    return _fun((void**)stack->_frames, Stack::num_frames) > 0;
  }
};

static BackTraceWrapper g_backtrace_wrapper;

/////// NMT-like callstack function

static bool capture_stack_nmt_like(Stack* stack) {
  int frame_idx = 0;
  int num_frames = 0;
  frame fr = os::current_frame();
  while (fr.pc() && frame_idx < Stack::num_frames) {
    stack->_frames[frame_idx ++] = fr.pc();
    num_frames++;
    if (fr.fp() == NULL || fr.cb() != NULL ||
        fr.sender_pc() == NULL || os::is_first_C_frame(&fr)) break;
    if (fr.sender_pc() && !os::is_first_C_frame(&fr)) {
      fr = os::get_sender_for_C_frame(&fr);
    } else {
      break;
    }
  }
  return num_frames > 0;
}

void Stack::print_on(outputStream* st) const {
  char tmp[256];
  for (int i = 0; i < num_frames && _frames[i] != NULL; i++) {
    st->print("[" PTR_FORMAT "] ", p2i(_frames[i]));
    if (print_function_and_library_name(st, _frames[i], tmp, sizeof(tmp), true, true, false)) {
      st->cr();
    }
  }
}

// Capture stack; try both methods and use the result from the
// one getting the better results.
bool Stack::capture_stack(Stack* stack, bool use_backtrace) {
  stack->reset();
  return use_backtrace ? g_backtrace_wrapper.capture(stack) : capture_stack_nmt_like(stack);
}

#ifdef ASSERT
void SiteTable::verify() const {
  unsigned num_sites_found = 0;
  uint64_t num_invocations_found = 0;
  for (unsigned slot = 0; slot < table_size; slot ++) {
    for (Node* n = _table[slot]; n != NULL; n = n->next) {
      num_sites_found ++;
      num_invocations_found += n->site.invocations;
      malloctrace_assert(slot_for_stack(&n->site.stack) == slot, "hash mismatch");
      malloctrace_assert(n->site.invocations > 0, "sanity");
      malloctrace_assert(n->site.invocations >= n->site.invocations_delta, "sanity");
    }
  }
  malloctrace_assert(num_sites_found <= _max_entries && num_sites_found == _size,
         "mismatch (found: %u, max: %u, size: %u)", num_sites_found, _max_entries, _size);
  malloctrace_assert(num_invocations_found + _lost == _invocations,
         "mismatch (" UINT64_FORMAT " vs " UINT64_FORMAT, num_invocations_found, _invocations);
  malloctrace_assert(num_sites_found <= max_entries(), "sanity");
}
#endif // ASSERT

SiteTable::SiteTable() {
  reset();
}

void SiteTable::reset_deltas() {
  for (unsigned slot = 0; slot < table_size; slot ++) {
    for (Node* n = _table[slot]; n != NULL; n = n->next) {
      n->site.invocations_delta = 0;
    }
  }
}

void SiteTable::reset() {
  _size = 0;
  _invocations = _lost = _collisions = 0;
  ::memset(_table, 0, sizeof(_table));
  _nodeheap.reset();
};

SiteTable* SiteTable::create() {
  void* p = ::malloc(sizeof(SiteTable));
  return new(p) SiteTable;
}

void SiteTable::print_stats(outputStream* st) const {
  unsigned longest_chain = 0;
  unsigned used_slots = 0;
  for (unsigned slot = 0; slot < table_size; slot ++) {
    unsigned len = 0;
    for (Node* n = _table[slot]; n != NULL; n = n->next) {
      len ++;
    }
    longest_chain = MAX2(len, longest_chain);
    if (len > 1) {
      used_slots ++;
    }
  }
  // Note: if you change this format, check gtest test_site_table parser.
  st->print("Table size: %u, num_entries: %u, used slots: %u, longest chain: %u, invocs: "
             UINT64_FORMAT ", lost: " UINT64_FORMAT ", collisions: " UINT64_FORMAT,
             table_size, _size, used_slots, longest_chain,
             _invocations, _lost, _collisions);
}

// Sorting stuff for printing the table

static int qsort_helper(const void* s1, const void* s2) {
  return ((const Site*)s2)->invocations > ((const Site*)s1)->invocations ? 1 : -1;
}

void SiteTable::print_table(outputStream* st, bool all) const {

  if (_size == 0) {
    st->print_cr("Table is empty.");
  }

  // We build up an index array of the filtered entries, then sort it by invocation counter.
  unsigned num_entries = 0;
  Site* const sorted_items = NEW_C_HEAP_ARRAY(Site, _size, mtInternal);

  for (unsigned slot = 0; slot < table_size; slot ++) {
    for (Node* n = _table[slot]; n != NULL; n = n->next) {
      if (n->site.invocations > 0) {
        sorted_items[num_entries] = n->site;
        num_entries ++;
      }
    }
  }
  malloctrace_assert(num_entries <= _size, "sanity");
  malloctrace_assert(num_entries <= max_entries(), "sanity");
  ::qsort(sorted_items, num_entries, sizeof(Site), qsort_helper);

  int rank = 0;
  const unsigned max_show = all ? _size : MIN2(_size, (unsigned)10);
  if (max_show < _size) {
    st->print_cr("---- %d hottest malloc sites: ----", max_show);
  }
  for (unsigned i = 0; i < max_show; i ++) {
    // For each call site, print out ranking, number of invocation,
    //  alloc size or alloc size range if non-uniform sizes, and stack.
    st->print_cr("---- %d ----", i);
    st->print_cr("Invocs: " UINT64_FORMAT " (+" UINT64_FORMAT ")",
                 sorted_items[i].invocations, sorted_items[i].invocations_delta);
    if (sorted_items[i].max_alloc_size == sorted_items[i].min_alloc_size) {
      st->print_cr("Alloc Size: " UINT32_FORMAT, sorted_items[i].max_alloc_size);
    } else {
      st->print_cr("Alloc Size Range: " UINT32_FORMAT " - " UINT32_FORMAT,
                   sorted_items[i].min_alloc_size, sorted_items[i].max_alloc_size);
    }
    sorted_items[i].stack.print_on(st);
  }
  if (max_show < _size) {
    st->print_cr("---- %d entries omitted - use \"all\" to print full table.",
                 _size - max_show);
  }
  st->cr();
  FREE_C_HEAP_ARRAY(Site, sorted_items);
}


} // namespace sap

#endif // HAVE_GLIBC_MALLOC_HOOKS
