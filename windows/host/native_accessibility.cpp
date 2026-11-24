// native_accessibility.cpp -- com.macapk.loader.HostAccessibility's natives: the app's side of
// the window's UI Automation provider (uia_provider.h).
//
// Java to the host: nativeInstall (the class is ready; the provider may call in from now on),
// nativePublish (a snapshot of the app's windows, uia_tree.h's text), nativeAnnounce (an
// announcement). The host to Java, on the provider's worker thread, attached to the JVM as a
// daemon: HostAccessibility.hostActivate, hostRefresh and hostPerform, which only post to the
// app's main thread, so the worker never waits on the app.
#include "host_natives.h"
#include "uia_provider.h"

#include <mutex>
#include <string>

namespace winapk::host {
namespace {

constexpr const char* kCls = "com.macapk.loader.HostAccessibility";

JavaVM* g_vm = nullptr;
jclass g_class = nullptr;  // a global reference
jmethodID g_activate = nullptr, g_refresh = nullptr, g_perform = nullptr;

JNIEnv* env_here() {
    if (!g_vm) return nullptr;
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) return nullptr;
    return env;
}

std::wstring wide(JNIEnv* env, jstring s) {
    if (!s) return std::wstring();
    const jsize n = env->GetStringLength(s);
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (n) env->GetStringRegion(s, 0, n, reinterpret_cast<jchar*>(&out[0]));
    return out;
}

jstring java_string(JNIEnv* env, const std::wstring& s) {
    return env->NewString(reinterpret_cast<const jchar*>(s.data()), static_cast<jsize>(s.size()));
}

void call_failed(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

class JavaSide final : public uia::AppSide {
public:
    void activate(bool on) override {
        JNIEnv* env = env_here();
        if (!env) return;
        env->CallStaticVoidMethod(g_class, g_activate, on ? JNI_TRUE : JNI_FALSE);
        call_failed(env);
    }
    void refresh() override {
        JNIEnv* env = env_here();
        if (!env) return;
        env->CallStaticVoidMethod(g_class, g_refresh);
        call_failed(env);
    }
    void perform(int64_t id, int action, const std::wstring& text, int arg1, int arg2, double value) override {
        JNIEnv* env = env_here();
        if (!env) return;
        jstring t = java_string(env, text);
        env->CallStaticVoidMethod(g_class, g_perform, static_cast<jlong>(id), static_cast<jint>(action), t,
                                  static_cast<jint>(arg1), static_cast<jint>(arg2), static_cast<jfloat>(value));
        call_failed(env);
        if (t) env->DeleteLocalRef(t);
    }
};

JavaSide g_side;
std::once_flag g_installed;

void JNICALL Acc_install(JNIEnv* env, jclass cls) {
    std::call_once(g_installed, [&] {
        if (env->GetJavaVM(&g_vm) != JNI_OK) return;
        g_activate = env->GetStaticMethodID(cls, "hostActivate", "(Z)V");
        g_refresh = env->GetStaticMethodID(cls, "hostRefresh", "()V");
        g_perform = env->GetStaticMethodID(cls, "hostPerform", "(JILjava/lang/String;IIF)V");
        if (!g_activate || !g_refresh || !g_perform) {
            env->ExceptionClear();
            return;
        }
        g_class = static_cast<jclass>(env->NewGlobalRef(cls));
        uia::set_app(&g_side);
    });
}

jboolean JNICALL Acc_publish(JNIEnv* env, jclass, jstring snapshot) {
    return uia::publish(wide(env, snapshot)) ? JNI_TRUE : JNI_FALSE;
}

void JNICALL Acc_announce(JNIEnv* env, jclass, jstring text, jboolean assertive) {
    uia::announce(wide(env, text), assertive == JNI_TRUE);
}

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(kCls, "nativeInstall", "()V", Acc_install),
    WINAPK_HOST_NATIVE(kCls, "nativePublish", "(Ljava/lang/String;)Z", Acc_publish),
    WINAPK_HOST_NATIVE(kCls, "nativeAnnounce", "(Ljava/lang/String;Z)V", Acc_announce),
};

}  // namespace

HostMethods native_accessibility_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
