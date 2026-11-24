// native_accessibility.cpp -- com.macapk.loader.HostAccessibility on Windows: the app's side of
// the screen-reader bridge (uia_provider.h). The Java class walks the window's views on the
// app's main thread and publishes them; the provider calls back here, on its one worker thread,
// to turn the app's accessibility on or off, to ask for a snapshot, or to perform an action,
// each of which the Java side only posts to the main thread.

#include "host_natives.h"
#include "uia_provider.h"

#include <cstdio>
#include <mutex>
#include <string>

namespace winapk::host {

namespace {

JavaVM* g_vm = nullptr;
jclass g_class = nullptr;  // HostAccessibility, a global reference
jmethodID g_activate = nullptr, g_refresh = nullptr, g_perform = nullptr;

std::wstring wide(JNIEnv* env, jstring s) {
    if (!s) return std::wstring();
    const jsize n = env->GetStringLength(s);
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (n > 0) env->GetStringRegion(s, 0, n, reinterpret_cast<jchar*>(&out[0]));
    return out;
}

// The worker thread, attached once (a daemon: it never ends before the process).
JNIEnv* worker_env() {
    thread_local JNIEnv* env = nullptr;
    if (!env && g_vm) {
        JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char*>("winapk-accessibility"), nullptr};
        if (g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &args) != JNI_OK) env = nullptr;
    }
    return env;
}

void clear(JNIEnv* env) {
    if (!env->ExceptionCheck()) return;
    std::fprintf(stderr, "[win.apk] accessibility: exception from HostAccessibility\n");
    env->ExceptionDescribe();
    env->ExceptionClear();
}

class JavaApp : public uia::AppSide {
public:
    void activate(bool on) override {
        JNIEnv* env = worker_env();
        if (!env || !g_activate) return;
        env->CallStaticVoidMethod(g_class, g_activate, on ? JNI_TRUE : JNI_FALSE);
        clear(env);
    }
    void refresh() override {
        JNIEnv* env = worker_env();
        if (!env || !g_refresh) return;
        env->CallStaticVoidMethod(g_class, g_refresh);
        clear(env);
    }
    void perform(int64_t id, int action, const std::wstring& text) override {
        JNIEnv* env = worker_env();
        if (!env || !g_perform) return;
        jstring s = env->NewString(reinterpret_cast<const jchar*>(text.data()), static_cast<jsize>(text.size()));
        if (s) env->CallStaticVoidMethod(g_class, g_perform, static_cast<jlong>(id), static_cast<jint>(action), s);
        clear(env);
        if (s) env->DeleteLocalRef(s);
    }
};

JavaApp g_app;

void JNICALL Acc_install(JNIEnv* env, jclass cls) {
    if (g_class) return;
    env->GetJavaVM(&g_vm);
    g_class = static_cast<jclass>(env->NewGlobalRef(cls));
    g_activate = env->GetStaticMethodID(cls, "activate", "(Z)V");
    g_refresh = env->GetStaticMethodID(cls, "refresh", "()V");
    g_perform = env->GetStaticMethodID(cls, "perform", "(JILjava/lang/String;)V");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        g_activate = g_refresh = g_perform = nullptr;
        return;
    }
    uia::set_app(&g_app);
}

jboolean JNICALL Acc_due(JNIEnv*, jclass) { return uia::due() ? JNI_TRUE : JNI_FALSE; }

jboolean JNICALL Acc_publish(JNIEnv* env, jclass, jstring snapshot) {
    return uia::publish(wide(env, snapshot)) ? JNI_TRUE : JNI_FALSE;
}

void JNICALL Acc_announce(JNIEnv* env, jclass, jstring text, jboolean assertive) {
    uia::announce(wide(env, text), assertive == JNI_TRUE);
}

constexpr const char* kCls = "com.macapk.loader.HostAccessibility";

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(kCls, "nativeInstall", "()V", Acc_install),
    WINAPK_HOST_NATIVE(kCls, "nativeDue", "()Z", Acc_due),
    WINAPK_HOST_NATIVE(kCls, "nativePublish", "(Ljava/lang/String;)Z", Acc_publish),
    WINAPK_HOST_NATIVE(kCls, "nativeAnnounce", "(Ljava/lang/String;Z)V", Acc_announce),
};

}  // namespace

HostMethods native_accessibility_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
