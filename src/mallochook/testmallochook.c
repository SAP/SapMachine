#define _GNU_SOURCE

#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "mallochook.h"

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

static void* my_malloc_hook(size_t size, void* caller_address, real_funcs_t* real_funcs) {
	print("caller address 0x");
	print_address(caller_address);
	print("\n");
	if (size == 3) return NULL;
	return real_funcs->real_malloc(size);
}

static void* my_calloc_hook(size_t elems, size_t size, void* caller_address, real_funcs_t* real_funcs) {
	print("caller address 0x");
	print_address(caller_address);
	print("\n");
	if (size == 3) return NULL;
	return real_funcs->real_calloc(elems, size);
}

static void* my_realloc_hook(void* ptr, size_t size, void* caller_address, real_funcs_t* real_funcs) {
	print("caller address 0x");
	print_address(caller_address);
	print("\n");
	if (size == 3) return NULL;
	return real_funcs->real_realloc(ptr, size);
}

static void my_free_hook(void* ptr, void* caller_address, real_funcs_t* real_funcs) {
	print("caller address 0x");
	print_address(caller_address);
	print("\n");
	real_funcs->real_free(ptr);
}

int main(int argc, char** argv) {
	registered_hooks_t hooks = {
		my_malloc_hook,
		my_calloc_hook,
		my_realloc_hook,
		my_free_hook
	};
	
	register_hooks_t* register_hooks = dlsym((void*) RTLD_DEFAULT, REGISTER_HOOKS_NAME);

	for (int i = 0; i < 3; ++i) {
		void* p1 = malloc(1);
		void* p2 = malloc(10000);
		void* p3 = malloc(0);
		void* p4 = malloc(3);
		void* p5 = calloc(10, 1);
		void* p6 = calloc(2, 10000);
		void* p7 = calloc(0, 12);
		void* p8 = calloc(3, 3);
		void* p9 = strdup("test");
		p1 = realloc(p1, 4);
		p2 = realloc(p2, 0);
		p3 = realloc(p3, 0);
		p4 = realloc(p4, 10);
		p5 = reallocarray(p5, 1, 4);
		p6 = reallocarray(p6, 2, 0);
		p7 = reallocarray(p7, 3, 0);
		p8 = reallocarray(p8, 4, 10);
		p9 = realloc(p9, 10);
		free(p1);
		free(p2);
		free(p3);
		free(p4);
		free(p5);
		free(p6);
		free(p7);
		free(p8);
		free(p9);

		if (i == 0) {
			print("Registered\n");
			if (register_hooks) register_hooks(&hooks);
		} else if (i == 1) {
			print("Deregistered\n");
			if (register_hooks) register_hooks(NULL);
		}
	}
}
