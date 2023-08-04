#include <sys/types.h>
#include <stddef.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "mallochook.h"

#define WITH_DEBUG_OUTPUT 1
#define DEBUG_FD 2
#define ALWAYS_USE_POSIX_MEMALIGN_FALLBACK 1


#if defined(__APPLE__)

#define MALLOC_REPLACEMENT         malloc
#define CALLOC_REPLACEMENT         calloc
#define REALLOC_REPLACEMENT        realloc
#define FREE_REPLACEMENT           free
#define POSIX_MEMALIGN_REPLACEMENT posix_memalign

#define REPLACE_NAME(x) x##_interpose

#elif defined(__GLIBC__)

void* __libc_malloc(size_t size);
void* __libc_calloc(size_t elems, size_t size);
void* __libc_realloc(void* ptr, size_t size);
void  __libc_free(void* ptr);

#define MALLOC_REPLACEMENT         __libc_malloc
#define CALLOC_REPLACEMENT         __libc_calloc
#define REALLOC_REPLACEMENT        __libc_realloc
#define FREE_REPLACEMENT           __libc_free
#define POSIX_MEMALIGN_REPLACEMENT fallback_posix_memalign

#define NEEDS_FALLBACK_POSIX_MEMALIGN

#else

#define MALLOC_REPLACEMENT         fallback_malloc
#define CALLOC_REPLACEMENT         fallback_calloc
#define REALLOC_REPLACEMENT        fallback_realloc
#define FREE_REPLACEMENT           fallback_free
#define POSIX_MEMALIGN_REPLACEMENT fallback_posix_memalign

#define NEEDS_FALLBACK_MALLOC
#define NEEDS_FALLBACK_POSIX_MEMALIGN

#endif

#ifndef REPLACE_NAME
#define REPLACE_NAME(x) x
#endif

#if !defined(_AIX)

#define LIB_INIT __attribute__((constructor))
#define EXPORT __attribute__((visibility("default")))

#else

#define LIB_INIT
#define EXPORT

#endif


#if WITH_DEBUG_OUTPUT

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

#if defined(NEEDS_FALLBACK_MALLOC)

static char  fallback_buffer[1024 * 1024];
static char* fallback_buffer_pos = fallback_buffer;
static char* fallback_buffer_end = &fallback_buffer[sizeof(fallback_buffer)];

void* fallback_malloc(size_t size) {
	// Align to 16 byte.
	size = ((size - 1) | 15) + 1;

	if (fallback_buffer_pos + size >= fallback_buffer_end) {
		return NULL;
	}

	void* result = fallback_buffer_pos;
	fallback_buffer_pos += size;

	return result;
}

void  fallback_free(void* ptr) {
	// Nothing to do.
}
#endif

// The baisc malloc/free to be used in other fallback methods.
static malloc_func_t* malloc_for_fallback = MALLOC_REPLACEMENT;
static free_func_t* free_for_fallback = FREE_REPLACEMENT;

#if defined(NEEDS_FALLBACK_MALLOC)
void* fallback_calloc(size_t elems, size_t size) {
	void* result = malloc_for_fallback(elems * size);

	if (result != NULL) {
		bzero(result, elems * size);
	}

	return result;
}

void* fallback_realloc(void* ptr, size_t size) {
	void* result = fallback_malloc(size);

	if (result != NULL) {
		// We don't know the original size, but we know it is from the preallocated
		// range, so it is OK to copy more.
		memcpy(result, ptr, size);
	}

	fallback_free(ptr);

	return result;
}

#endif

#if defined(NEEDS_FALLBACK_POSIX_MEMALIGN)

int fallback_posix_memalign(void** ptr, size_t align, size_t size) {
	// We don't expect this to ever be called, since we assume the
	// posix_memalign method of the glibc to be found during the init
	// function.
	//
	// Since this should be at most used during initialization, we don't
	// check for overflow in allocation size or other invalid arguments.
	//
	// Allocate larger and larger chunks and hope we somehow find an aligned
	// allocation.
	void* raw = malloc_for_fallback(size);
	*ptr = NULL;

	if (raw == NULL) {
		return ENOMEM;
	}

	size_t alloc_size = size + align;

	while ((((intptr_t) raw) & (align - 1)) != 0) {
		void* new_raw = malloc_for_fallback(alloc_size);
		free_for_fallback(raw);
		raw = new_raw;

		if (raw == NULL) {
			return ENOMEM;
		}

		alloc_size += 8;
	}

	*ptr = raw;
	return 0;
}

#endif

static real_funcs_t real_funcs = {
	MALLOC_REPLACEMENT,
	CALLOC_REPLACEMENT,
	REALLOC_REPLACEMENT,
	FREE_REPLACEMENT,
        POSIX_MEMALIGN_REPLACEMENT
};

static void assign_function(void** dest, char const* symbol) {
	void* func = dlsym(RTLD_NEXT, symbol);

	if (func != NULL) {
		*dest = func;
	} else {
		write(DEBUG_FD, symbol, strlen(symbol));
		write(DEBUG_FD, "not found\n", 8);
		exit(1);
	}
}

static void LIB_INIT init(void) {
#if defined(NEEDS_FALLBACK_MALLOC)
	assign_function((void**) &real_funcs.real_malloc, "malloc");
	assign_function((void**) &real_funcs.real_free, "free");
	assign_function((void**) &real_funcs.real_realloc, "realloc");

	// Assign now, since it really hurts to use the fallback malloc/frees i
	// the fallback calloc/posix_memalign.
	malloc_for_fallback = real_funcs.real_malloc;
	free_for_fallback = real_funcs.real_free;

	print_size(fallback_buffer_pos - fallback_buffer);
	print(" bytes used for fallback\n");
	print_size(fallback_buffer_end - fallback_buffer_pos);
	print(" bytes not used for fallback\n");

	assign_function((void**) &real_funcs.real_calloc, "calloc");
#endif

#if defined(NEEDS_FALLBACK_POSIX_MEMALIGN)
	assign_function((void**) &real_funcs.real_posix_memalign, "posix_memalign");
#endif
}

static registered_hooks_t empty_registered_hooks = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

static volatile registered_hooks_t* registered_hooks = &empty_registered_hooks;

EXPORT real_funcs_t* register_hooks(registered_hooks_t* hooks) {
	if (hooks == NULL) {
                print("Deregistered hooks\n");
		registered_hooks = &empty_registered_hooks;
	} else {
                print("Registered hooks\n");
		registered_hooks = hooks;
	}

	return &real_funcs;
}

EXPORT void* REPLACE_NAME(malloc)(size_t size) {
	malloc_hook_t* tmp_hook = registered_hooks->malloc_hook;
	void* result;

	if (tmp_hook != NULL) {
		result = tmp_hook(size, __builtin_return_address(0), &real_funcs);
	} else {
		result = real_funcs.real_malloc(size);
	}

	print("malloc size ");
	print_size(size);
	print(" allocated at ");
	print_ptr(result);
	print(tmp_hook ? " with hook\n" : " without hook\n");

	return result;
}

EXPORT void* REPLACE_NAME(calloc)(size_t elems, size_t size) {
	calloc_hook_t* tmp_hook = registered_hooks->calloc_hook;
	void* result;

	if (tmp_hook != NULL) {
		result = tmp_hook(elems, size, __builtin_return_address(0), &real_funcs);
	} else {
		result = real_funcs.real_calloc(elems, size);
	}

	print("calloc size ");
	print_size(elems);
	print("x");
	print_size(size);
	print(" allocated at ");
	print_ptr(result);
	print(tmp_hook ? " with hook\n" : " without hook\n");

	return result;
}

EXPORT void* REPLACE_NAME(realloc)(void* ptr, size_t size) {
	realloc_hook_t* tmp_hook = registered_hooks->realloc_hook;
	void* result;

	if (tmp_hook != NULL) {
		result = tmp_hook(ptr, size, __builtin_return_address(0), &real_funcs);
	} else {
		result = real_funcs.real_realloc(ptr, size);
	}

	print("realloc of ");
	print_ptr(ptr);
	print(" of size ");
	print_size(size);
	print(" allocated at ");
	print_ptr(result);
	print(tmp_hook ? " with hook\n" : " without hook\n");

	return result;
}

EXPORT void REPLACE_NAME(free)(void* ptr) {
#if defined(NEEDS_FALLBACK_MALLOC)
	// We might see remnants of the fallback allocations here.
	if ((ptr >= (void*) fallback_buffer) && (ptr < (void*) fallback_buffer_end)) {
		return;
	}
#endif

	free_hook_t* tmp_hook = registered_hooks->free_hook;

	if (tmp_hook != NULL) {
		tmp_hook(ptr, __builtin_return_address(0), &real_funcs);
	} else {
		real_funcs.real_free(ptr);
	}

	print("free of ");
	print_ptr(ptr);
	print(tmp_hook ? " with hook\n" : " without hook\n");
}

EXPORT int REPLACE_NAME(posix_memalign)(void** ptr, size_t align, size_t size) {
	posix_memalign_hook_t* tmp_hook = registered_hooks->posix_memalign_hook;
	int result;

	if (tmp_hook != NULL) {
		result = tmp_hook(ptr, align, size, __builtin_return_address(0), &real_funcs);
	} else {
		result = real_funcs.real_posix_memalign(ptr, align, size);
	}

	print("posix_memalign with alignment ");
	print_size(align);
	print(" and size ");
	print_size(size);

	if (result == 0) {
	        print(" allocated at ");
		print_ptr(*ptr);
	} else {
		print(" failed");
	}

	print(tmp_hook ? " with hook\n" : " without hook\n");

	return result;
}

#if defined(__APPLE__)

#define DYLD_INTERPOSE(_replacement,_replacee) \
   __attribute__((used)) static struct{ const void* replacement; const void* replacee; } _interpose_##_replacee \
   __attribute__ ((section ("__DATA,__interpose"))) = { (const void*)(unsigned long)&_replacement, (const void*)(unsigned long)&_replacee };

DYLD_INTERPOSE(REPLACE_NAME(malloc), malloc)
DYLD_INTERPOSE(REPLACE_NAME(calloc), calloc)
DYLD_INTERPOSE(REPLACE_NAME(realloc), realloc)
DYLD_INTERPOSE(REPLACE_NAME(free), free)
DYLD_INTERPOSE(REPLACE_NAME(posix_memalign), posix_memalign)

#endif

