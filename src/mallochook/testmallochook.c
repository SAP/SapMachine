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

int main(int argc, char** argv) {
	register_new_malloc_hook_t* register_malloc_hook = dlsym((void*) RTLD_DEFAULT, "register_new_malloc_hook");
	register_new_calloc_hook_t* register_calloc_hook = dlsym((void*) RTLD_DEFAULT, "register_new_calloc_hook");

	for (int i = 0; i < 3; ++i) {
		malloc(1);
		malloc(10000);
		malloc(0);
		malloc(3);
		calloc(10, 1);
		calloc(2, 10000);
		calloc(0, 12);
		calloc(3, 3);

		if (i == 0) {
			print("Registered\n");
			register_malloc_hook(my_malloc_hook);
			register_calloc_hook(my_calloc_hook);
		} else if (i == 1) {
			print("Deregistered\n");
			register_malloc_hook(NULL);
			register_calloc_hook(NULL);
		}
	}
}
