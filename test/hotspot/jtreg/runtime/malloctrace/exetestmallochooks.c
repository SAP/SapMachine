/*
 * Copyright (c) 2024 SAP SE. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#if defined(LINUX) || defined(__APPLE__)
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dlfcn.h>

#include "mallochooks.h"


static void write_string(char const* str) {
    size_t left = strlen(str);
    char const* pos = str;

    while (left > 0) {
        ssize_t result = write(1, pos, left);

        if (result <= 0) {
            break;
        }

        pos += result;
        left -= result;
    }
}

static void check(bool condition, char const* msg) {
  if (!condition) {
    write_string("Check failed: ");
    write_string(msg);
    write_string("\n");
    exit(1);
  }
}

static real_malloc_funcs_t* funcs;

static bool no_hooks_should_be_called;

static void* test_malloc_hook(size_t size, void* caller) {
    check(!no_hooks_should_be_called, "Called malloc hook when should not");
    no_hooks_should_be_called = true;

    void* result = funcs->malloc(size);
    no_hooks_should_be_called = false;

    return result;
}

static void* test_calloc_hook(size_t elems, size_t size, void* caller) {
    check(!no_hooks_should_be_called, "Called calloc hook when should not");
    no_hooks_should_be_called = true;

    void* result = funcs->calloc(elems, size);
    no_hooks_should_be_called = false;

    return result;
}

static void* test_realloc_hook(void* ptr, size_t size, void* caller) {
    check(!no_hooks_should_be_called, "Called realloc hook when should not");
    no_hooks_should_be_called = true;

    void* result = funcs->realloc(ptr, size);
    no_hooks_should_be_called = false;

    return result;
}

static void test_free_hook(void* ptr, void* caller) {
    check(!no_hooks_should_be_called, "Called free hook when should not");
    no_hooks_should_be_called = true;

    funcs->free(ptr);
    no_hooks_should_be_called = false;
}

static int test_posix_memalign_hook(void** ptr, size_t align, size_t size, void* caller) {
    check(!no_hooks_should_be_called, "Called posix_memalign hook when should not");
    no_hooks_should_be_called = true;

    int result = funcs->posix_memalign(ptr, align, size);
    no_hooks_should_be_called = false;

    return result;
}

static void* test_memalign_hook(size_t align, size_t size, void* caller) {
    check(!no_hooks_should_be_called, "Called memalign hook when should not");
    no_hooks_should_be_called = true;

    void* result = funcs->memalign(align, size);
    no_hooks_should_be_called = false;

    return result;
}

static void* test_aligned_alloc_hook(size_t align, size_t size, void* caller) {
    check(!no_hooks_should_be_called, "Called aligned_alloc hook when should not");
    no_hooks_should_be_called = true;

    void* result = funcs->aligned_alloc(align, size);
    no_hooks_should_be_called = false;

    return result;
}

static void* test_valloc_hook(size_t size, void* caller) {
    check(!no_hooks_should_be_called, "Called valloc hook when should not");
    no_hooks_should_be_called = true;

    void* result = funcs->valloc(size);
    no_hooks_should_be_called = false;

    return result;
}

static void* test_pvalloc_hook(size_t size, void* caller) {
    check(!no_hooks_should_be_called, "Called pvalloc hook when should not");
    no_hooks_should_be_called = true;

    void* result = funcs->pvalloc(size);
    no_hooks_should_be_called = false;

    return result;
}

static void test_no_recursive_calls() {
    register_hooks_t* register_hooks = (register_hooks_t*) dlsym((void*) RTLD_DEFAULT, REGISTER_HOOKS_NAME);
    check(register_hooks != NULL, "Could not get register function");
    get_real_malloc_funcs_t* get_real_malloc_funcs = (get_real_malloc_funcs_t*) 
                                                     dlsym((void*) RTLD_DEFAULT, GET_REAL_MALLOC_FUNCS_NAME);
    check(get_real_malloc_funcs != NULL, "Could not get get_real_funcs function");

    registered_hooks_t test_hooks = {
        test_malloc_hook,
        test_calloc_hook,
        test_realloc_hook,
        test_free_hook,
        test_posix_memalign_hook,
        test_memalign_hook,
        test_aligned_alloc_hook,
        test_valloc_hook,
        test_pvalloc_hook
    };

    funcs = get_real_malloc_funcs();
    register_hooks(&test_hooks);

    // Check that all the real functions do not trigger the hooks.
    void* ptr;

    write_string("Testing malloc\n");
    funcs->malloc(0);
    funcs->malloc(1);

    write_string("Testing calloc\n");
    funcs->calloc(0, 12);
    funcs->calloc(12, 0);
    funcs->calloc(12, 12);

    write_string("Testing realloc\n");
    funcs->realloc(NULL, 0);
    funcs->realloc(NULL, 12);
    funcs->realloc(funcs->malloc(12), 0);
    funcs->realloc(funcs->malloc(12), 12);

    write_string("Testing free\n");
    funcs->free(NULL);
    funcs->free(funcs->malloc(12));

    write_string("Testing posix_memalign\n");
    funcs->posix_memalign(&ptr, 1024, 0);
    funcs->posix_memalign(&ptr, 1024, 12);

    // MacOSX has no memalign and aligned_alloc.
#if !defined(__APPLE__)
    write_string("Testing memalign\n");
    funcs->memalign(1024, 0);
    funcs->memalign(1024, 12);

    write_string("Testing aligned_alloc\n");
    funcs->aligned_alloc(1024, 0);
    funcs->aligned_alloc(1024, 12);
#endif

    // Musl has no valloc function.
#if defined(__GLIBC__) || defined(__APPLE__)
    write_string("Testing valloc\n");
    funcs->valloc(0);
    funcs->valloc(12);
#endif

    // Musl and MacOSX have no pvalloc function.
#if defined(__GLIBC__)
    write_string("Testing pvalloc\n");
    funcs->pvalloc(0);
    funcs->pvalloc(12);
#endif

    write_string("Testing hooks finished \n");
    register_hooks(NULL);
}

int main(int argc, char** argv) {
  test_no_recursive_calls();
  return 0;
}

#else // defined(LINUX) || defined(__APPLE__)

int main(int argc, char** argv) {
  return 0;
}

#endif // defined(LINUX) || defined(__APPLE__)

