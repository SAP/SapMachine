#define _GNU_SOURCE

#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include "mallochooks.h"

#define PRINT_CALLER_ADDRESS 1

#if PRINT_CALLER_ADDRESS
static void print_address(void* addr) {
  int shift = 32;

  do {
    shift -= 4;
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

static void* my_malloc_hook(size_t size, void* caller_address, malloc_func_t* real_malloc, malloc_size_func_t real_malloc_size) {
  print("caller address 0x");
  print_address(caller_address);
  print("\n");
  if (size == 3) return NULL;
  return real_malloc(size);
}

static void* my_calloc_hook(size_t elems, size_t size, void* caller_address, calloc_func_t* real_calloc, malloc_size_func_t real_malloc_size) {
  print("caller address 0x");
  print_address(caller_address);
  print("\n");
  if (size == 3) return NULL;
  return real_calloc(elems, size);
}

static void* my_realloc_hook(void* ptr, size_t size, void* caller_address, realloc_func_t* real_realloc, malloc_size_func_t real_malloc_size) {
  print("caller address 0x");
  print_address(caller_address);
  print("\n");
  if (size == 3) return NULL;
  return real_realloc(ptr, size);
}

static void my_free_hook(void* ptr, void* caller_address, free_func_t* real_free, malloc_size_func_t real_malloc_size) {
  print("caller address 0x");
  print_address(caller_address);
  print("\n");
  real_free(ptr);
}

static int my_posix_memalign_hook(void** ptr, size_t align, size_t size, void* caller_address, posix_memalign_func_t* real_posix_memalign, malloc_size_func_t real_malloc_size) {
  print("caller address 0x");
  print_address(caller_address);
  print("\n");
  return real_posix_memalign(ptr, align, size);
}

static void* my_memalign_hook(size_t align, size_t size, void* caller_address, memalign_func_t* real_memalign, malloc_size_func_t real_malloc_size) {
  print("caller address 0x");
  print_address(caller_address);
  print("\n");
  return real_memalign(align, size);
}

static void* my_aligned_alloc_hook(size_t align, size_t size, void* caller_address, aligned_alloc_func_t* real_aligned_alloc, malloc_size_func_t real_malloc_size) {
  print("caller address 0x");
  print_address(caller_address);
  print("\n");
  return real_aligned_alloc(align, size);
}

static void* my_valloc_hook(size_t size, void* caller_address, valloc_func_t* real_valloc, malloc_size_func_t real_malloc_size) {
  print("caller address 0x");
  print_address(caller_address);
  print("\n");
  return real_valloc(size);
}

static void* my_pvalloc_hook(size_t size, void* caller_address, pvalloc_func_t* real_pvalloc, malloc_size_func_t real_malloc_size) {
  print("caller address 0x");
  print_address(caller_address);
  print("\n");
  return real_pvalloc(size);
}

void test_hooks(registered_hooks_t* hooks, register_hooks_t* register_hooks) {
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
    void* pa, *pb, *pc, *pd;
    posix_memalign(&pa, 4, 1028);
    posix_memalign(&pb, 32, 513);
    posix_memalign(&pc, 65536 * 4, 65536 * 27);
    posix_memalign(&pd, 65536 * 4, 0);
    void* pe = memalign(4, 1028);
    void* pf = memalign(32, 513);
    void* pg = memalign(65536 * 4, 65536 * 27);
    void* ph = memalign(65536 * 4, 0);
    void* pi = valloc(0);
    void* pj = valloc(3);
    void* pk = valloc(4097);
    void* pl = aligned_alloc(16, 31);
    void* pm = aligned_alloc(64, 128);
    void* pn = pvalloc(4096);
    void* po = pvalloc(4097);
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
    free(pa);
    free(pb);
    free(pc);
    free(pd);
    free(pe);
    free(pf);
    free(pg);
    free(ph);
    free(pi);
    free(pj);
    free(pk);
    free(pl);
    free(pm);
    free(pn);
    free(po);

    if (i == 0) {
      print("Registered\n");
      if (register_hooks) register_hooks(hooks);
    } else if (i == 1) {
      print("Deregistered\n");
      if (register_hooks) register_hooks(NULL);
    }
  }
}

int main(int argc, char** argv) {
  registered_hooks_t hooks = {
    my_malloc_hook,
    my_calloc_hook,
    my_realloc_hook,
    my_free_hook,
    my_posix_memalign_hook,
    my_memalign_hook,
    my_aligned_alloc_hook,
    my_valloc_hook,
    my_pvalloc_hook
  };

  register_hooks_t* register_hooks = dlsym((void*) RTLD_DEFAULT, REGISTER_HOOKS_NAME);
  get_real_funcs_t* get_real_funcs = dlsym((void*) RTLD_DEFAULT, GET_REAL_FUNCS_NAME);
  print("Regsiter func: ");
  print_address(register_hooks);
  print("\n");
  print("get_real_funcs func: ");
  print_address(get_real_funcs);
  print("\n");

  test_hooks(&hooks, register_hooks);

  // Remove some hooks and see if it still works
  hooks.realloc_hook = NULL;
  hooks.calloc_hook = NULL;
  test_hooks(&hooks, register_hooks);

  // Remove all hooks and see if it still works
  hooks.malloc_hook = NULL;
  hooks.free_hook = NULL;
  hooks.posix_memalign_hook = NULL;
  test_hooks(&hooks, register_hooks);
}
