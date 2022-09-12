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

#include "jvm_io.h"
#include "malloctrace/assertHandling.hpp"
#include "malloctrace/locker.hpp"
#include "malloctrace/mallocTrace.hpp"
#include "malloctrace/siteTable.hpp"
#include "memory/allStatic.hpp"
#include "runtime/globals.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"

#include <malloc.h>

#ifdef HAVE_GLIBC_MALLOC_HOOKS

// Since JDK-8289633 we forbid calling raw C-heap allocation functions using Kim's FORBID_C_FUNCTION.
// Callers need to explicitly opt in with ALLOW_C_FUNCTION.
// Since this code calls raw C-heap functions as a matter of course, instead of marking each call site
// with ALLOW_C_FUNCTION(..), I just mark them wholesale.
#if (__GNUC__ >= 10)
PRAGMA_DISABLE_GCC_WARNING("-Wattribute-warning")
#endif

namespace sap {

// Needed to stop the gcc from complaining about malloc hooks being deprecated.
PRAGMA_DISABLE_GCC_WARNING("-Wdeprecated-declarations")

typedef void* (*malloc_hook_fun_t) (size_t len, const void* caller);
typedef void* (*realloc_hook_fun_t) (void* old, size_t len, const void* caller);
typedef void* (*memalign_hook_fun_t) (size_t alignment, size_t size, const void* caller);

static void* my_malloc_hook(size_t size, const void *caller);
static void* my_realloc_hook(void* old, size_t size, const void *caller);
static void* my_memalign_hook(size_t alignment, size_t size, const void *caller);

// Hook changes, hook ownership:
//
// Hooks are a global resource and everyone can change them concurrently. In practice
// this does not happen often, so using them for our purposes here is generally safe
// and we can generally rely on us being the sole changer of hooks.
//
// Exceptions:
// 1) gdb debugging facilities like mtrace() or MALLOC_CHECK_ use them too
// 2)  there is a initialization race: both hooks are initially set to glibc-internal
//    initialization functions which will do some stuff, them set them to NULL for the
//    rest of the program run. These init functions (malloc_hook_ini() and realloc_hook_ini()),
//    see malloc/hooks.c) run *lazily*, the first time malloc or realloc is called.
//    So there is a race window here where we could possibly install our hooks while
//    some other thread calls realloc, still sees the original function pointer, executed
//    the init function and resets our hook. To make matters worse and more surprising, the
//    realloc hook function also resets the malloc hook for some reason (I consider this a
//    bug since realloc(3) may run way later than malloc(3)).
//
// There is nothing we can do about (1) except, well, not do it. About (2), we can effectively
//  prevent that from happening by calling malloc and realloc very early. The earliest we
//  can manage is during C++ dyn init of the libjvm:
struct RunAtDynInit {
  RunAtDynInit() {
    // Call malloc, realloc, free, calloc and posix_memalign.
    // This may be overkill, but I want all hooks to have executed once, in case
    // they have side effects on the other hooks (like the realloc hook which resets the malloc
    // hook)
    void* p = ::malloc(10);
    p = ::realloc(p, 20);
    ::free(p);
    if (::posix_memalign(&p, 8, 10) == 0) {
      ::free(p);
    }
  }
};
static RunAtDynInit g_run_at_dyn_init;

class HookControl : public AllStatic {
  static bool _hooks_are_active;
  static malloc_hook_fun_t    _old_malloc_hook;
  static realloc_hook_fun_t   _old_realloc_hook;
  static memalign_hook_fun_t  _old_memalign_hook;

public:

#ifdef ASSERT
  static char* print_hooks(char* out, size_t outlen) {
    jio_snprintf(out, outlen, "__malloc_hook=" PTR_FORMAT ", __realloc_hook=" PTR_FORMAT ", __memalign_hook=" PTR_FORMAT ", "
                 "my_malloc_hook=" PTR_FORMAT ", my_realloc_hook=" PTR_FORMAT ", my_memalign_hook=" PTR_FORMAT ".",
                 (intptr_t)__malloc_hook, (intptr_t)__realloc_hook, (intptr_t)__memalign_hook,
                 (intptr_t)my_malloc_hook,  (intptr_t)my_realloc_hook,  (intptr_t)my_memalign_hook);
    return out;
  }
  static void verify() {
    char tmp[256];
    if (_hooks_are_active) {
      malloctrace_assert(__malloc_hook == my_malloc_hook && __realloc_hook == my_realloc_hook &&
                         __memalign_hook == my_memalign_hook,
                         "Hook mismatch (expected my hooks to be active). Hook state: %s",
                         print_hooks(tmp, sizeof(tmp)));
    } else {
      malloctrace_assert(__malloc_hook != my_malloc_hook && __realloc_hook != my_realloc_hook &&
                         __memalign_hook != my_memalign_hook,
                         "Hook mismatch (expected default hooks to be active). Hook state: %s",
                         print_hooks(tmp, sizeof(tmp)));
    }
  }
#endif

  // Return true if my hooks are active
  static bool hooks_are_active() {
    DEBUG_ONLY(verify();)
    return _hooks_are_active;
  }

  static void enable() {
    DEBUG_ONLY(verify();)
    malloctrace_assert(!hooks_are_active(), "Sanity");
    _old_malloc_hook = __malloc_hook;
    __malloc_hook = my_malloc_hook;
    _old_realloc_hook = __realloc_hook;
    __realloc_hook = my_realloc_hook;
    _old_memalign_hook = __memalign_hook;
    __memalign_hook = my_memalign_hook;
    _hooks_are_active = true;
  }

  static void disable() {
    DEBUG_ONLY(verify();)
    malloctrace_assert(hooks_are_active(), "Sanity");
    __malloc_hook = _old_malloc_hook;
    __realloc_hook = _old_realloc_hook;
    __memalign_hook = _old_memalign_hook;
    _hooks_are_active = false;
  }
};

bool HookControl::_hooks_are_active = false;
malloc_hook_fun_t HookControl::_old_malloc_hook = NULL;
realloc_hook_fun_t HookControl::_old_realloc_hook = NULL;
memalign_hook_fun_t HookControl::_old_memalign_hook = NULL;

// A stack mark for temporarily disabling hooks - if they are active - and
// restoring the old state
class DisableHookMark {
  const bool _state;
public:
  DisableHookMark() : _state(HookControl::hooks_are_active()) {
    if (_state) {
      HookControl::disable();
    }
  }
  ~DisableHookMark() {
    if (_state) {
      HookControl::enable();
    }
  }
};

/////////////////////////////////////////////////////////////////

static SiteTable* g_sites = NULL;

static bool g_use_backtrace = true;
static uint64_t g_num_captures = 0;
static uint64_t g_num_captures_without_stack = 0;

#ifdef ASSERT
static int g_times_enabled = 0;
static int g_times_printed = 0;
#endif

#define CAPTURE_STACK_AND_ADD_TO_SITE_TABLE \
{ \
  Stack stack; \
  if (Stack::capture_stack(&stack, g_use_backtrace)) {  \
    malloctrace_assert(g_sites != NULL, "Site table not allocated");  \
    g_sites->add_site(&stack, alloc_size); \
  } else { \
    g_num_captures_without_stack ++; \
  } \
}

static void* my_malloc_or_realloc_hook(void* old, size_t alloc_size) {
  Locker lck;
  g_num_captures ++;

  // If someone switched off tracing while we waited for the lock, just quietly do
  // malloc/realloc and tippytoe out of this function. Don't modify hooks, don't
  // collect stacks.
  if (HookControl::hooks_are_active() == false) {
    return old != NULL ? ::realloc(old, alloc_size) : ::malloc(alloc_size);
  }

  // From here on disable hooks. We will collect a stack, then register it with
  // the site table, then call the real malloc to satisfy the allocation for the
  // caller. All of these things may internally malloc (even the sitemap, which may
  // assert). These recursive mallocs should not end up in this hook otherwise we
  // deadlock.
  //
  // Concurrency note: Concurrent threads will not be disturbed by this since:
  // - either they already entered this function, in which case they wait at the lock
  // - or they call malloc/realloc after we restored the hooks. In that case they
  //   just will end up doing the original malloc. We loose them for the statistic,
  //   but we wont disturb them, nor they us.
  //   (caveat: we assume here that the order in which we restore the hooks - which
  //    will appear random for outside threads - does not matter. After studying the
  //    glibc sources, I believe it does not.)
  HookControl::disable();

  CAPTURE_STACK_AND_ADD_TO_SITE_TABLE

  // Now do the actual allocation for the caller
  void* p = old != NULL ? ::realloc(old, alloc_size) : ::malloc(alloc_size);

#ifdef ASSERT
  if ((g_num_captures % 10000) == 0) { // expensive, do this only sometimes
    g_sites->verify();
  }
#endif

  // Reinstate my hooks
  HookControl::enable();

  return p;
}

static void* my_malloc_hook(size_t size, const void *caller) {
  return my_malloc_or_realloc_hook(NULL, size);
}

static void* my_realloc_hook(void* old, size_t size, const void *caller) {
  // realloc(0): "If size was equal to 0, either NULL or a pointer suitable to be passed to free() is returned."
  // The glibc currently does the former (unlike malloc(0), which does the latter and can cause leaks). As long
  // as we are sure the glibc returns NULL for realloc(0), we can shortcut here.
  if (size == 0) {
    return NULL;
  }
  return my_malloc_or_realloc_hook(old, size);
}

static void* posix_memalign_wrapper(size_t alignment, size_t size) {
  void* p = NULL;
  if (::posix_memalign(&p, alignment, size) == 0) {
    return p;
  }
  return NULL;
}

static void* my_memalign_hook(size_t alignment, size_t alloc_size, const void *caller) {
  Locker lck;
  g_num_captures ++;

  // For explanations, see my_malloc_or_realloc_hook

  if (HookControl::hooks_are_active() == false) {
    return posix_memalign_wrapper(alignment, alloc_size);
  }

  HookControl::disable();

  CAPTURE_STACK_AND_ADD_TO_SITE_TABLE

  // Now do the actual allocation for the caller
  void* p = posix_memalign_wrapper(alignment, alloc_size);

#ifdef ASSERT
  if ((g_num_captures % 10000) == 0) { // expensive, do this only sometimes
    g_sites->verify();
  }
#endif

  // Reinstate my hooks
  HookControl::enable();

  return p;
}


/////////// Externals /////////////////////////

bool MallocTracer::enable(bool use_backtrace) {
  Locker lck;
  if (!HookControl::hooks_are_active()) {
    if (g_sites == NULL) {
      // First time malloc trace is enabled, allocate the site table. We don't want to preallocate it
      // unconditionally since it costs several MB.
      g_sites = SiteTable::create();
      if (g_sites == NULL) {
        return false;
      }
    }
    HookControl::enable(); // << from this moment on concurrent threads may enter our hooks but will then wait on the lock
    g_use_backtrace = use_backtrace;
    DEBUG_ONLY(g_times_enabled ++;)
  }
  return true;
}

void MallocTracer::disable() {
  Locker lck;
  if (HookControl::hooks_are_active()) {
    HookControl::disable();
  }
}

void MallocTracer::reset() {
  Locker lck;
  if (g_sites != NULL) {
    g_sites->reset();
    g_num_captures = g_num_captures_without_stack = 0;
  }
}

void MallocTracer::reset_deltas() {
  Locker lck;
  if (g_sites != NULL) {
    g_sites->reset_deltas();
  }
}

void MallocTracer::print(outputStream* st, bool all) {
  Locker lck;
  if (g_sites != NULL) {
    bool state_now = HookControl::hooks_are_active(); // query hooks before temporarily disabling them
    {
      DisableHookMark disableHookMark;
      g_sites->print_table(st, all);
      g_sites->print_stats(st);
      st->cr();
      st->print_cr("Malloc trace %s.", state_now ? "on" : "off");
      if (state_now) {
        st->print_cr(" (method: %s)", g_use_backtrace ? "backtrace" : "nmt-ish");
      }
      st->cr();
      st->print_cr(UINT64_FORMAT " captures (" UINT64_FORMAT " without stack).", g_num_captures, g_num_captures_without_stack);
      DEBUG_ONLY(g_times_printed ++;)
      DEBUG_ONLY(st->print_cr("%d times enabled, %d times printed", g_times_enabled, g_times_printed));
      DEBUG_ONLY(g_sites->verify();)
      // After each print, we reset table deltas
      g_sites->reset_deltas();
    }
  } else {
    // Malloc trace has never been activated.
    st->print_cr("Malloc trace off.");
  }
}

void MallocTracer::print_on_error(outputStream* st) {
  // Don't lock. Don't change hooks. Just print the table stats.
  if (g_sites != NULL) {
    g_sites->print_stats(st);
  }
}

///////////////////////

// test: enable at libjvm load
// struct AutoOn { AutoOn() { MallocTracer::enable(); } };
// static AutoOn g_autoon;

} // namespace sap

#endif // HAVE_GLIBC_MALLOC_HOOKS
