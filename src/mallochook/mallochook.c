#include <sys/types.h>
#include <stddef.h>

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

static void __attribute__((constructor)) init(void) {
	// Could be used to get to the real malloc implementation
	// via dlsym when relying on __libc_malloc and friends
	// is not feasible.
}

typedef void* real_malloc_t(size_t size);
typedef void* malloc_hook_t(size_t size, void* caller, real_malloc_t* real_malloc);
typedef void* real_calloc_t(size_t elems, size_t size);
typedef void* calloc_hook_t(size_t elems, size_t size, void* caller, real_calloc_t* real_calloc);
typedef void* real_realloc_t(void* ptr, size_t size);
typedef void* realloc_hook_t(void* ptr, size_t size, void* caller, real_realloc_t* real_realloc);
typedef void  real_free_t(void* ptr);
typedef void  free_hook_t(void* ptr, void* caller, real_free_t* real_free);

static volatile malloc_hook_t* malloc_hook = NULL;
static volatile calloc_hook_t* calloc_hook = NULL;
static volatile realloc_hook_t* realloc_hook = NULL;
static volatile free_hook_t* free_hook = NULL;

void register_new_malloc_hook(malloc_hook_t* new_malloc_hook) {
	malloc_hook = (volatile malloc_hook_t*) new_malloc_hook;
}

void register_new_calloc_hook(calloc_hook_t* new_calloc_hook) {
	calloc_hook = (volatile calloc_hook_t*) new_calloc_hook;
}

void register_new_realloc_hook(realloc_hook_t* new_realloc_hook) {
	realloc_hook = (volatile realloc_hook_t*) new_realloc_hook;
}

void register_new_free_hook(malloc_hook_t* new_free_hook) {
	free_hook = (volatile free_hook_t*) new_free_hook;
}

void* malloc(size_t size) {
	malloc_hook_t* tmp_hook = malloc_hook;
	void* result;

	if (tmp_hook != NULL) {
		result = tmp_hook(size, __builtin_return_address(0), __libc_malloc);
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
	calloc_hook_t* tmp_hook = calloc_hook;
	void* result;

	if (tmp_hook != NULL) {
		result = tmp_hook(elems, size, __builtin_return_address(0), __libc_calloc);
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
	realloc_hook_t* tmp_hook = realloc_hook;
	void* result;

	if (tmp_hook != NULL) {
		result = tmp_hook(ptr, size, __builtin_return_address(0), __libc_realloc);
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
	free_hook_t* tmp_hook = free_hook;

	if (tmp_hook != NULL) {
		tmp_hook(ptr, __builtin_return_address(0), __libc_free);
	} else {
		__libc_free(ptr);
	}

	print("free of ");
	print_ptr(ptr);
	print_cr(tmp_hook ? " with hook" : " without hook");
}

