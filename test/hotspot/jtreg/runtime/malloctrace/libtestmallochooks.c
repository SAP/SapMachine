#if defined(LINUX) || defined(__APPLE__)

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

#if !defined(__APPLE__)
#include <malloc.h>
#endif

#include "jni.h"

#include "mallochooks.h"

#define MAX_ALLOC 8192
#define MAX_CALLOC 64

#define SAFE_PRIME_64 7570865665517081723ull
#define SAFE_PRIME 1000000007

int next_rand(int last, int base) {
  return (int) ((((unsigned long long) last) * base) % SAFE_PRIME);
}

#define MALLOC 0
#define CALLOC 1
#define REALLOC 2
#define POSIX_MEMALIGN 3
#define MEMALIGN 4
#define ALIGNED_ALLOC 5
#define VALLOC 6
#define PVALLOC 7

#define TRACK(what, size) \
if (roots[idx] != NULL) { \
    if (trackLive) { \
        sizes[(what)] += funcs->malloc_size(roots[idx]); \
    } else { \
        sizes[(what)] += (size); \
    } \
    counts[(what)] += 1; \
    source[(idx)] = (what); \
} else { \
    source[(idx)] = -1; \
}

static void do_alloc_with_stack_impl(int size, int type);
static void do_alloc_with_stack2(int size, int type, int stack);

static void do_alloc_with_stack1(int size, int type, int stack) {
    int new_stack = stack / 2;

    if (new_stack == 0) {
        do_alloc_with_stack_impl(size, type);
    } else if (new_stack & 1) {
        do_alloc_with_stack1(size, type, new_stack);
    } else {
        do_alloc_with_stack2(size, type, new_stack);
    }

    // Inhibit tail call optimization.
    if (stack < 0) {
      abort();
    }
}

static void do_alloc_with_stack2(int size, int type, int stack) {
    int new_stack = stack / 2;

    if (new_stack == 0) {
        do_alloc_with_stack_impl(size, type);
    } else if (new_stack & 1) {
        do_alloc_with_stack1(size, type, new_stack);
    } else {
        do_alloc_with_stack2(size, type, new_stack);
    }

    // Inhibit tail call optimization.
    if (stack < 0) {
      abort();
    }
}

static void do_alloc_with_stack_impl(int size, int type) {
    void* mem = NULL;

    switch (type & 7) {
        case 0:
            mem = malloc(size);
            break;
        case 1:
            mem = calloc(1, size);
            break;
        case 2:
            mem = realloc(NULL, size);
            break;
        case 3:
            posix_memalign(&mem, 128, size);
            break;
#if !defined(__APPLE__)
        case 4:
            mem = memalign(128, size);
            break;
#endif
#if !defined(__APPLE__)
        case 5:
            mem = aligned_alloc(128, size);
            break;
#endif
#if defined(__GLIBC__) || defined(__APPLE__)
        case 6:
            mem = valloc(size);
            break;
#endif
#if defined(__GLIBC__)
        case 7:
            mem = pvalloc(size);
            break;
#endif
        default:
            mem = malloc(size);
    }

    free(mem);
}

JNIEXPORT void JNICALL
Java_MallocHooksTest_doRandomAllocsWithFrees(JNIEnv *env, jclass cls, jint nrOfOps, jint size,
                                             jint maxStack, jint seed) {
    int i;
    int rand = 1;
    int stack_rand = 1;

    for (i = 0; i < nrOfOps; ++i) {
        rand = next_rand(rand, seed);
        stack_rand = next_rand(rand, seed);

        if (stack_rand & 1) {
            do_alloc_with_stack1(size, rand & 7, stack_rand & ((1 << maxStack) - 1));
        } else {
            do_alloc_with_stack2(size, rand & 7, stack_rand & ((1 << maxStack) - 1));
        }
    }
}

JNIEXPORT void JNICALL
Java_MallocHooksTest_doRandomMemOps(JNIEnv *env, jclass cls, jint nrOfOps, jint maxLiveAllocations, jint seed,
                                    jboolean trackLive, jlongArray resultSizes, jlongArray resultCounts) {
    get_real_funcs_t* get_real_funcs = (get_real_funcs_t*) dlsym((void*) RTLD_DEFAULT, GET_REAL_FUNCS_NAME);
    real_funcs_t* funcs = get_real_funcs();

    void** roots = funcs->calloc(maxLiveAllocations, sizeof(void*));
    signed char* source = (signed char*) funcs->calloc(maxLiveAllocations, sizeof(char));

    int i;
    int rand = 1;
    jlong sizes[] = {0, 0, 0, 0, 0, 0, 0, 0};
    jlong counts[] = {0, 0, 0, 0, 0, 0, 0, 0};

    for (i = 0; i < nrOfOps; ++i) {
        rand = next_rand(rand, seed);
        int idx = rand % maxLiveAllocations;

        if (roots[idx] == NULL) {
            rand = next_rand(rand, seed);
            int what = rand & 31;
            rand = next_rand(rand, seed);
            int malloc_size = rand & (MAX_ALLOC - 1);
            int calloc_size = rand & (MAX_CALLOC - 1);

            if (what < 11) {
                roots[idx] = malloc(malloc_size + 1);
                TRACK(MALLOC, malloc_size + 1);
            } else if (what < 22) {
                rand = next_rand(rand, seed);
                int calloc_count = rand & (MAX_CALLOC - 1);
                roots[idx] = calloc(calloc_count + 1, calloc_size + 1);
                TRACK(CALLOC, (calloc_count + 1) * (calloc_size + 1));
            } else if (what < 24) {
                void* mem;
                int result = posix_memalign(&mem, 64, malloc_size + 1);
                roots[idx] = result != 0 ? NULL : mem;
                TRACK(POSIX_MEMALIGN, result != 0 ? 0 : funcs->malloc_size(mem));
            } else if (what < 26) {
#if !defined(__APPLE__)
                roots[idx] = memalign(64, malloc_size + 1);
                TRACK(MEMALIGN, roots[idx] == NULL ? 0 : funcs->malloc_size(roots[idx]));
#endif
            } else if (what < 28) {
#if !defined(__APPLE__)
                roots[idx] = aligned_alloc(64, malloc_size + 1);
                TRACK(ALIGNED_ALLOC, roots[idx] == NULL ? 0 : funcs->malloc_size(roots[idx]));
#endif
            } else if (what < 30) {
#if defined(__GLIBC__) || defined(__APPLE__)
                roots[idx] = valloc(malloc_size + 1);
                TRACK(VALLOC, roots[idx] == NULL ? 0 : funcs->malloc_size(roots[idx]));
#endif
            } else {
#if defined(__GLIBC__)
                roots[idx] = pvalloc(malloc_size + 1);
                TRACK(PVALLOC, roots[idx] == NULL ? 0 : funcs->malloc_size(roots[idx]));
#endif
            }
        } else {
            rand = next_rand(rand, seed);

            if ((rand & 3) != 0) {
                if (trackLive) {
                    sizes[source[idx]] -= funcs->malloc_size(roots[idx]);
                    counts[source[idx]] -= 1;
                }
                free(roots[idx]);
                roots[idx] = NULL;
                source[idx] = -1;
            } else {
                rand = next_rand(rand, seed);
                size_t old_size = funcs->malloc_size(roots[idx]);
                int malloc_size = rand & (MAX_ALLOC - 1);
                roots[idx] = realloc(roots[idx], malloc_size + 1);
                if (roots[idx] != NULL) {
                    if (trackLive) {
                        sizes[source[idx]] -= old_size;
                        counts[source[idx]] -= 1;
                    }
                }
                if (trackLive) {
                    TRACK(REALLOC, malloc_size + 1);
                } else if (old_size < (size_t) (malloc_size + 1)) {
                    TRACK(REALLOC, malloc_size + 1 - old_size);
                }
            }
        }
    }

    (*env)->SetLongArrayRegion(env, resultSizes, 0, 8, sizes);
    (*env)->SetLongArrayRegion(env, resultCounts, 0, 8, counts);
}

#endif

