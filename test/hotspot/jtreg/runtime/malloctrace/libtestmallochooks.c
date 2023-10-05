#if defined(LINUX) || defined(__APPLE__)

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#include "jni.h"

#define MAX_ALLOC 8192
#define MAX_CALLOC 64

JNIEXPORT void JNICALL
Java_MallocHooksTest_doRandomMemOps(JNIEnv *env, jclass cls, jint nrOfOps, jint maxLiveAllocations) {
    void** roots = calloc(maxLiveAllocations, sizeof(void*));
    int i;

    for (i = 0; i < nrOfOps; ++i) {
        int idx = (int) (drand48() * maxLiveAllocations);

        if (roots[idx] == NULL) {
            double r = drand48();

            if (r < 0.4) {
                roots[idx] = malloc((int) (drand48() * MAX_ALLOC + 1));
            } else if (r < 0.75) {
                roots[idx] = calloc((int) (drand48() * MAX_CALLOC + 1), (int) (drand48() * MAX_CALLOC + 1));
            } else if (r < 0.80) {
                posix_memalign(&roots[idx], 64, (int) (drand48() * MAX_ALLOC + 1));
            } else if (r < 0.85) {
#if !defined(__APPLE__)
                roots[idx] = memalign(64, (int) (drand48() * MAX_ALLOC + 1));
#endif
            } else if (r < 0.90) {
#if !defined(__APPLE__)
                roots[idx] = aligned_alloc(64, (int) (drand48() * MAX_ALLOC + 1));
#endif
            } else if (r < 0.95) {
#if defined(__GLIBC__) || defined(__APPLE__)
                roots[idx] = valloc((int) (drand48() * MAX_ALLOC + 1));
#endif
            } else {
#if defined(__GLIBC__)
                roots[idx] = pvalloc((int) (drand48() * MAX_ALLOC + 1));
#endif
            }
        } else {
            if (drand48() < 0.8) {
                free(roots[idx]);
                roots[idx] = NULL;
            } else {
                roots[idx] = realloc(roots[idx], (int) (drand48() * MAX_ALLOC + 1));
            }
        }
    }
}

#endif

