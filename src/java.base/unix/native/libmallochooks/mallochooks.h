/*
 * Copyright (c) 2023 SAP SE. All rights reserved.
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

#ifndef __SAPMACHINE_MALLOC_HOOK
#define __SAPMACHINE_MALLOC_HOOK

typedef void* malloc_func_t(size_t size);
typedef void* calloc_func_t(size_t elems, size_t size);
typedef void* realloc_func_t(void* ptr, size_t size);
typedef void  free_func_t(void* ptr);
typedef int   posix_memalign_func_t(void** ptr, size_t align, size_t size);
typedef void* memalign_func_t(size_t align, size_t size);
typedef void* aligned_alloc_func_t(size_t align, size_t size);
typedef void* valloc_func_t(size_t size);
typedef void* pvalloc_func_t(size_t size);

typedef size_t malloc_size_func_t(void* ptr);
typedef void*  malloc_hook_t(size_t size, void* caller);
typedef void*  calloc_hook_t(size_t elems, size_t size, void* caller);
typedef void*  realloc_hook_t(void* ptr, size_t size, void* caller);
typedef void   free_hook_t(void* ptr, void* caller);
typedef int    posix_memalign_hook_t(void** ptr, size_t align, size_t size, void* caller);
typedef void*  memalign_hook_t(size_t align, size_t size, void* caller);
typedef void*  aligned_alloc_hook_t(size_t align, size_t size, void* caller);
typedef void*  valloc_hook_t(size_t size, void* caller);
typedef void*  pvalloc_hook_t(size_t size, void* caller);

typedef struct {
  malloc_hook_t* malloc_hook;
  calloc_hook_t* calloc_hook;
  realloc_hook_t* realloc_hook;
  free_hook_t* free_hook;
  posix_memalign_hook_t* posix_memalign_hook;
  memalign_hook_t* memalign_hook;
  aligned_alloc_hook_t* aligned_alloc_hook;
  valloc_hook_t* valloc_hook;
  pvalloc_hook_t* pvalloc_hook;
} registered_hooks_t;

typedef struct {
  malloc_func_t* malloc;
  calloc_func_t* calloc;
  realloc_func_t* realloc;
  free_func_t* free;
  posix_memalign_func_t* posix_memalign;
  memalign_func_t* memalign;
  aligned_alloc_func_t* aligned_alloc;
  valloc_func_t* valloc;
  pvalloc_func_t* pvalloc;
  malloc_size_func_t* malloc_size;
} real_funcs_t;

typedef registered_hooks_t* register_hooks_t(registered_hooks_t* registered_hooks);
typedef registered_hooks_t* active_hooks_t();
typedef real_funcs_t* get_real_funcs_t();

#define REGISTER_HOOKS_NAME "malloc_hooks_register_hooks"
#define ACTIVE_HOOKS_NAME "malloc_hooks_active_hooks"
#define GET_REAL_FUNCS_NAME "malloc_hooks_get_real_funcs"

#endif

