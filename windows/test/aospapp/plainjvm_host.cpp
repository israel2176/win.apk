// What winapk-runtime.exe gives a JVM, for a plain-JVM test of AOSP's app layer (AospAppLifecycleTest):
// the two host natives it reaches (the log funnel, setenv; windows/host binds the real ones) and the
// binding of Robolectric's native runtime onto AOSP's graphics classes, with the host's own redirect
// (windows/host/aosp_gfx_redirect.cpp, as host/aosp_gfx.cpp uses it). The test loads this library
// with System.load; the natives bind by their JNI names.
//
//   c++ -std=c++17 -shared -fPIC -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
//       plainjvm_host.cpp ../../host/aosp_gfx_redirect.cpp -o libplainjvm_host.so
//   (Windows, MSYS2 CLANG64: clang++ ... -I.../win32 ... -o plainjvm_host.dll)
#include <jni.h>
#include <jvmti.h>

#include <cstdio>
#include <cstdlib>

#include "../../host/aosp_gfx_redirect.h"

namespace {
JavaVM* g_vm;

jvmtiEnv* jvmti_of(JNIEnv* env) {
    JavaVM* vm = g_vm;
    if (!vm && env->GetJavaVM(&vm) != JNI_OK) return nullptr;
    jvmtiEnv* jvmti = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK) return nullptr;
    return jvmti;
}
}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    return JNI_VERSION_1_8;
}

// com.macapk.loader.NativeLog.print(int level, String tag, String msg): to stderr.
JNIEXPORT void JNICALL Java_com_macapk_loader_NativeLog_print(JNIEnv* env, jclass, jint level, jstring tag,
                                                              jstring msg) {
    const char* t = tag ? env->GetStringUTFChars(tag, nullptr) : nullptr;
    const char* m = msg ? env->GetStringUTFChars(msg, nullptr) : nullptr;
    std::fprintf(stderr, "[log %d] %s: %s\n", static_cast<int>(level), t ? t : "", m ? m : "");
    if (t) env->ReleaseStringUTFChars(tag, t);
    if (m) env->ReleaseStringUTFChars(msg, m);
}

// com.macapk.loader.NativeOs.setenv(String name, String value, boolean overwrite): 0 or -errno.
JNIEXPORT jint JNICALL Java_com_macapk_loader_NativeOs_setenv(JNIEnv* env, jclass, jstring name, jstring value,
                                                              jboolean overwrite) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    const char* v = env->GetStringUTFChars(value, nullptr);
#ifdef _WIN32
    int rc = (overwrite || !std::getenv(n)) ? _putenv_s(n, v) : 0;
#else
    int rc = setenv(n, v, overwrite ? 1 : 0);
#endif
    env->ReleaseStringUTFChars(name, n);
    env->ReleaseStringUTFChars(value, v);
    return rc == 0 ? 0 : -22;
}

// com.macapk.loader.NativeCocoa's display: a headless 360x640 dp phone at density 2 and 60 Hz, as the
// runtime's headless mode reports one (WINAPK_HEADLESS=1).
JNIEXPORT jint JNICALL Java_com_macapk_loader_NativeCocoa_phoneWidth(JNIEnv*, jclass) { return 360; }
JNIEXPORT jint JNICALL Java_com_macapk_loader_NativeCocoa_phoneHeight(JNIEnv*, jclass) { return 640; }
JNIEXPORT jfloat JNICALL Java_com_macapk_loader_NativeCocoa_backingScale(JNIEnv*, jclass) { return 2.0f; }
JNIEXPORT jint JNICALL Java_com_macapk_loader_NativeCocoa_displayHz(JNIEnv*, jclass) { return 60; }

// AospAppLifecycleTest.redirectBegin() / redirectEnd(): around System.load of the native runtime, as
// host/aosp_gfx.cpp does around its LoadLibrary.
JNIEXPORT jboolean JNICALL Java_AospAppLifecycleTest_redirectBegin(JNIEnv* env, jclass) {
    jvmtiEnv* jvmti = jvmti_of(env);
    return jvmti && winapk::host::aosp_gfx::redirect_begin(jvmti, env) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_AospAppLifecycleTest_redirectEnd(JNIEnv* env, jclass) {
    jvmtiEnv* jvmti = jvmti_of(env);
    if (!jvmti || !winapk::host::aosp_gfx::redirect_end(jvmti)) return -1;
    auto c = winapk::host::aosp_gfx::redirect_counts();
    return c.bound + c.adapted;
}

}  // extern "C"
