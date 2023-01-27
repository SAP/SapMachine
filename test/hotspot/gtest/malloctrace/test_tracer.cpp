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

#include "malloctrace/mallocTrace.hpp"
#include "memory/allocation.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/ostream.hpp"

#include "concurrentTestRunner.inline.hpp"
#include "unittest.hpp"
#include <malloc.h>

#ifdef HAVE_GLIBC_MALLOC_HOOKS

// Since JDK-8289633 we forbid calling raw C-heap allocation functions using Kim's FORBID_C_FUNCTION.
// Callers need to explicitly opt in with ALLOW_C_FUNCTION.
// Since this code calls raw C-heap functions as a matter of course, instead of marking each call site
// with ALLOW_C_FUNCTION(..), I just mark them wholesale.
#if (__GNUC__ >= 10)
PRAGMA_DISABLE_GCC_WARNING("-Wattribute-warning")
#endif

using sap::MallocTracer;

static void init_random_randomly() {
  os::init_random((int)os::elapsed_counter());
}

//#define LOG

static size_t random_size() { return os::random() % 123; }

static void test_print_statistics() {
  stringStream ss;

  MallocTracer::print_on_error(&ss); // Test print on error
  ASSERT_NE(::strstr(ss.base(), "num_entries:"), (char*)NULL);
  ss.reset();

  MallocTracer::print(&ss, false);
}

struct MyTestRunnable_raw_malloc : public TestRunnable {
  void runUnitTest() const {
    void* p = ::malloc(random_size());
    if (os::random() % 2) {
      p = ::realloc(p, random_size());
    }
    ::free(p);
  }
};

struct MyTestRunnable_raw_memalign : public TestRunnable {
  void runUnitTest() const {
    void* p = NULL;
    // note min alignment for posix_memalign is sizeof(void*)
    size_t alignment = 1 << (4 + (os::random() % 4)); // 16 ... 256
    int rc = ::posix_memalign(&p, alignment, random_size());
    assert(rc == 0 && p != NULL && is_aligned(p, alignment),
           "bad memalign result %d, " PTR_FORMAT, rc, p2i(p));
    ::free(p);
  }
};

struct MyTestRunnable_os_malloc : public TestRunnable {
  void runUnitTest() const {
    void* p = os::malloc(random_size(), mtTest);
    if (os::random() % 2) {
      p = os::realloc(p, random_size(), mtTest);
    }
    os::free(p);
  }
};

struct MyTestRunnable_mixed_all : public TestRunnable {
  void runUnitTest() const {
    char buf[128]; // truncation ok and expected
    stringStream ss(buf, sizeof(buf));
    int chance = os::random() % 100;
    if (chance < 20) {
      (void) MallocTracer::disable();
      os::naked_short_sleep(1);
      (void) MallocTracer::enable(true);
    } else if (chance < 25) {
      MallocTracer::print(&ss, false);
    } else {
      void* p = ::malloc(random_size());
      if (os::random() % 2) {
        p = ::realloc(p, random_size());
      }
      ::free(p);
    }
  }
};

// Mark to switch on tracing and restore the old state
class TraceRestorer {
  const bool _restore;
public:
  TraceRestorer() : _restore(MallocTracer::enable(true)) {}
  ~TraceRestorer() {
    if (_restore) {
      MallocTracer::disable();
    }
  }
};

TEST_VM(MallocTrace, tracer_os_malloc) {
  init_random_randomly();
  TraceRestorer restorer;
  MyTestRunnable_os_malloc my_runnable;
  ConcurrentTestRunner testRunner(&my_runnable, 5, 3000);
  testRunner.run();
  test_print_statistics();
#ifdef LOG
  MallocTracer::print(tty, false);
#endif
}

TEST_VM(MallocTrace, tracer_raw_malloc) {
  init_random_randomly();
  TraceRestorer restorer;
  MyTestRunnable_raw_malloc my_runnable;
  ConcurrentTestRunner testRunner(&my_runnable, 5, 3000);
  testRunner.run();
  test_print_statistics();
#ifdef LOG
  MallocTracer::print(tty, false);
#endif
}

TEST_VM(MallocTrace, tracer_raw_memalign) {
  init_random_randomly();
  TraceRestorer restorer;
  MyTestRunnable_raw_memalign my_runnable;
  ConcurrentTestRunner testRunner(&my_runnable, 5, 2000);
  testRunner.run();
  test_print_statistics();
#ifdef LOG
  MallocTracer::print(tty, false);
#endif
}

TEST_VM(MallocTrace, tracer_mixed_all) {
  init_random_randomly();
  TraceRestorer restorer;
  MyTestRunnable_mixed_all my_runnable;
  ConcurrentTestRunner testRunner(&my_runnable, 5, 3000);
  testRunner.run();
  test_print_statistics();
#ifdef LOG
  MallocTracer::print(tty, false);
#endif
}

#endif // HAVE_GLIBC_MALLOC_HOOKS

#endif // LINUX
