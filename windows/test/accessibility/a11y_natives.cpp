// AccessibilityTreeTest's two natives (redirectBegin, redirectEnd): the host's graphics redirect
// around System.load of Robolectric's native runtime, as windows/test/aospapp/plainjvm_host.cpp
// gives AospAppLifecycleTest. Linked into the same library as plainjvm_host.cpp (run.sh).
#include <jni.h>
#include <jvmti.h>

#include "../../host/aosp_gfx_redirect.h"

namespace {
jvmtiEnv* jvmti_of(JNIEnv* env) {
    JavaVM* vm = nullptr;
    jvmtiEnv* jvmti = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK || vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK) {
        return nullptr;
    }
    return jvmti;
}
}  // namespace

extern "C" {

JNIEXPORT jboolean JNICALL Java_AccessibilityTreeTest_redirectBegin(JNIEnv* env, jclass) {
    jvmtiEnv* jvmti = jvmti_of(env);
    return jvmti && winapk::host::aosp_gfx::redirect_begin(jvmti, env) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_AccessibilityTreeTest_redirectEnd(JNIEnv* env, jclass) {
    jvmtiEnv* jvmti = jvmti_of(env);
    if (!jvmti || !winapk::host::aosp_gfx::redirect_end(jvmti)) return -1;
    auto c = winapk::host::aosp_gfx::redirect_counts();
    return c.bound + c.adapted;
}

}  // extern "C"
