#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dlfcn.h>

#include "jni.h"

#include "mallochooks.h"


static jboolean no_hooks_should_be_called;
static char const* last_error;
static pthread_t main_thread;

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

static void assert_with_text(jboolean condition, char const* str) {
    if (!condition) {
        write_string("Assertion failed: ");
        write_string(str);
        write_string("\n");
        last_error = str;
    }
}

static void* test_malloc_hook(size_t size, void* caller, malloc_func_t* real_malloc,
                              malloc_size_func_t real_malloc_size) {
    if (!pthread_equal(pthread_self(), main_thread)) {
        return real_malloc(size);
    }

    assert_with_text(!no_hooks_should_be_called, "Called malloc hook when should not");
    no_hooks_should_be_called = JNI_TRUE;

    void* result = real_malloc(size);
    no_hooks_should_be_called = JNI_FALSE;

    return result;
}

void* test_calloc_hook(size_t elems, size_t size, void* caller, calloc_func_t* real_calloc,
                       malloc_size_func_t real_malloc_size) {
    if (!pthread_equal(pthread_self(), main_thread)) {
        return real_calloc(elems, size);
    }

    assert_with_text(!no_hooks_should_be_called, "Called calloc hook when should not");
    no_hooks_should_be_called = JNI_TRUE;

    void* result = real_calloc(elems, size);
    no_hooks_should_be_called = JNI_FALSE;

    return result;
}

void* test_realloc_hook(void* ptr, size_t size, void* caller, realloc_func_t* real_realloc,
                        malloc_size_func_t real_malloc_size) {
    if (!pthread_equal(pthread_self(), main_thread)) {
        return real_realloc(ptr, size);
    }

    assert_with_text(!no_hooks_should_be_called, "Called realloc hook when should not");
    no_hooks_should_be_called = JNI_TRUE;

    void* result = real_realloc(ptr, size);
    no_hooks_should_be_called = JNI_FALSE;

    return result;
}

void test_free_hook(void* ptr, void* caller, free_func_t* real_free, malloc_size_func_t real_malloc_size) {
    if (!pthread_equal(pthread_self(), main_thread)) {
        real_free(ptr);

        return;
    }

    assert_with_text(!no_hooks_should_be_called, "Called free hook when should not");
    no_hooks_should_be_called = JNI_TRUE;

    real_free(ptr);
    no_hooks_should_be_called = JNI_FALSE;
}

int test_posix_memalign_hook(void** ptr, size_t align, size_t size, void* caller,
                             posix_memalign_func_t* real_posix_memalign, malloc_size_func_t real_malloc_size) {
    if (!pthread_equal(pthread_self(), main_thread)) {
        return real_posix_memalign(ptr, align, size);
    }

    assert_with_text(!no_hooks_should_be_called, "Called posix_memalign hook when should not");
    no_hooks_should_be_called = JNI_TRUE;

    int result = real_posix_memalign(ptr, align, size);
    no_hooks_should_be_called = JNI_FALSE;

    return result;
}

void* test_memalign_hook(size_t align, size_t size, void* caller, memalign_func_t* real_memalign,
                         malloc_size_func_t real_malloc_size) {
    if (!pthread_equal(pthread_self(), main_thread)) {
        return real_memalign(align, size);
    }

    assert_with_text(!no_hooks_should_be_called, "Called memalign hook when should not");
    no_hooks_should_be_called = JNI_TRUE;

    void* result = real_memalign(align, size);
    no_hooks_should_be_called = JNI_FALSE;

    return result;
}

void* test_aligned_alloc_hook(size_t align, size_t size, void* caller, aligned_alloc_func_t* real_aligned_alloc,
                              malloc_size_func_t real_malloc_size) {
    if (!pthread_equal(pthread_self(), main_thread)) {
        return real_aligned_alloc(align, size);
    }

    assert_with_text(!no_hooks_should_be_called, "Called aligned_alloc hook when should not");
    no_hooks_should_be_called = JNI_TRUE;

    void* result = real_aligned_alloc(align, size);
    no_hooks_should_be_called = JNI_FALSE;

    return result;
}

void* test_valloc_hook(size_t size, void* caller, valloc_func_t* real_valloc,
                       malloc_size_func_t real_malloc_size) {
    if (!pthread_equal(pthread_self(), main_thread)) {
        return real_valloc(size);
    }

    assert_with_text(!no_hooks_should_be_called, "Called valloc hook when should not");
    no_hooks_should_be_called = JNI_TRUE;

    void* result = real_valloc(size);
    no_hooks_should_be_called = JNI_FALSE;

    return result;
}

void* test_pvalloc_hook(size_t size, void* caller, pvalloc_func_t* real_pvalloc,
                        malloc_size_func_t real_malloc_size) {
    if (!pthread_equal(pthread_self(), main_thread)) {
        return real_pvalloc(size);
    }

    assert_with_text(!no_hooks_should_be_called, "Called pvalloc hook when should not");
    no_hooks_should_be_called = JNI_TRUE;

    void* result = real_pvalloc(size);
    no_hooks_should_be_called = JNI_FALSE;

    return result;
}

JNIEXPORT jboolean JNICALL
Java_MallocHooksTest_hasActiveHooks(JNIEnv *env, jclass cl) {
#if defined(LINUX) || defined(__APPLE__)
    active_hooks_t* active_hooks_func = (active_hooks_t*) dlsym((void*) RTLD_DEFAULT, ACTIVE_HOOKS_NAME);

    if ((active_hooks_func != NULL) && (active_hooks_func() != NULL)) {
        return JNI_TRUE;
    }
#endif

    return JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_MallocHooksTest_testNoRecursiveCalls(JNIEnv *env, jclass cl) {
    last_error = NULL;

#if defined(LINUX) || defined(__APPLE__)
    register_hooks_t* register_func = (register_hooks_t*) dlsym((void*) RTLD_DEFAULT, REGISTER_HOOKS_NAME);

    if (register_func != NULL) {
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

        main_thread = pthread_self();
        no_hooks_should_be_called = JNI_FALSE;
        real_funcs_t* funcs = register_func(&test_hooks);

        // Check that all the real functions do not trigger the hooks.
        no_hooks_should_be_called = JNI_TRUE;
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
        register_func(NULL);
    } else {
        last_error = "Could not load the malloc hooks library.";
    }
#endif

    if (last_error != NULL) {
      return (*env)->NewStringUTF(env, last_error);
    }

    return NULL;
}

