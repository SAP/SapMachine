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

static void __attribute__((constructor)) init(void) {
}

typedef void* real_malloc_t(size_t size);
typedef void* malloc_hook_t(size_t size, void* caller, real_malloc_t* real_malloc);
typedef void* real_calloc_t(size_t elems, size_t size);
typedef void* calloc_hook_t(size_t elems, size_t size, void* caller, real_calloc_t* real_calloc);

static volatile malloc_hook_t* malloc_hook = NULL;
static volatile calloc_hook_t* calloc_hook = NULL;

void register_new_malloc_hook(malloc_hook_t* new_malloc_hook) {
	malloc_hook = (volatile malloc_hook_t*) new_malloc_hook;
}

void register_new_calloc_hook(calloc_hook_t* new_calloc_hook) {
	calloc_hook = (volatile calloc_hook_t*) new_calloc_hook;
}

void* malloc(size_t size) {
	malloc_hook_t* tmp_hook = malloc_hook;

	if (tmp_hook != NULL) {
		void* result = tmp_hook(size, __builtin_return_address(0), __libc_malloc);
		print("malloc size ");
		print_size(size);
		print(" allocated at ");
		print_ptr(result);
		print_cr(" with hook");
		return result;
	}

	return __libc_malloc(size);
}


void* calloc(size_t elems, size_t size) {
	calloc_hook_t* tmp_hook = calloc_hook;

	if (tmp_hook != NULL) {
		void* result = tmp_hook(elems, size, __builtin_return_address(0), __libc_calloc);
		print("calloc size ");
		print_size(elems);
		print("x");
		print_size(size);
		print(" allocated at ");
		print_ptr(result);
		print_cr(" with hook");
		return result;
	}

	return __libc_malloc(size);
}
