#include <sys/types.h>
#include <stddef.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "mallochook.h"

#define EXPORT __attribute__((visibility("default")))


#define WITH_DEBUG_OUTPUT 1
#define DEBUG_FD 2
#define ALWAYS_USE_POSIX_MEMALIGN_FALLBACK 1

#if WITH_DEBUG_OUTPUT

#if defined(__GLIBC__)
#define USE_LIBC_FALLBACKS
#endif

void print(char const* str) {
	write(DEBUG_FD, str, strlen(str));
}

void print_ptr(void* ptr) {
	int shift = 32;
	print("0x");

	do {
		shift -= 4;
		write(DEBUG_FD, "0123456789abcdef" + ((((size_t) ptr) >> shift) & 15), 1);
		
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

#ifdef USE_LIBC_FALLBACKS

void* __libc_malloc(size_t size);
void* __libc_calloc(size_t elems, size_t size);
void* __libc_realloc(void* ptr, size_t size);
void  __libc_free(void* ptr);

static malloc_func_t* malloc_for_posix_memalign = __libc_malloc;
static free_func_t* free_for_posix_memalign = __libc_free;

#else

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

void* fallback_calloc(size_t elems, size_t size) {
	void* result = fallback_malloc(elems * size);

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

	return result;
}

void  fallback_free(void* ptr) {
	// Nothing to do.
}

static malloc_func_t* malloc_for_posix_memalign = fallback_malloc;
static free_func_t* free_for_posix_memalign = fallback_free;

#endif


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
	void* raw = malloc_for_posix_memalign(size);
	*ptr = NULL;

	if (raw == NULL) {
		return ENOMEM;
	}

	size_t alloc_size = size + align;

	while ((((intptr_t) raw) & (align - 1)) != 0) {
		void* new_raw = malloc_for_posix_memalign(alloc_size);
		free_for_posix_memalign(raw);
		raw = new_raw;

		if (raw == NULL) {
			return ENOMEM;
		}

		alloc_size += 8;
	}

	*ptr = raw;
	return 0;
}

static real_funcs_t real_funcs = {
#ifdef USE_LIBC_FALLBACKS
	__libc_malloc,
	__libc_calloc,
	__libc_realloc,
	__libc_free,
#else
	fallback_malloc,
	fallback_calloc,
	fallback_realloc,
	fallback_free,
#endif
        fallback_posix_memalign
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

static void __attribute__((constructor)) init(void) {
#ifndef USE_LIBC_FALLBACKS
	assign_function((void**) &real_funcs.real_malloc, "malloc");
	assign_function((void**) &real_funcs.real_free, "free");
	// Assign now, since it really hurts to use the fallback malloc/frees i
	// the fallback posix_memalign.
	malloc_for_posix_memalign = real_funcs.real_malloc;
	free_for_posix_memalign = real_funcs.real_free;
	assign_function((void**) &real_funcs.real_calloc, "calloc");
	assign_function((void**) &real_funcs.real_realloc, "realloc");
#endif

#if ALWAYS_USE_POSIX_MEMALIGN_FALLBACK
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

EXPORT void* malloc(size_t size) {
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

EXPORT void* calloc(size_t elems, size_t size) {
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

EXPORT void* realloc(void* ptr, size_t size) {
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

EXPORT void free(void* ptr) {
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

EXPORT int posix_memalign(void** ptr, size_t align, size_t size) {
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

