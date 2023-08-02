#ifndef __SAPMACHINE_MALLOC_HOOK
#define __SAPMACHINE_MALLOC_HOOK

typedef void* real_malloc_t(size_t size);
typedef void* real_calloc_t(size_t elems, size_t size);
typedef void* real_realloc_t(void* ptr, size_t size);
typedef void  real_free_t(void* ptr);

typedef struct {
	real_malloc_t* real_malloc;
	real_calloc_t* real_calloc;
	real_realloc_t* real_realloc;
	real_free_t* real_free;
} real_funcs_t;

typedef void* malloc_hook_t(size_t size, void* caller, real_funcs_t* real_funcs);
typedef void* calloc_hook_t(size_t elems, size_t size, void* caller, real_funcs_t* real_funcs);
typedef void* realloc_hook_t(void* ptr, size_t size, void* caller, real_funcs_t* real_funcs);
typedef void  free_hook_t(void* ptr, void* caller, real_funcs_t* real_funcs);

typedef struct {
	malloc_hook_t* malloc_hook;
	calloc_hook_t* calloc_hook;
	realloc_hook_t* realloc_hook;
	free_hook_t* free_hook;
} registered_hooks_t;

typedef void register_hooks_t(registered_hooks_t* registered_hooks);

#define REGISTER_HOOKS_NAME "register_hooks"

#endif

