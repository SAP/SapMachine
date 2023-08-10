#ifndef __SAPMACHINE_MALLOC_HOOK
#define __SAPMACHINE_MALLOC_HOOK

typedef void* malloc_func_t(size_t size);
typedef void* calloc_func_t(size_t elems, size_t size);
typedef void* realloc_func_t(void* ptr, size_t size);
typedef void  free_func_t(void* ptr);
typedef int   posix_memalign_func_t(void** ptr, size_t align, size_t size);
typedef void* memalign_func_t(size_t align, size_t size);
typedef void* valloc_func_t(size_t size);

typedef size_t malloc_size_func_t(void* ptr);
typedef void* malloc_hook_t(size_t size, void* caller, malloc_func_t* real_malloc,
                            malloc_size_func_t real_malloc_size);
typedef void* calloc_hook_t(size_t elems, size_t size, void* caller, calloc_func_t* real_calloc,
                            malloc_size_func_t real_malloc_size);
typedef void* realloc_hook_t(void* ptr, size_t size, void* caller, realloc_func_t* real_realloc,
                             malloc_size_func_t real_malloc_size);
typedef void  free_hook_t(void* ptr, void* caller, free_func_t* real_free, malloc_size_func_t real_malloc_size);
typedef int   posix_memalign_hook_t(void** ptr, size_t align, size_t size, void* caller,
                                    posix_memalign_func_t* real_posix_memalign, malloc_size_func_t real_malloc_size);
typedef void* memalign_hook_t(size_t align, size_t size, void* caller, memalign_func_t* real_memalign,
                              malloc_size_func_t real_malloc_size);
typedef void* valloc_hook_t(size_t size, void* caller, valloc_func_t* real_valloc,
                            malloc_size_func_t real_malloc_size);

typedef struct {
	malloc_hook_t* malloc_hook;
	calloc_hook_t* calloc_hook;
	realloc_hook_t* realloc_hook;
	free_hook_t* free_hook;
	posix_memalign_hook_t* posix_memalign_hook;
	memalign_hook_t* memalign_hook;
	valloc_hook_t* valloc_hook;
} registered_hooks_t;

typedef struct {
	malloc_func_t* real_malloc;
	calloc_func_t* real_calloc;
	realloc_func_t* real_realloc;
	free_func_t* real_free;
        posix_memalign_func_t* real_posix_memalign;
	memalign_func_t* real_memalign;
	valloc_func_t* real_valloc;
	malloc_size_func_t* real_malloc_size;
} real_funcs_t;

typedef real_funcs_t* register_hooks_t(registered_hooks_t* registered_hooks);

#define REGISTER_HOOKS_NAME "register_hooks"

#endif

