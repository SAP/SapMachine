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
 */

#include "precompiled.hpp"

#ifdef LINUX

#include "jvm_io.h"
#include "malloctrace/mallocTrace.hpp"
#include "malloctrace/siteTable.hpp"
#include "memory/allocation.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/ostream.hpp"
#include "unittest.hpp"

#ifdef HAVE_GLIBC_MALLOC_HOOKS

using sap::SiteTable;
using sap::Stack;

static void init_random_randomly() {
  os::init_random((int)os::elapsed_counter());
}

//#define LOG

static void fill_stack_randomly(sap::Stack* s) {
  for (unsigned i = 0; i < Stack::num_frames; i++) {
    s->_frames[i] = (address)(intptr_t)os::random();
  }
}

// Since SiteTable is too large to be put onto the stack of a test function,
// we need to create it dynamically. I don't want to make it a CHeapObj only
// for the sake of these tests though, so I have to use placement new.
static SiteTable* create_site_table() {
  void* p = NEW_C_HEAP_ARRAY(SiteTable, 1, mtTest);
  return new (p) SiteTable;
}

static void destroy_site_table(SiteTable* s) {
  FREE_C_HEAP_ARRAY(SiteTable, s);
}

// Helper, create an array of unique stacks, randomly filled; returned array is C-heap allocated
static Stack* create_unique_stack_array(int num) {
  Stack* random_stacks = NEW_C_HEAP_ARRAY(Stack, num, mtTest);
  for (int i = 0; i < num; i ++) {
    fill_stack_randomly(random_stacks + i);
    // ensure uniqueness
    random_stacks[i]._frames[0] = (address)(intptr_t)i;
  }
  return random_stacks;
}

static void test_print_table(const SiteTable* table, int expected_entries) {
  stringStream ss;

  table->print_stats(&ss);
  if (expected_entries != -1) {
    char match[32];
    jio_snprintf(match, sizeof(match),
                 "num_entries: %u,", expected_entries);
    ASSERT_NE(::strstr(ss.base(), match), (char*)NULL);
  }
  ss.reset();

  table->print_table(&ss, true);
  if (expected_entries != -1) {
    if (expected_entries > 0) {
      // Note, output buffer may not hold full output
      ASSERT_NE(::strstr(ss.base(), "--- 1 ---"), (char*)NULL);
    } else {
      ASSERT_NE(::strstr(ss.base(), "Table is empty"), (char*)NULL);
    }
  }
}

TEST_VM(MallocTrace, site_table_basics) {

  init_random_randomly();

  SiteTable* table = create_site_table();

  test_print_table(table, 0); // Test printing empty table.

  const unsigned safe_to_add_without_overflow = SiteTable::max_entries();

  // Generate a number of random stacks; enough to hit overflow limit from time to time.
  const int num_stacks = safe_to_add_without_overflow + 100;
  Stack* random_stacks = create_unique_stack_array(num_stacks);

  // Add n guaranteed-to-be-unique call stacks to the table; observe table; do that n times, which should
  // increase invoc counters.
  uint64_t expected_invocs = 0;
  unsigned expected_unique_callsites = 0;
  for (int invocs_per_stack = 0; invocs_per_stack < 10; invocs_per_stack++) {
    for (unsigned num_callstacks = 0; num_callstacks < safe_to_add_without_overflow; num_callstacks++) {
      table->add_site(random_stacks + num_callstacks, 1024);
      expected_invocs ++;
      if (invocs_per_stack == 0) {
        // On the first iteration we expect a new callsite table node to be created for this stack
        expected_unique_callsites++;
      }
      ASSERT_EQ(table->invocations(), expected_invocs);
      ASSERT_EQ(table->size(), expected_unique_callsites);  // Must be, since all stacks we add are be unique
      ASSERT_EQ(table->lost(), (uint64_t)0);                // So far we should see no losses
    }
  }
  test_print_table(table, expected_unique_callsites);
  DEBUG_ONLY(table->verify();)

  // Now cause table to overflow by adding further unique call stacks. Table should reject these new stacks
  // and count them in lost counter
  for (int overflow_num = 0; overflow_num < 100; overflow_num++) {
    table->add_site(random_stacks + safe_to_add_without_overflow + overflow_num, 1024);
    ASSERT_EQ(table->size(), expected_unique_callsites);                 // Should stay constant, no further adds should be accepted
    ASSERT_EQ(table->lost(), (uint64_t)(overflow_num + 1));              // Lost counter should go up
    ASSERT_EQ(table->invocations(), expected_invocs + overflow_num + 1); // Invocations counter includes lost
  }

  test_print_table(table, expected_unique_callsites);
  DEBUG_ONLY(table->verify();)

#ifdef LOG
  //table->print_table(tty, true);
  table->print_stats(tty);
  tty->cr();
#endif

  destroy_site_table(table);
}

TEST_VM(MallocTrace, site_table_random) {
  SiteTable* table = create_site_table();

  init_random_randomly();

  // Generate a number of random stacks; enough to hit overflow limit from time to time.
  const int num_stacks = SiteTable::max_entries() * 1.3;
  Stack* random_stacks = create_unique_stack_array(num_stacks);

  for (int i = 0; i < num_stacks; i ++) {
    fill_stack_randomly(random_stacks + i);
  }

  // Now register these stacks randomly, a lot of times.
  for (int i = 0; i < 1000*1000; i ++) {
    Stack* stack = random_stacks + (os::random() % num_stacks);
    table->add_site(stack, 1024);
    ASSERT_EQ(table->invocations(), (uint64_t)i + 1);
  }

  // test table printing, but we do not know how many unique stacks we have randomly generated, so don't
  // test the exact number of entries
  test_print_table(table, -1);

  DEBUG_ONLY(table->verify();)

  FREE_C_HEAP_ARRAY(Stack, random_stacks);

#ifdef LOG
  //table->print_table(tty, true);
  table->print_stats(tty);
  tty->cr();
#endif

  destroy_site_table(table);
}

#endif // HAVE_GLIBC_MALLOC_HOOKS

#endif // LINUX
