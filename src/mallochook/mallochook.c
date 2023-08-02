#include <sys/types.h>
#include <stddef.h>

#include "mallochook.h"

#define WITH_DEBUG_OUTPUT 1

#if WITH_DEBUG_OUTPUT
#include <unistd.h>
#include <string.h>

void print(char const* str) {
	write(1, str, strlen(str));
}

void print_ptr(void* ptr) {
	int shift = 32;
	print("0x");

	do {
		shift -= 1;
		write(1, "0123456789abcdef" + ((((size_t) ptr) >> shift) & 15), 1);
		
	} while (shift > 0);
}

void print_size(size_t size) {
	char buf[20];
	size_t pos = sizeof(buf);
	
	do {
		buf[--pos] = '0' + (size % 10);
		size /= 10;
	}  while (size > 0);

	write(1, buf + pos, sizeof(buf) - pos);
}

void print_cr(char const* str) {
	print(str);
	print("\n");
}
#else
#define print_ptr(x)
#define print_size(x)
#define print(x)
#define print_cr(x)
#endif

void* __libc_malloc(size_t size);
void* __libc_calloc(size_t elems, size_t size);
void* __libc_realloc(void* ptr, size_t size);
void  __libc_free(void* ptr);

static real_funcs_t real_funcs = {
	__libc_malloc,
	__libc_calloc,
	__libc_realloc,
	__libc_free
};

static void __attribute__((constructor)) init(void) {
	// Could be used to get to the real malloc implementation
	// via dlsym when relying on __libc_malloc and friends
	// is not feasible.
}

static registered_hooks_t empty_registered_hooks = {
	NULL,
	NULL,
	NULL,
	NULL
};

static volatile registered_hooks_t* registered_hooks = &empty_registered_hooks;

void register_hooks(registered_hooks_t* hooks) {
	if (hooks == NULL) {
		registered_hooks = &empty_registered_hooks;
	} else {
		registered_hooks = hooks;
	}
}

void* malloc(size_t size) {
	malloc_hook_t* tmp_hook = registered_hooks->malloc_hook;
	void* result;

	if (tmp_hook != NULL) {
		result = tmp_hook(size, __builtin_return_address(0), &real_funcs);
	} else {
		result = __libc_malloc(size);
	}

	print("malloc size ");
	print_size(size);
	print(" allocated at ");
	print_ptr(result);
	print_cr(tmp_hook ? " with hook" : " without hook");

	return result;
}

void* calloc(size_t elems, size_t size) {
	calloc_hook_t* tmp_hook = registered_hooks->calloc_hook;
	void* result;

	if (tmp_hook != NULL) {
		result = tmp_hook(elems, size, __builtin_return_address(0), &real_funcs);
	} else {
		result = __libc_calloc(elems, size);
	}

	print("calloc size ");
	print_size(elems);
	print("x");
	print_size(size);
	print(" allocated at ");
	print_ptr(result);
 	print_cr(tmp_hook ? " with hook" : " without hook");

	return result;
}

void* realloc(void* ptr, size_t size) {
	realloc_hook_t* tmp_hook = registered_hooks->realloc_hook;
	void* result;

	if (tmp_hook != NULL) {
		result = tmp_hook(ptr, size, __builtin_return_address(0), &real_funcs);
	} else {
		result = __libc_realloc(ptr, size);
	}

	print("realloc of ");
	print_ptr(ptr);
	print(" of size ");
	print_size(size);
	print(" allocated at ");
	print_ptr(result);
	print_cr(tmp_hook ? " with hook" : " without hook");

	return result;
}

void free(void* ptr) {
	free_hook_t* tmp_hook = registered_hooks->free_hook;

	if (tmp_hook != NULL) {
		tmp_hook(ptr, __builtin_return_address(0), &real_funcs);
	} else {
		__libc_free(ptr);
	}

	print("free of ");
	print_ptr(ptr);
	print_cr(tmp_hook ? " with hook" : " without hook");
}

