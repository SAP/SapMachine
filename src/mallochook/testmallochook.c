#define _GNU_SOURCE

#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef void* real_malloc_t(size_t size);
typedef void* malloc_hook_t(size_t size, void* caller_address, real_malloc_t* real_malloc);
typedef void register_new_malloc_hook_t(malloc_hook_t* new_malloc_hook);
typedef void* real_calloc_t(size_t elems, size_t size);
typedef void* calloc_hook_t(size_t elems, size_t size, void* caller_address, real_calloc_t* real_calloc);
typedef void register_new_calloc_hook_t(calloc_hook_t* new_calloc_hook);
typedef void* real_realloc_t(void* ptr, size_t size);
typedef void* realloc_hook_t(void* ptr, size_t size, void* caller_address, real_realloc_t* real_realloc);
typedef void register_new_realloc_hook_t(realloc_hook_t* new_realloc_hook);

#define PRINT_CALLER_ADDRESS 0

#if PRINT_CALLER_ADDRESS
static void print_address(void* addr) {
	int shift = 32;

	do {
		shift -= 1;
		write(1, "0123456789abcdef" + ((((size_t) addr) >> shift) & 15), 1);
		
	} while (shift > 0);
}

static void print(char const* str) {
	write(1, str, strlen(str));
}
#else
#define print_address(x);
#define print(x)
#endif

static void* my_malloc_hook(size_t size, void* caller_address, real_malloc_t* real_malloc) {
	print("caller address 0x");
	print_address(caller_address);
	print("\n");
	if (size == 3) return NULL;
	return real_malloc(size);
}

static void* my_calloc_hook(size_t elems, size_t size, void* caller_address, real_calloc_t* real_calloc) {
	print("caller address 0x");
	print_address(caller_address);
	print("\n");
	if (size == 3) return NULL;
	return real_calloc(elems, size);
}

static void* my_realloc_hook(void* ptr, size_t size, void* caller_address, real_realloc_t* real_realloc) {
	print("caller address 0x");
	print_address(caller_address);
	print("\n");
	if (size == 3) return NULL;
	return real_realloc(ptr, size);
}

int main(int argc, char** argv) {
	register_new_malloc_hook_t* register_malloc_hook = dlsym((void*) RTLD_DEFAULT, "register_new_malloc_hook");
	register_new_calloc_hook_t* register_calloc_hook = dlsym((void*) RTLD_DEFAULT, "register_new_calloc_hook");
	register_new_realloc_hook_t* register_realloc_hook = dlsym((void*) RTLD_DEFAULT, "register_new_realloc_hook");

	for (int i = 0; i < 3; ++i) {
		void* p1 = malloc(1);
		void* p2 = malloc(10000);
		void* p3 = malloc(0);
		void* p4 = malloc(3);
		void* p5 = calloc(10, 1);
		void* p6 = calloc(2, 10000);
		void* p7 = calloc(0, 12);
		void* p8 = calloc(3, 3);
		p1 = realloc(p1, 4);
		p2 = realloc(p2, 0);
		p3 = realloc(p3, 0);
	  p4 = realloc(p4, 10);
		p5 = realloc(p5, 4);
		p6 = realloc(p6, 0);
		p7 = realloc(p7, 0);
	  p8 = realloc(p8, 10);

		if (i == 0) {
			print("Registered\n");
			if (register_malloc_hook) register_malloc_hook(my_malloc_hook);
			if (register_calloc_hook) register_calloc_hook(my_calloc_hook);
			if (register_realloc_hook) register_realloc_hook(my_realloc_hook);
		} else if (i == 1) {
			print("Deregistered\n");
			if (register_malloc_hook) register_malloc_hook(NULL);
			if (register_calloc_hook) register_calloc_hook(NULL);
			if (register_realloc_hook) register_realloc_hook(NULL);
		}
	}
}
