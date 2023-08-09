#include <sys/types.h>
#include <stddef.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>

#include "mallochook.h"

// Define to get debug output to the file descriptor
#define DEBUG_FD 2

// The level of self testing during startup.
#define TEST_LEVEL 1

// Define top always use the fallback_* methods initially
#define ALLWAYS_USE_FALLBACKS

#if defined(__APPLE__)

#if !defined(ALLWAYS_USE_FALLBACKS)
#define MALLOC_REPLACEMENT         malloc
#define CALLOC_REPLACEMENT         calloc
#define REALLOC_REPLACEMENT        realloc
#define FREE_REPLACEMENT           free
#define POSIX_MEMALIGN_REPLACEMENT posix_memalign
#define MEMALIGN_REPLACEMENT       NULL
#define VALLOC_REPLACEMENT         valloc
#endif

#define REPLACE_NAME(x) x##_interpose
#define NO_SYMBOL_LOADING

#elif defined(__GLIBC__)

void* __libc_malloc(size_t size);
void* __libc_calloc(size_t elems, size_t size);
void* __libc_realloc(void* ptr, size_t size);
void  __libc_free(void* ptr);
void* __libc_memalign(size_t align, size_t size);
void* __libc_valloc(size_t size);

#if !defined(ALLWAYS_USE_FALLBACKS)
#define MALLOC_REPLACEMENT         __libc_malloc
#define CALLOC_REPLACEMENT         __libc_calloc
#define REALLOC_REPLACEMENT        __libc_realloc
#define FREE_REPLACEMENT           __libc_free
#define MEMALIGN_REPLACEMENT       __libc_memalign
#define VALLOC_REPLACEMENT         __libc_valloc
#endif

#elif defined(_AIX)

#else  // This must be musl. Since they are cool they don't set a define.

#define VALLOC_REPLACEMENT         NULL

#endif


#if defined(DEBUG_FD)

void print(char const* str) {
	write(DEBUG_FD, str, strlen(str));
}

void print_ptr(void* ptr) {
	int shift = 32;
	print("0x");

	do {
		shift -= 4;
		write(DEBUG_FD, &("0123456789abcdef"[((((size_t) ptr) >> shift) & 15)]), 1);
		
	} while (shift > 0);
}

void print_size(size_t size) {
	char buf[20];
	size_t pos = sizeof(buf);
	
	do {
		buf[--pos] = '0' + (size % 10);
		size /= 10;
	}  while (size > 0);

	write(DEBUG_FD, buf + pos, sizeof(buf) - pos);
}

#else

#define print_ptr(x)
#define print_size(x)
#define print(x)
#define print_cr(x)

#endif

#if !defined(MALLOC_REPLACEMENT)
static char  fallback_buffer[1024 * 1024];
static char* fallback_buffer_pos = fallback_buffer;
static char* fallback_buffer_end = &fallback_buffer[sizeof(fallback_buffer)];

static void* fallback_malloc(size_t size) {
	// Align to 16 byte.
	size = ((size - 1) | 15) + 1;

	if (fallback_buffer_pos + size >= fallback_buffer_end) {
		return NULL;
	}

	void* result = fallback_buffer_pos;
	fallback_buffer_pos += size;

	return result;
}

static malloc_func_t* malloc_for_fallback = fallback_malloc;
#else
static malloc_func_t* fallback_malloc = NULL;
static malloc_func_t* malloc_for_fallback = MALLOC_REPLACEMENT;
#endif

#if !defined(FREE_REPLACEMENT)
static void  fallback_free(void* ptr) {
	// Nothing to do.
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
	void* result = fallback_malloc(size);

	if (result != NULL) {
		// We don't know the original size, but we know it is from the preallocated
		// range, so it is OK to copy more.
		memcpy(result, ptr, size);
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
	// We don't expect this to ever be called, since we assume the
	// posix_memalign method of the glibc to be found during the init
	// function.
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

#if !defined(VALLOC_REPLACEMENT)
static size_t page_size = 0;

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

#if TEST_LEVEL > 0
static void* g1, *g2, *g3;

static void assign_function_test(char const* symbol) {
	// Simulate having to malloc for dlsym. Only use the most likely called
	// allocation methods at this level, so no aligned allocations.
	void* p1 = malloc(strlen(symbol) * 2 + 1024);
	void* p2 = calloc(4, 256);
	char* p3 = strdup(symbol);
	p1 = realloc(p1, 2048);
	free(p2);
	free(p3);
	free(p1);

	if (g1 == NULL) {
		g1 = malloc(12);
	} else if (g2 == NULL) {
		g2 = malloc(13);
	} else if (g3 == NULL) {
		g3 = malloc(14);
	}
}

void assign_function_post_test() {
	g1 = realloc(g1, 128);
	g1 = realloc(g1, 0);
	free(g1);
	free(g2);
	free(g3);
}
#endif

static void assign_function(void** dest, char const* symbol, bool mandatory) {
#if TEST_LEVEL > 0
	assign_function_test(symbol);
#endif

	print("Resolving '");
	print(symbol);
	print("'\n");

	*dest = dlsym(RTLD_NEXT, symbol);

	if (mandatory && (*dest == NULL)) {
		char const* not_found = " not found\n";
		write(DEBUG_FD, symbol, strlen(symbol));
		write(DEBUG_FD, not_found, strlen(not_found));
		exit(1);
	}

	print("Found at 0x");
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
#if !defined(MALLOC_REPLACEMENT) || !defined(REALLOC_REPLACEMENT) || !defined(FREE_REPLACEMENT)
	void* real_malloc;
	void* real_realloc;
	void* real_free;

	// malloc/realloc/free are the most important ones, so we get them
	// first. We cannot run with only a part being the real ones and a
	// part being fallback, so assign them in one go.

	assign_function(&real_malloc, "malloc", true);
	assign_function(&real_realloc, "realloc", true);
	assign_function(&real_free, "free", true);

	malloc_for_fallback = (malloc_func_t*) real_malloc;
	realloc_for_fallback = (realloc_func_t*) real_realloc;
	free_for_fallback = (free_func_t*) real_free;

	print_size(fallback_buffer_pos - fallback_buffer);
	print(" bytes used for fallback\n");
	print_size(fallback_buffer_end - fallback_buffer_pos);
	print(" bytes not used for fallback\n");
#endif

	// Assign memalign first, since other aligned fallback methods use it as base.
#if !defined(MEMALIGN_REPLACEMENT)
	assign_function((void**) &memalign_for_fallback, "memalign", true);
#endif

	// Now the rest can be assigned, since even the fallack methods are not
	// too bad.
#if !defined(CALLOC_REPLACEMENT)
	assign_function((void**) &calloc_for_fallback, "calloc", true);
#endif
#if !defined(POSIX_MEMALIGN_REPLACEMENT)
	assign_function((void**) &posix_memalign_for_fallback, "posix_memalign", true);
#endif
#if !defined(VALLOC_REPLACEMENT)
	assign_function((void**) &valloc_for_fallback, "valloc", false);
#endif

#if TEST_LEVEL > 0
	assign_function_post_test();
#endif
}

static registered_hooks_t empty_registered_hooks = {
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

	real_funcs.real_malloc = malloc_for_fallback;
	real_funcs.real_calloc = calloc_for_fallback;
	real_funcs.real_realloc = realloc_for_fallback;
	real_funcs.real_free = free_for_fallback;
	real_funcs.real_posix_memalign = posix_memalign_for_fallback;
	real_funcs.real_memalign = memalign_for_fallback;
	real_funcs.real_valloc = valloc_for_fallback;

	return &real_funcs;
}

#ifndef REPLACE_NAME
#define REPLACE_NAME(x) x
#endif

#define LOG_FUNC(func) \
	print(#func); \
	if (func##_for_fallback == fallback_##func) { \
		print(" (fallback)"); \
	}

#define LOG_ALIGN(align) \
	print(" alignment "); \
	print_size(align);

#define LOG_PTR(ptr) \
	print(" 0x"); \
	print_ptr(ptr);

#define LOG_ELEMS(elems) \
	print(" #elems "); \
	print_size(elems);

#define LOG_SIZE(size) \
	print(" size "); \
	print_size(size);

#define LOG_ALLOCATION_RESULT(result) \
        print(" allocated at 0x"); \
	print_ptr(result);

#define LOG_RESULT(result) \
	print(" result "); \
	print_size(result);

#define LOG_HOOK \
	print(tmp_hook ? " with hook\n" : " without hook\n");


EXPORT void* REPLACE_NAME(malloc)(size_t size) {
	malloc_hook_t* tmp_hook = registered_hooks->malloc_hook;
	void* result;

	if (tmp_hook != NULL) {
		result = tmp_hook(size, __builtin_return_address(0), malloc_for_fallback);
	} else {
		result = malloc_for_fallback(size);
	}

	LOG_FUNC(malloc);
	LOG_SIZE(size);
	LOG_ALLOCATION_RESULT(result);
	LOG_HOOK;

	return result;
}

EXPORT void* REPLACE_NAME(calloc)(size_t elems, size_t size) {
	calloc_hook_t* tmp_hook = registered_hooks->calloc_hook;
	void* result;

	if (tmp_hook != NULL) {
		result = tmp_hook(elems, size, __builtin_return_address(0), calloc_for_fallback);
	} else {
		result = calloc_for_fallback(elems, size);
	}

	LOG_FUNC(calloc);
	LOG_ELEMS(elems);
	LOG_SIZE(size);
	LOG_ALLOCATION_RESULT(result);
	LOG_HOOK;

	return result;
}

EXPORT void* REPLACE_NAME(realloc)(void* ptr, size_t size) {
	realloc_hook_t* tmp_hook = registered_hooks->realloc_hook;
	void* result;

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
		result = tmp_hook(ptr, size, __builtin_return_address(0), realloc_for_fallback);
	} else {
		result = realloc_for_fallback(ptr, size);
	}

	LOG_FUNC(realloc);
	LOG_PTR(ptr);
	LOG_SIZE(size);
	LOG_ALLOCATION_RESULT(result);
	LOG_HOOK;

	return result;
}

EXPORT void REPLACE_NAME(free)(void* ptr) {
	free_hook_t* tmp_hook = registered_hooks->free_hook;

#if !defined(MALLOC_REPLACEMENT)
	// We might see remnants of the fallback allocations here.
	if ((ptr >= (void*) fallback_buffer) && (ptr < (void*) fallback_buffer_end)) {
		// Nothinng to do
	} else
#endif
	if (tmp_hook != NULL) {
		tmp_hook(ptr, __builtin_return_address(0), free_for_fallback);
	} else {
		free_for_fallback(ptr);
	}

	LOG_FUNC(free);
	LOG_PTR(ptr);
	LOG_HOOK;
}

EXPORT int REPLACE_NAME(posix_memalign)(void** ptr, size_t align, size_t size) {
	posix_memalign_hook_t* tmp_hook = registered_hooks->posix_memalign_hook;
	int result;

	if (tmp_hook != NULL) {
		result = tmp_hook(ptr, align, size, __builtin_return_address(0), posix_memalign_for_fallback);
	} else {
		result = posix_memalign_for_fallback(ptr, align, size);
	}

	LOG_FUNC(posix_memalign);
	LOG_ALIGN(align);
	LOG_SIZE(size);
	LOG_ALLOCATION_RESULT(*ptr);
	LOG_RESULT(result);
	LOG_HOOK;

	return result;
}

EXPORT void* REPLACE_NAME(memalign)(size_t align, size_t size) {
	memalign_hook_t* tmp_hook = registered_hooks->memalign_hook;
	void* result;

	if (tmp_hook != NULL) {
		result = tmp_hook(align, size, __builtin_return_address(0), memalign_for_fallback);
	} else {
		result = memalign_for_fallback(align, size);
	}

	LOG_FUNC(memalign);
	LOG_ALIGN(align);
	LOG_SIZE(size);
	LOG_ALLOCATION_RESULT(result);
	LOG_HOOK;

	return result;
}

EXPORT void* REPLACE_NAME(valloc)(size_t size) {
	valloc_hook_t* tmp_hook = registered_hooks->valloc_hook;
	void* result;

	if (tmp_hook != NULL) {
		result = tmp_hook(size, __builtin_return_address(0), valloc_for_fallback);
	} else {
		result = valloc_for_fallback(size);
	}

	LOG_FUNC(valloc);
	LOG_SIZE(size);
	LOG_ALLOCATION_RESULT(result);
	LOG_HOOK;

	return result;
}


#if defined(__APPLE__) && !defined(ALLWAYS_USE_FALLBACKS)

#define DYLD_INTERPOSE(_replacement,_replacee) \
   __attribute__((used)) static struct{ const void* replacement; const void* replacee; } _interpose_##_replacee \
   __attribute__ ((section ("__DATA,__interpose"))) = { (const void*)(unsigned long)&_replacement, (const void*)(unsigned long)&_replacee };

DYLD_INTERPOSE(REPLACE_NAME(malloc), malloc)
DYLD_INTERPOSE(REPLACE_NAME(calloc), calloc)
DYLD_INTERPOSE(REPLACE_NAME(realloc), realloc)
DYLD_INTERPOSE(REPLACE_NAME(free), free)
DYLD_INTERPOSE(REPLACE_NAME(posix_memalign), posix_memalign)
DYLD_INTERPOSE(REPLACE_NAME(valloc), valloc)

#endif

