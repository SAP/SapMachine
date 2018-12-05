#include <unistd.h>

#include "jni.h"


JNIEXPORT jlong Java_CreateNewProcessGroupOnSpawnTest_getpgid0 (JNIEnv *env, jclass ignore, jlong pid) {
    return (jlong) getpgid((pid_t)pid);
}
