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

// The level of testing (0: none, 1: just debug output, 2: additional normal allocations
// during startup, 3: additional aligned allocations during startup, 4: use fallbacks
// whenever possible
#define TEST_LEVEL 0

// If > 0 we sync after each write.
#define SYNC_WRITE 0

void write_safe(int fd, char const* buf, size_t len) {
  int errno_backup = errno;

  write(fd, buf, len);

#if SYNC_WRITE > 0
  fsync(fd);
#endif

  errno = errno_backup;
}

#if defined(__APPLE__)

#define MALLOC_REPLACEMENT         malloc
#define CALLOC_REPLACEMENT         calloc
#define REALLOC_REPLACEMENT        realloc
#define FREE_REPLACEMENT           free
#define POSIX_MEMALIGN_REPLACEMENT posix_memalign
#define MEMALIGN_REPLACEMENT       NULL
#define ALIGNED_ALLOC_REPLACEMENT  NULL
#define VALLOC_REPLACEMENT         valloc
#define PVALLOC_REPLACEMENT        NULL

#define REPLACE_NAME(x) x##_interpose
#define NO_SYMBOL_LOADING

#elif defined(__GLIBC__)

void* __libc_malloc(size_t size);
void* __libc_calloc(size_t elems, size_t size);
void* __libc_realloc(void* ptr, size_t size);
void  __libc_free(void* ptr);
void* __libc_memalign(size_t align, size_t size);
void* __libc_valloc(size_t size);
void* __libc_pvalloc(size_t size);

#if TEST_LEVEL < 4
#define MALLOC_REPLACEMENT   __libc_malloc
#define CALLOC_REPLACEMENT   __libc_calloc
#define REALLOC_REPLACEMENT  __libc_realloc
#define FREE_REPLACEMENT     __libc_free
#define MEMALIGN_REPLACEMENT __libc_memalign
#define VALLOC_REPLACEMENT   __libc_valloc
#define PVALLOC_REPLACEMENT  __libc_pvalloc
#endif

#elif defined(_AIX)

#else
  // This must be musl. Since they are cool they don't set a define.
#define __THIS_IS_MUSL__

#define VALLOC_REPLACEMENT NULL
#define PVALLOC_REPLACEMENT NULL

#endif

#if TEST_LEVEL > 0

static void print(char const* str);
static void print_ptr(void* ptr);
static void print_size(size_t size);

#else

#define print_ptr(x)
#define print_size(x)
#define print(x)

#endif

static void print_error(char const* msg, int error_code) {
  write_safe(2, msg, strlen(msg));

  if (error_code > 0) {
    exit(error_code);
  }
}

#if !defined(MALLOC_REPLACEMENT)

#if TEST_LEVEL > 1
static char  fallback_buffer[32 * 1024 * 1024];
#else
static char  fallback_buffer[1024 * 1024];
#endif

static char* fallback_buffer_pos = fallback_buffer;
static char* fallback_buffer_end = &fallback_buffer[sizeof(fallback_buffer)];

static void* fallback_malloc(size_t size) {
  // Align to 16 byte and add 16 bytes for the header.
  size_t real_size = 16 + ((size - 1) | 15) + 1;

  if (fallback_buffer_pos + real_size >= fallback_buffer_end) {
    return NULL;
  }

  void* result = fallback_buffer_pos;
  ((size_t*) result)[-1] = real_size - 16;
  fallback_buffer_pos += real_size;

  return result;
}

static malloc_func_t* malloc_for_fallback = fallback_malloc;
#else
static malloc_func_t* fallback_malloc = NULL;
static malloc_func_t* malloc_for_fallback = MALLOC_REPLACEMENT;
#endif

#if !defined(FREE_REPLACEMENT)
static void  fallback_free(void* ptr) {
  if (((char*) ptr < fallback_buffer) && ((char*) ptr >= fallback_buffer_end)) {
    print_error("fallback free called with wrong pointer!\n", 1);
  }
}

static free_func_t* free_for_fallback = fallback_free;
#else
static free_func_t* fallback_free = NULL;
static free_func_t* free_for_fallback = FREE_REPLACEMENT;
#endif

#if !defined(CALLOC_REPLACEMENT)
static void* fallback_calloc(size_t elems, size_t size) {
  void* result = malloc_for_fallback(elems * size);

  if (result != NULL) {
    bzero(result, elems * size);
  }

  return result;
}

static calloc_func_t* calloc_for_fallback = fallback_calloc;
#else
static calloc_func_t* fallback_calloc = NULL;
static calloc_func_t* calloc_for_fallback = CALLOC_REPLACEMENT;
#endif

#if !defined(REALLOC_REPLACEMENT)
static void* fallback_realloc(void* ptr, size_t size) {
  if (((char*) ptr < fallback_buffer) && ((char*) ptr >= fallback_buffer_end)) {
    print_error("fallback realloc called with wrong pointer!\n", 1);
  }

  void* result = fallback_malloc(size);

  if ((result != NULL) && (ptr != NULL)) {
    size_t old_size = ((size_t*) ptr)[-1];
    memcpy(result, ptr, old_size);
  }

  fallback_free(ptr);

  return result;
}

static realloc_func_t* realloc_for_fallback = fallback_realloc;
#else
static realloc_func_t* fallback_realloc = NULL;
static realloc_func_t* realloc_for_fallback = REALLOC_REPLACEMENT;
#endif


#if !defined(MEMALIGN_REPLACEMENT)
static void* fallback_memalign(size_t align, size_t size) {
  // We don't expect this to ever be called, since we assume
  // the dlsym functions would not needed aligned allocations.
  // Nevertheless you'll never know, so we implement one.
  //
  // Since this should be at most used during initialization, we don't
  // check for overflow in allocation size or other invalid arguments.
  //
  // Allocate larger and larger chunks and hope we somehow find an aligned
  // allocation.
  void* result = NULL;
  size_t alloc_size = size;

  while (true) {
    void* new_result = malloc_for_fallback(alloc_size);
    free_for_fallback(result);
    result = new_result;

    if (result == NULL) {
      return NULL;
    }

    // Check if aligned by chance.
    if ((((intptr_t) result) & (align - 1)) == 0) {
      return result;
    }

    alloc_size += 8;
  }
}

static memalign_func_t* memalign_for_fallback = fallback_memalign;
#else
static memalign_func_t* fallback_memalign = NULL;
static memalign_func_t* memalign_for_fallback = MEMALIGN_REPLACEMENT;
#endif

#if !defined(POSIX_MEMALIGN_REPLACEMENT)
static int fallback_posix_memalign(void** ptr, size_t align, size_t size) {
  *ptr = memalign_for_fallback(align, size);

  if (*ptr == NULL) {
    return ENOMEM;
  }

  return 0;
}

static posix_memalign_func_t* posix_memalign_for_fallback = fallback_posix_memalign;
#else
static posix_memalign_func_t* fallback_posix_memalign = NULL;
static posix_memalign_func_t* posix_memalign_for_fallback = POSIX_MEMALIGN_REPLACEMENT;
#endif

#if !defined(ALIGNED_ALLOC_REPLACEMENT)
static void* fallback_aligned_alloc(size_t align, size_t size) {
  if ((align == 0) || ((size & align) != 0)) {
    errno = EINVAL;
    return NULL;
  }

  return memalign(align, size);
}

static aligned_alloc_func_t* aligned_alloc_for_fallback = fallback_aligned_alloc;
#else
static aligned_alloc_func_t* fallback_aligned_alloc = NULL;
#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
#endif
static aligned_alloc_func_t* aligned_alloc_for_fallback = ALIGNED_ALLOC_REPLACEMENT;
#if defined(__APPLE__)
#pragma clang diagnostic pop
#endif
#endif

#if !defined(VALLOC_REPLACEMENT) || !defined(PVALLOC_REPLACEMENT)
static size_t page_size = 0;
#endif

#if !defined(VALLOC_REPLACEMENT)
static void* fallback_valloc(size_t size) {
  if (page_size == 0) {
    page_size = (size_t) getpagesize();
  }

  return fallback_memalign(page_size, size);
}

static valloc_func_t* valloc_for_fallback = fallback_valloc;
#else
static valloc_func_t* fallback_valloc = NULL;
static valloc_func_t* valloc_for_fallback = VALLOC_REPLACEMENT;
#endif

#if !defined(PVALLOC_REPLACEMENT)
static void* fallback_pvalloc(size_t size) {
  if (page_size == 0) {
    page_size = (size_t) getpagesize();
  }

  size_t real_size = 1 + ((size - 1) | page_size);
  return fallback_memalign(page_size, real_size);
}

static pvalloc_func_t* pvalloc_for_fallback = fallback_pvalloc;
#else
static pvalloc_func_t* fallback_pvalloc = NULL;
static pvalloc_func_t* pvalloc_for_fallback = PVALLOC_REPLACEMENT;
#endif

static size_t get_allocated_size(void* ptr) {
  if (ptr == NULL) {
    return 0;
  }

#if !defined(MALLOC_REPLACEMENT)
  // We always know for fallback methods.
  if (((char*) ptr >= fallback_buffer) && ((char*) ptr < fallback_buffer_end)) {
    return ((size_t*) ptr)[-1];
  }
#endif

#if defined(__GLIBC__)
  return ((size_t*) ptr)[-1] & ~((size_t) 15);
#elif defined(__APPLE__)
  return malloc_size(ptr);
#elif defined(__AIX__)
  return 0; // We don't know
#elif defined (__THIS_IS_MUSL__)
  return malloc_usable_size(ptr);
#endif
}

#ifndef REPLACE_NAME
#define REPLACE_NAME(x) x
#endif

#if TEST_LEVEL > 1
static void* g1, *g2, *g3, *ag1, *ag2, *ag3;
static void assign_function_test(char const* symbol);
static void assign_function_post_test();
#else
#define assign_function_test(symbol)
#define assign_function_post_test()
#endif

#if TEST_LEVEL > 2
static void allow_aligned_malloc_in_test();
#else
#define allow_aligned_malloc_in_test()
#endif

static void assign_function(void** dest, char const* symbol) {
  assign_function_test(symbol);

  print("Resolving '");
  print(symbol);
  print("'\n");

  *dest = dlsym(RTLD_NEXT, symbol);

  if (*dest == NULL) {
    print_error(symbol, 0);
    print_error(" not found!\n", 1);
  }

  print("Found at ");
  print_ptr(*dest);
  print("\n");
}

#if !defined(_AIX)

#define LIB_INIT __attribute__((constructor))
#define EXPORT __attribute__((visibility("default")))

#else

#define LIB_INIT
#define EXPORT

#endif

static void LIB_INIT init(void) {
#if !defined(MALLOC_REPLACEMENT) || !defined(REALLOC_REPLACEMENT) || !defined(FREE_REPLACEMENT) || !defined(MEMALIGN_REPLACEMENT) || !defined(ALIGNED_ALLOC_REPLACEMENT)
  void* real_malloc;
  void* real_realloc;
  void* real_free;
  void* real_memalign;
  void* real_aligned_alloc;

  // malloc/realloc/free are the most important ones, so we get them
  // first. We cannot run with only a part being the real ones and a
  // part being fallback, so assign them in one go.

  assign_function(&real_malloc, "malloc");
  assign_function(&real_realloc, "realloc");
  assign_function(&real_free, "free");

  // Resolved memalign and aligned_alloc at the same time, since on
  // MUSL at least memalign just forwards to aligned_alloc.
  assign_function(&real_memalign, "memalign");
  assign_function(&real_aligned_alloc, "aligned_alloc");

  malloc_for_fallback = (malloc_func_t*) real_malloc;
  realloc_for_fallback = (realloc_func_t*) real_realloc;
  free_for_fallback = (free_func_t*) real_free;
  memalign_for_fallback = (memalign_func_t*) real_memalign;
  aligned_alloc_for_fallback = (aligned_alloc_func_t*) real_aligned_alloc;

  allow_aligned_malloc_in_test();

#if !defined(MALLOC_REPLACEMENT)
  print_size(fallback_buffer_pos - fallback_buffer);
  print(" bytes used for fallback\n");
  print_size(fallback_buffer_end - fallback_buffer_pos);
  print(" bytes not used for fallback\n");
#endif

#endif

  // Now the rest can be assigned, since even the fallack methods are not
  // too bad.
#if !defined(CALLOC_REPLACEMENT)
  assign_function((void**) &calloc_for_fallback, "calloc");
#endif
#if !defined(POSIX_MEMALIGN_REPLACEMENT)
  assign_function((void**) &posix_memalign_for_fallback, "posix_memalign");
#endif
#if !defined(VALLOC_REPLACEMENT)
  assign_function((void**) &valloc_for_fallback, "valloc");
#endif
#if !defined(PVALLOC_REPLACEMENT)
  assign_function((void**) &pvalloc_for_fallback, "pvalloc");
#endif

  assign_function_post_test();
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

static volatile registered_hooks_t* registered_hooks = &empty_registered_hooks;

static real_funcs_t real_funcs;

EXPORT real_funcs_t* register_hooks(registered_hooks_t* hooks) {
  if (hooks == NULL) {
    print("Deregistered hooks\n");
    registered_hooks = &empty_registered_hooks;
  } else {
    print("Registered hooks\n");
    registered_hooks = hooks;
  }

  real_funcs.malloc = malloc_for_fallback;
  real_funcs.calloc = calloc_for_fallback;
  real_funcs.realloc = realloc_for_fallback;
  real_funcs.free = free_for_fallback;
  real_funcs.posix_memalign = posix_memalign_for_fallback;
  real_funcs.memalign = memalign_for_fallback;
  real_funcs.aligned_alloc = aligned_alloc_for_fallback;
  real_funcs.valloc = valloc_for_fallback;
  real_funcs.pvalloc = pvalloc_for_fallback;
  real_funcs.malloc_size = get_allocated_size;

  return &real_funcs;
}



#define LOG_FUNC(func) \
  print(#func); \
  if (func##_for_fallback == fallback_##func) { \
    print(" (fallback)"); \
  }

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
  print(tmp_hook ? " with hook\n" : " without hook\n");

EXPORT void* REPLACE_NAME(malloc)(size_t size) {
  malloc_hook_t* tmp_hook = registered_hooks->malloc_hook;
  void* result;

  LOG_FUNC(malloc);
  LOG_SIZE(size);

  if (tmp_hook != NULL) {
    result = tmp_hook(size, __builtin_return_address(0), malloc_for_fallback, get_allocated_size);
  } else {
    result = malloc_for_fallback(size);
  }

  LOG_ALLOCATION_RESULT(result);
  LOG_HOOK;

  return result;
}

EXPORT void* REPLACE_NAME(calloc)(size_t elems, size_t size) {
  calloc_hook_t* tmp_hook = registered_hooks->calloc_hook;
  void* result;

  LOG_FUNC(calloc);
  LOG_ELEMS(elems);
  LOG_SIZE(size);

  if (tmp_hook != NULL) {
    result = tmp_hook(elems, size, __builtin_return_address(0), calloc_for_fallback, get_allocated_size);
  } else {
    result = calloc_for_fallback(elems, size);
  }

  LOG_ALLOCATION_RESULT(result);
  LOG_HOOK;

  return result;
}

EXPORT void* REPLACE_NAME(realloc)(void* ptr, size_t size) {
  realloc_hook_t* tmp_hook = registered_hooks->realloc_hook;
  void* result;

  LOG_FUNC(realloc);
  LOG_PTR_WITH_SIZE(ptr);
  LOG_SIZE(size);

#if !defined(MALLOC_REPLACEMENT)
  // We might see remnants of the fallback allocations here.
  if (((char*) ptr >= fallback_buffer) && ((char*) ptr < fallback_buffer_end)) {
    result = fallback_malloc(size);

    if (result != NULL) {
      size_t max_to_copy = fallback_buffer_end - (char*) ptr;
      memcpy(result, ptr, max_to_copy > size ? size : max_to_copy);
    }
  } else
#endif
  if (tmp_hook != NULL) {
    result = tmp_hook(ptr, size, __builtin_return_address(0), realloc_for_fallback, get_allocated_size);
  } else {
    result = realloc_for_fallback(ptr, size);
  }

  LOG_ALLOCATION_RESULT(result);
  LOG_HOOK;

  return result;
}

EXPORT void REPLACE_NAME(free)(void* ptr) {
  free_hook_t* tmp_hook = registered_hooks->free_hook;

  LOG_FUNC(free);
  LOG_PTR_WITH_SIZE(ptr);

#if !defined(MALLOC_REPLACEMENT)
  // We might see remnants of the fallback allocations here.
  if ((ptr >= (void*) fallback_buffer) && (ptr < (void*) fallback_buffer_end)) {
    // Nothinng to do
  } else
#endif
  if (tmp_hook != NULL) {
    tmp_hook(ptr, __builtin_return_address(0), free_for_fallback, get_allocated_size);
  } else {
    free_for_fallback(ptr);
  }

  LOG_HOOK;
}

EXPORT int REPLACE_NAME(posix_memalign)(void** ptr, size_t align, size_t size) {
  posix_memalign_hook_t* tmp_hook = registered_hooks->posix_memalign_hook;
  int result;

  LOG_FUNC(posix_memalign);
  LOG_ALIGN(align);
  LOG_SIZE(size);

  if (tmp_hook != NULL) {
    result = tmp_hook(ptr, align, size, __builtin_return_address(0), posix_memalign_for_fallback, get_allocated_size);
  } else {
    result = posix_memalign_for_fallback(ptr, align, size);
  }

  LOG_ALLOCATION_RESULT(*ptr);
  LOG_RESULT(result);
  LOG_HOOK;

  return result;
}

#if !defined(__APPLE__)
EXPORT void* REPLACE_NAME(memalign)(size_t align, size_t size) {
  memalign_hook_t* tmp_hook = registered_hooks->memalign_hook;
  void* result;

  LOG_FUNC(memalign);
  LOG_ALIGN(align);
  LOG_SIZE(size);

  if (tmp_hook != NULL) {
    result = tmp_hook(align, size, __builtin_return_address(0), memalign_for_fallback, get_allocated_size);
  } else {
    result = memalign_for_fallback(align, size);
  }

  LOG_ALLOCATION_RESULT(result);
  LOG_HOOK;

  return result;
}
#endif

EXPORT void* REPLACE_NAME(aligned_alloc)(size_t align, size_t size) {
  memalign_hook_t* tmp_hook = registered_hooks->aligned_alloc_hook;
  void* result;

  LOG_FUNC(aligned_alloc);
  LOG_ALIGN(align);
  LOG_SIZE(size);

  if (tmp_hook != NULL) {
    result = tmp_hook(align, size, __builtin_return_address(0), aligned_alloc_for_fallback, get_allocated_size);
  } else {
    result = aligned_alloc_for_fallback(align, size);
  }

  LOG_ALLOCATION_RESULT(result);
  LOG_HOOK;

  return result;
}

#if !defined(__THIS_IS_MUSL__)
EXPORT void* REPLACE_NAME(valloc)(size_t size) {
  valloc_hook_t* tmp_hook = registered_hooks->valloc_hook;
  void* result;

  LOG_FUNC(valloc);
  LOG_SIZE(size);

  if (tmp_hook != NULL) {
    result = tmp_hook(size, __builtin_return_address(0), valloc_for_fallback, get_allocated_size);
  } else {
    result = valloc_for_fallback(size);
  }

  LOG_ALLOCATION_RESULT(result);
  LOG_HOOK;

  return result;
}
#endif

#if !defined(__THIS_IS_MUSL__)
EXPORT void* REPLACE_NAME(pvalloc)(size_t size) {
  pvalloc_hook_t* tmp_hook = registered_hooks->pvalloc_hook;
  void* result;

  LOG_FUNC(pvalloc);
  LOG_SIZE(size);

  if (tmp_hook != NULL) {
    result = tmp_hook(size, __builtin_return_address(0), pvalloc_for_fallback, get_allocated_size);
  } else {
    result = pvalloc_for_fallback(size);
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


#if TEST_LEVEL > 0

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


#if TEST_LEVEL > 2

static void* memalign_for_test(size_t align, size_t size) {
#if defined(__APPLE__)
  // No memalign on MacOSX
  void* result;

  if (REPLACE_NAME(posix_memalign)(&result, align, size) == 0) {
    return result;
  }

  return NULL;
#else
  return REPLACE_NAME(memalign)(align, size);
#endif
}

static void* valloc_for_test(size_t size) {
#if defined(__THIS_IS_MUSL__)
  // musl has no valloc
  return REPLACE_NAME(memalign)(getpagesize(), size);
#else
  return REPLACE_NAME(valloc)(size);
#endif
}

static void* pvalloc_for_test(size_t size) {
#if defined(__THIS_IS_MUSL__)
  // musl has no pvalloc
  return REPLACE_NAME(memalign)(getpagesize(), size);
#else
  return REPLACE_NAME(pvalloc)(size);
#endif
}

static bool allow_aligned_malloc;

static void allow_aligned_malloc_in_test() {
  allow_aligned_malloc = true;
}

#endif

#if TEST_LEVEL > 1
static void assign_function_test(char const* symbol) {
  // Simulate having to malloc for dlsym. Only use the most likely called
  // allocation methods at this level, so no aligned allocations.
  void* p1 = REPLACE_NAME(malloc)(strlen(symbol) * 2 + 1024);
  void* p2 = REPLACE_NAME(calloc)(4, 256);
  char* p3 = strdup(symbol);
  p1 = REPLACE_NAME(realloc)(p1, 2048);
  REPLACE_NAME(free)(p2);
  REPLACE_NAME(free)(p3);
  REPLACE_NAME(free)(p1);

  if (g1 == NULL) {
    g1 = REPLACE_NAME(malloc)(12);
  } if (g2 == NULL) {
    g2 = REPLACE_NAME(malloc)(13);
  } else if (g3 == NULL) {
    g3 = REPLACE_NAME(malloc)(14);
  }

  // For higher test levels test the aligned allocations too.
#if TEST_LEVEL > 2
  if (allow_aligned_malloc) {
    REPLACE_NAME(posix_memalign)(&p1, 64, 256);
    p2 = memalign_for_test(32, 128);
    p3 = valloc_for_test(7);
    p1 = REPLACE_NAME(realloc)(p1, 2048);
    REPLACE_NAME(free)(p2);
    REPLACE_NAME(free)(p3);
    REPLACE_NAME(free)(p1);

    p1 = pvalloc_for_test(127);
    REPLACE_NAME(free)(p1);

    if (ag1 == NULL) {
      REPLACE_NAME(posix_memalign)(&ag1, 64, 1024);
    } else if (ag2 == NULL) {
      ag2 = valloc_for_test(16365);
    } else if (ag3 == NULL) {
      ag3 = memalign_for_test(128, 512);
    }
  }
#endif
}

void assign_function_post_test() {
  g1 = REPLACE_NAME(realloc)(g1, 128);
  g1 = REPLACE_NAME(realloc)(g1, 0);
  REPLACE_NAME(free)(g1);
  REPLACE_NAME(free)(g2);
  REPLACE_NAME(free)(g3);
  void* p = NULL;
  REPLACE_NAME(posix_memalign)(&p, 12, 204);
  REPLACE_NAME(free)(p);
  REPLACE_NAME(posix_memalign)(&p, 2048, 7);
  p = REPLACE_NAME(realloc)(p, 19);
  REPLACE_NAME(free)(p);

#if !defined(__APPLE__)
  p = REPLACE_NAME(memalign)(12, 204);
  REPLACE_NAME(free)(p);
  p = REPLACE_NAME(memalign)(2048, 7);
  p = REPLACE_NAME(realloc)(p, 19);
  REPLACE_NAME(free)(p);
#endif

#if !defined(__THIS_IS_MUSL__)
  p = REPLACE_NAME(valloc)(7);
  p = REPLACE_NAME(realloc)(p, 19);
  REPLACE_NAME(free)(p);
#endif

#if TEST_LEVEL > 2
  ag1 = REPLACE_NAME(realloc)(ag1, 128);
  ag1 = REPLACE_NAME(realloc)(ag1, 0);
  REPLACE_NAME(free)(ag1);
  REPLACE_NAME(free)(ag2);
  REPLACE_NAME(free)(ag3);
#endif

  // Do a large allocation which might be mmap'ed by the allocator
  void* large = REPLACE_NAME(malloc)(1024 * 1024);
  REPLACE_NAME(free)(large);
}

#endif

