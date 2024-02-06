/*
 * Copyright (c) 2023 SAP SE. All rights reserved.
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

#include <sys/types.h>
#include <stddef.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

#include "mallochooks.h"

// The log level. 0 is none, 1 is basic logging.
#define LOG_LEVEL 0

// If > 0 we sync after each write.
#define SYNC_WRITE 0

void write_safe(int fd, char const* buf, size_t len) {
  int errno_backup = errno;
  size_t left = len;
  ssize_t result;

  while ((result = write(fd, buf, left)) > 0) {
    buf += result;
    left -= result;
  }

#if SYNC_WRITE > 0
  fsync(fd);
#endif

  errno = errno_backup;
}

static void print_error(char const* msg) {
  write_safe(2, msg, strlen(msg));
}


static void unepected_call() {
  print_error("Uninitialzed function called. libmallochooks.so must the the first preloaded library.\n");
  exit(1);
}

// The tag for malloc functions which should be loaded by dl_sym.
#define LOAD_DYNAMIC  ((void*) unepected_call)

static size_t get_allocated_size(void* ptr) {
  if (ptr == NULL) {
    return 0;
  }

#if defined(__GLIBC__)
  return ((size_t*) ptr)[-1] & ~((size_t) 15);
#elif defined(__APPLE__)
  return malloc_size(ptr);
#elif defined (MUSL_LIBC)
  return malloc_usable_size(ptr);
#endif
}

#if defined(__APPLE__)

static real_funcs_t impl = {
  malloc,
  calloc,
  realloc,
  free,
  posix_memalign,
  NULL,
  NULL,
  valloc,
  NULL,
  get_allocated_size
};

#define REPLACE_NAME(x) x##_interpose

#elif defined(__GLIBC__)

void* __libc_malloc(size_t size);
void* __libc_calloc(size_t elems, size_t size);
void* __libc_realloc(void* ptr, size_t size);
void  __libc_free(void* ptr);
void* __libc_memalign(size_t align, size_t size);
void* __libc_valloc(size_t size);
void* __libc_pvalloc(size_t size);

static real_funcs_t impl = {
  __libc_malloc,
  __libc_calloc,
  __libc_realloc,
  __libc_free,
  (posix_memalign_func_t*) LOAD_DYNAMIC,
  __libc_memalign,
  (aligned_alloc_func_t*) LOAD_DYNAMIC,
  __libc_valloc,
  __libc_pvalloc,
  get_allocated_size
};

#elif defined(MUSL_LIBC)

static void* calloc_by_malloc(size_t elems, size_t size);
static int posix_memalign_by_aligned_alloc(void** ptr, size_t align, size_t size);
static void* memalign_by_aligned_alloc(size_t align, size_t size);

static real_funcs_t impl = {
  (malloc_func_t*) LOAD_DYNAMIC,
  calloc_by_malloc,
  (realloc_func_t*) LOAD_DYNAMIC,
  (free_func_t*) LOAD_DYNAMIC,
  posix_memalign_by_aligned_alloc,
  memalign_by_aligned_alloc,
  (aligned_alloc_func_t*) LOAD_DYNAMIC,
  NULL,
  NULL,
  get_allocated_size
};

/* musl calloc would call the redirected malloc, so we call the right malloc here. */
static void* calloc_by_malloc(size_t elems, size_t size) {
  /* Check for overflow */
  if (size > 0 && (elems > ((size_t) -1) / size)) {
    errno = ENOMEM;
    return NULL;
  }

  void* result = impl.malloc(elems * size);

  if (result != NULL) {
    bzero(result, elems * size);
  }

  return result;
}

/* musl posix_memalign would call the redirected aligned_alloc, so we call the right aligned_alloc here. */
static int posix_memalign_by_aligned_alloc(void** ptr, size_t align, size_t size) {
  void* result = impl.aligned_alloc(align, size);

  if (ptr != NULL) {
    *ptr = result;

    return 0;
  }

  return errno;
}

/* musl memalign would call the redirected aligned_alloc, so we call the right aligned_alloc here. */
static void* memalign_by_aligned_alloc(size_t align, size_t size) {
  return impl.aligned_alloc(align, size);
}

#else
#error "Unexpected platform"
#endif

#if LOG_LEVEL > 0

static void print(char const* str);
static void print_ptr(void* ptr);
static void print_size(size_t size);

#else

#define print_ptr(x)
#define print_size(x)
#define print(x)

#endif

#ifndef REPLACE_NAME
#define REPLACE_NAME(x) x
#endif

static void assign_function(void** dest, char const* symbol) {
  if (*dest != LOAD_DYNAMIC) {
    print("Don't need to load '");
    print(symbol);
    print("'\n");

    return;
  }

  print("Resolving '");
  print(symbol);
  print("'\n");

  *dest = dlsym(RTLD_NEXT, symbol);

  if (*dest == NULL) {
    print_error(symbol);
    print_error(" not found!\n");
    exit(1);
  }

  print("Found at ");
  print_ptr(*dest);
  print("\n");
}

#define LIB_INIT __attribute__((constructor))
#define EXPORT __attribute__((visibility("default")))

static void LIB_INIT init(void) {
  assign_function((void**) &impl.malloc, "malloc");
  assign_function((void**) &impl.calloc, "calloc");
  assign_function((void**) &impl.realloc, "realloc");
  assign_function((void**) &impl.free, "free");
  assign_function((void**) &impl.memalign, "memalign");
  assign_function((void**) &impl.posix_memalign, "posix_memalign");
  assign_function((void**) &impl.aligned_alloc, "aligned_alloc");
  assign_function((void**) &impl.valloc, "valloc");
  assign_function((void**) &impl.pvalloc, "pvalloc");
}

static registered_hooks_t empty_registered_hooks = {
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL
};

static registered_hooks_t* volatile registered_hooks = &empty_registered_hooks;

EXPORT registered_hooks_t* malloc_hooks_register_hooks(registered_hooks_t* hooks) {
  registered_hooks_t* old_hooks = registered_hooks;

  if (hooks == NULL) {
    print("Deregistered hooks\n");
    registered_hooks = &empty_registered_hooks;
  } else {
    print("Registered hooks\n");
    registered_hooks = hooks;
  }

  return old_hooks == &empty_registered_hooks ? NULL : old_hooks;
}

EXPORT registered_hooks_t* malloc_hooks_active_hooks() {
  if (registered_hooks == &empty_registered_hooks) {
    return NULL;
  }

  return (registered_hooks_t*) registered_hooks;
}

EXPORT real_funcs_t* malloc_hooks_get_real_funcs() {
  return &impl;
}

#if LOG_LEVEL > 0

#define LOG_FUNC(func) \
  print(#func);

#define LOG_ALIGN(align) \
  print(" alignment "); \
  print_size(align);

#define LOG_PTR(ptr) \
  print(" "); \
  print_ptr(ptr);

#define LOG_PTR_WITH_SIZE(ptr) \
  LOG_PTR(ptr); \
  if (ptr != NULL) { \
    size_t size = get_allocated_size(ptr); \
    if (size > 0) { \
      print(" (size "); \
      print_size(size); \
      print(")"); \
    } \
  }

#define LOG_ELEMS(elems) \
  print(" #elems "); \
  print_size(elems);

#define LOG_SIZE(size) \
  print(" size "); \
  print_size(size);

#define LOG_ALLOCATION_RESULT(result) \
  if (result == NULL) { \
    print(" failed with errno "); \
    print_size(errno); \
  } else { \
    print(" allocated at"); \
    LOG_PTR_WITH_SIZE(result); \
  }

#define LOG_RESULT(result) \
  print(" result "); \
  print_size(result);

#define LOG_HOOK \
  print(hook ? " with hook\n" : " without hook\n");

#else

#define LOG_FUNC(func)
#define LOG_ALIGN(align)
#define LOG_PTR(ptr)
#define LOG_PTR_WITH_SIZE(ptr)
#define LOG_ELEMS(elems)
#define LOG_SIZE(size)
#define LOG_ALLOCATION_RESULT(result)
#define LOG_RESULT(result)
#define LOG_HOOK

#endif

EXPORT void* REPLACE_NAME(malloc)(size_t size) {
  malloc_hook_t* hook = registered_hooks->malloc_hook;
  void* result;

  LOG_FUNC(malloc);
  LOG_SIZE(size);

  if (hook != NULL) {
    result = hook(size, __builtin_return_address(0));
  } else {
    result = impl.malloc(size);
  }

  LOG_ALLOCATION_RESULT(result);
  LOG_HOOK;

  return result;
}

EXPORT void* REPLACE_NAME(calloc)(size_t elems, size_t size) {
  calloc_hook_t* hook = registered_hooks->calloc_hook;
  void* result;

  LOG_FUNC(calloc);
  LOG_ELEMS(elems);
  LOG_SIZE(size);

  if (hook != NULL) {
    result = hook(elems, size, __builtin_return_address(0));
  } else {
    result = impl.calloc(elems, size);
  }

  LOG_ALLOCATION_RESULT(result);
  LOG_HOOK;

  return result;
}

EXPORT void* REPLACE_NAME(realloc)(void* ptr, size_t size) {
  realloc_hook_t* hook = registered_hooks->realloc_hook;
  void* result;

  LOG_FUNC(realloc);
  LOG_PTR_WITH_SIZE(ptr);
  LOG_SIZE(size);

  if (hook != NULL) {
    result = hook(ptr, size, __builtin_return_address(0));
  } else {
    result = impl.realloc(ptr, size);
  }

  LOG_ALLOCATION_RESULT(result);
  LOG_HOOK;

  return result;
}

EXPORT void REPLACE_NAME(free)(void* ptr) {
  free_hook_t* hook = registered_hooks->free_hook;

  LOG_FUNC(free);
  LOG_PTR_WITH_SIZE(ptr);

  if (hook != NULL) {
    hook(ptr, __builtin_return_address(0));
  } else {
    impl.free(ptr);
  }

  LOG_HOOK;
}

EXPORT int REPLACE_NAME(posix_memalign)(void** ptr, size_t align, size_t size) {
  posix_memalign_hook_t* hook = registered_hooks->posix_memalign_hook;
  int result;

  LOG_FUNC(posix_memalign);
  LOG_ALIGN(align);
  LOG_SIZE(size);

  if (hook != NULL) {
    result = hook(ptr, align, size, __builtin_return_address(0));
  } else {
    result = impl.posix_memalign(ptr, align, size);
  }

  LOG_ALLOCATION_RESULT(*ptr);
  LOG_RESULT(result);
  LOG_HOOK;

  return result;
}

#if !defined(__APPLE__)
EXPORT void* REPLACE_NAME(memalign)(size_t align, size_t size) {
  memalign_hook_t* hook = registered_hooks->memalign_hook;
  void* result;

  LOG_FUNC(memalign);
  LOG_ALIGN(align);
  LOG_SIZE(size);

  if (hook != NULL) {
    result = hook(align, size, __builtin_return_address(0));
  } else {
    result = impl.memalign(align, size);
  }

  LOG_ALLOCATION_RESULT(result);
  LOG_HOOK;

  return result;
}
#endif

EXPORT void* REPLACE_NAME(aligned_alloc)(size_t align, size_t size) {
  memalign_hook_t* hook = registered_hooks->aligned_alloc_hook;
  void* result;

  LOG_FUNC(aligned_alloc);
  LOG_ALIGN(align);
  LOG_SIZE(size);

  if (hook != NULL) {
    result = hook(align, size, __builtin_return_address(0));
  } else {
    result = impl.aligned_alloc(align, size);
  }

  LOG_ALLOCATION_RESULT(result);
  LOG_HOOK;

  return result;
}

#if !defined(MUSL_LIBC)
EXPORT void* REPLACE_NAME(valloc)(size_t size) {
  valloc_hook_t* hook = registered_hooks->valloc_hook;
  void* result;

  LOG_FUNC(valloc);
  LOG_SIZE(size);

  if (hook != NULL) {
    result = hook(size, __builtin_return_address(0));
  } else {
    result = impl.valloc(size);
  }

  LOG_ALLOCATION_RESULT(result);
  LOG_HOOK;

  return result;
}
#endif

#if !defined(MUSL_LIBC)
EXPORT void* REPLACE_NAME(pvalloc)(size_t size) {
  pvalloc_hook_t* hook = registered_hooks->pvalloc_hook;
  void* result;

  LOG_FUNC(pvalloc);
  LOG_SIZE(size);

  if (hook != NULL) {
    result = hook(size, __builtin_return_address(0));
  } else {
    result = impl.pvalloc(size);
  }

  LOG_ALLOCATION_RESULT(result);
  LOG_HOOK;

  return result;
}
#endif

#if defined(__APPLE__)

#define DYLD_INTERPOSE(_replacement,_replacee) \
   __attribute__((used)) static struct{ const void* replacement; const void* replacee; } _interpose_##_replacee \
   __attribute__ ((section ("__DATA,__interpose"))) = { (const void*)(unsigned long)&_replacement, (const void*)(unsigned long)&_replacee };

DYLD_INTERPOSE(REPLACE_NAME(malloc), malloc)
DYLD_INTERPOSE(REPLACE_NAME(calloc), calloc)
DYLD_INTERPOSE(REPLACE_NAME(realloc), realloc)
DYLD_INTERPOSE(REPLACE_NAME(free), free)
DYLD_INTERPOSE(REPLACE_NAME(posix_memalign), posix_memalign)

// We compile for 10.12 but aligned_alloc is only available in 10.15 and up
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
DYLD_INTERPOSE(REPLACE_NAME(aligned_alloc), aligned_alloc)
#pragma clang diagnostic pop

DYLD_INTERPOSE(REPLACE_NAME(valloc), valloc)

#endif


// D E B U G   C O D E


#if LOG_LEVEL > 0

#define DEBUG_FD 2

static void print(char const* str) {
  write_safe(DEBUG_FD, str, strlen(str));
}

static void print_ptr(void* ptr) {
  char buf[18];
  int shift = 64;
  buf[0] = '0';
  buf[1] = 'x';
  char* p = buf + 2;
  print("0x");

  do {
    shift -= 4;
    *p = "0123456789abcdef"[((((size_t) ptr) >> shift) & 15)];
    ++p;
  } while (shift > 0);

  write_safe(DEBUG_FD, buf, p - buf);
}

static void print_size(size_t size) {
  char buf[20];
  size_t pos = sizeof(buf);

  do {
    buf[--pos] = '0' + (size % 10);
    size /= 10;
  }  while (size > 0);

  write_safe(DEBUG_FD, buf + pos, sizeof(buf) - pos);
}
#endif

