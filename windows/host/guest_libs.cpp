// guest_libs.cpp -- loads an app's x86_64 or arm64 native libraries; see
// guest_libs.h.

#include "guest_libs.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "winapk/abi_thunk.h"
#include "winapk/arm64_guest.h"
#include "winapk/guest_jni.h"
#include "winapk/jni_bind.h"
#include "winapk/linker.h"
#include "winapk/mini_bionic.h"

#include "native_asset.h"
#include "winapk/jnigraphics.h"

#include "native_aaudio.h"
#include "native_activity.h"
#include "native_media.h"
#include "ndk_camera.h"
#include "native_sensor.h"
#include "native_opensles.h"
#include "native_window.h"
#include "vk/guest_vk.h"
#include "stub_hits.h"

namespace {

std::mutex g_lock;
std::unique_ptr<winapk::GuestLinker> g_linker;
JavaVM* g_guest_vm = nullptr;
JavaVM* g_host_vm = nullptr;  // the JVM's own: JNIEnv for the thread running a JNI_OnLoad
// The stub module of each app library System.load()'ed, and its guest library.
std::unordered_map<jlong, winapk::GuestLibrary*> g_by_module;

// Handles System.load() gave the JVM for an ELF it was handed itself (see
// load_library): the GuestLibrary, not a module, with how many of the JVM's
// libraries hold it (two paths can name one ELF, which the linker opens once).
std::unordered_map<jlong, int> g_direct;
// Each library whose JNI_OnLoad has run, with the version it returned: a second
// module for the same ELF (a stub and a System.load of the file itself) does
// not run it again, as a second load of an open library is a no-op on Android.
std::unordered_map<winapk::GuestLibrary*, jint> g_onload_done;
// The libraries whose JNI_OnLoad is running (see LoadingLibrary).
std::vector<winapk::GuestLibrary*> g_onloading;

using FindEntry = jlong(JNICALL*)(JNIEnv*, jclass, jlong, jstring);
FindEntry g_jdk_find_entry = nullptr;
using LoadLibraryNative = jboolean(JNICALL*)(JNIEnv*, jclass, jobject, jstring, jboolean, jboolean);
LoadLibraryNative g_jdk_load = nullptr;
using UnloadLibraryNative = void(JNICALL*)(JNIEnv*, jclass, jstring, jboolean, jlong);
UnloadLibraryNative g_jdk_unload = nullptr;

std::string utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

bool is_guest_code(const void* addr) {
    winapk::GuestLinker* linker = winapk::GuestLinker::process();
    return linker && linker->image_containing(addr);
}

// ---- system functions the shim does not have yet ----------------------------

// Called (Microsoft x64, from the tagged stub) by the guest; see
// make_tagged_zero_stub. The guest gets 0. Counted for the summary at exit
// (stub_hits.h), logged the first time; two threads may meet in the same stub.
void report_missing(void*, const void* tag) {
    auto* t = static_cast<winapk::host::StubHit*>(const_cast<void*>(tag));
    if (!winapk::host::stub_hit(t)) return;
    std::fprintf(stderr, "[win.apk] guest called a system function not implemented on Windows yet, returning 0: %s\n",
                 t->name.c_str());
    std::fflush(stderr);
}

void* missing_stub(const char* name) {
    static std::mutex lock;
    static std::unordered_map<std::string, void*> stubs;
    std::lock_guard<std::mutex> hold(lock);
    auto it = stubs.find(name);
    if (it != stubs.end()) return it->second;
    winapk::host::StubHit* tag = winapk::host::stub_hit_register(name, "system function");
    void* stub = winapk::make_tagged_zero_stub(reinterpret_cast<void*>(&report_missing), tag);
    stubs.emplace(name, stub);
    return stub;
}

// A system variable the shim does not have: zeroed memory, large enough for
// the objects libraries import (FILE arrays, vtables, interface ids).
void* missing_object(const char* name) {
    static std::mutex lock;
    static std::unordered_map<std::string, void*> blocks;
    std::lock_guard<std::mutex> hold(lock);
    auto it = blocks.find(name);
    if (it != blocks.end()) return it->second;
    void* block = std::calloc(1, 4096);
    if (!block) return nullptr;  // the linker then names it among the missing symbols instead of a null object
    blocks.emplace(name, block);
    std::fprintf(stderr, "[win.apk] guest uses a system variable not implemented on Windows yet, bound to zeroed memory: %s\n",
                 name);
    return block;
}

// A guest library is being unloaded (dlclose brought its last reference
// down): the thunks HotSpot was handed for its functions and the stub-module
// entry that named it must not outlive it. A library the JVM System.load()ed
// keeps the JVM's reference, so this only happens for one the app dlopen'ed
// itself.
void on_guest_unload(const winapk::ElfImage* image, void*) {
    winapk::guest_native_thunks_forget(image->base(), image->mapped_size());
    std::lock_guard<std::mutex> hold(g_lock);
    for (auto it = g_by_module.begin(); it != g_by_module.end();) {
        it = winapk::GuestLinker::image_of(it->second) == image ? g_by_module.erase(it) : std::next(it);
    }
    for (auto it = g_onload_done.begin(); it != g_onload_done.end();) {
        it = winapk::GuestLinker::image_of(it->first) == image ? g_onload_done.erase(it) : std::next(it);
    }
}

// A library's natives found by name while its JNI_OnLoad runs, as on ART. ART puts the library in
// its class loader's list before calling JNI_OnLoad (java_vm_ext.cc, LoadNativeLibrary), so a
// static initializer that JNI_OnLoad sets off (GetStaticMethodID initializes the class, on ART
// too) finds the library's Java_* functions. HotSpot puts it in the list (NativeLibraries'
// `libraries`) only once JNI_OnLoad has returned, and sets its handle after that too: libpag's
// JNI_OnLoad asks for org.libpag.VideoSurface.Make, whose <clinit> calls its native nativeInit,
// which threw UnsatisfiedLinkError; the lookup answered null and Roborock aborted at its next
// call with it. So for the length of JNI_OnLoad the JDK's record of the load being made
// (NativeLibraryContext.peek()) is put in its class loader's map, and a lookup through it, its
// handle not set yet, is answered from the libraries whose JNI_OnLoad is running.
class LoadingLibrary {
public:
    LoadingLibrary(JNIEnv* env, winapk::GuestLibrary* lib) : env_(env), lib_(lib) {
        {
            std::lock_guard<std::mutex> hold(g_lock);
            g_onloading.push_back(lib);
        }
        if (env_) list_in_loader();
    }
    ~LoadingLibrary() {
        if (map_) {
            jthrowable pending = env_->ExceptionOccurred();
            env_->ExceptionClear();
            // The JDK puts it back itself once the load has succeeded.
            if (jclass map_cls = env_->FindClass("java/util/Map")) {
                jmethodID remove = env_->GetMethodID(map_cls, "remove", "(Ljava/lang/Object;)Ljava/lang/Object;");
                if (remove) {
                    jobject old = env_->CallObjectMethod(map_, remove, key_);
                    if (old) env_->DeleteLocalRef(old);
                }
                env_->DeleteLocalRef(map_cls);
            }
            env_->ExceptionClear();
            env_->DeleteGlobalRef(map_);
            env_->DeleteGlobalRef(key_);
            if (pending) {
                env_->Throw(pending);
                env_->DeleteLocalRef(pending);
            }
        }
        std::lock_guard<std::mutex> hold(g_lock);
        for (auto it = g_onloading.rbegin(); it != g_onloading.rend(); ++it) {
            if (*it == lib_) {
                g_onloading.erase(std::next(it).base());
                break;
            }
        }
    }
    LoadingLibrary(const LoadingLibrary&) = delete;
    LoadingLibrary& operator=(const LoadingLibrary&) = delete;

private:
    void list_in_loader() {
        JNIEnv* env = env_;
        jthrowable pending = env->ExceptionOccurred();
        env->ExceptionClear();
        jobject impl = nullptr, from = nullptr, loader = nullptr, libs = nullptr, map = nullptr, name = nullptr;
        jclass ctx = env->FindClass("jdk/internal/loader/NativeLibraries$NativeLibraryContext");
        jmethodID peek =
            ctx ? env->GetStaticMethodID(ctx, "peek", "()Ljdk/internal/loader/NativeLibraries$NativeLibraryImpl;")
                : nullptr;
        impl = peek ? env->CallStaticObjectMethod(ctx, peek) : nullptr;
        jclass impl_cls = impl ? env->GetObjectClass(impl) : nullptr;
        jfieldID from_f = impl_cls ? env->GetFieldID(impl_cls, "fromClass", "Ljava/lang/Class;") : nullptr;
        jfieldID name_f = impl_cls ? env->GetFieldID(impl_cls, "name", "Ljava/lang/String;") : nullptr;
        from = from_f ? env->GetObjectField(impl, from_f) : nullptr;
        name = name_f ? env->GetObjectField(impl, name_f) : nullptr;
        jclass class_cls = env->FindClass("java/lang/Class");
        jmethodID get_loader =
            class_cls ? env->GetMethodID(class_cls, "getClassLoader", "()Ljava/lang/ClassLoader;") : nullptr;
        if (from && get_loader) loader = env->CallObjectMethod(from, get_loader);
        if (loader) {
            jclass cl = env->FindClass("java/lang/ClassLoader");
            jfieldID f = cl ? env->GetFieldID(cl, "libraries", "Ljdk/internal/loader/NativeLibraries;") : nullptr;
            libs = f ? env->GetObjectField(loader, f) : nullptr;
            if (cl) env->DeleteLocalRef(cl);
        } else if (from) {
            jclass boot = env->FindClass("jdk/internal/loader/BootLoader");
            jmethodID get =
                boot ? env->GetStaticMethodID(boot, "getNativeLibraries", "()Ljdk/internal/loader/NativeLibraries;")
                     : nullptr;
            libs = get ? env->CallStaticObjectMethod(boot, get) : nullptr;
            if (boot) env->DeleteLocalRef(boot);
        }
        if (libs) {
            jclass libs_cls = env->GetObjectClass(libs);
            jfieldID f = env->GetFieldID(libs_cls, "libraries", "Ljava/util/Map;");
            map = f ? env->GetObjectField(libs, f) : nullptr;
            env->DeleteLocalRef(libs_cls);
        }
        if (map && name) {
            jclass map_cls = env->FindClass("java/util/Map");
            jmethodID put_if_absent =
                map_cls ? env->GetMethodID(map_cls, "putIfAbsent",
                                           "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;")
                        : nullptr;
            jobject was = put_if_absent ? env->CallObjectMethod(map, put_if_absent, name, impl) : nullptr;
            if (put_if_absent && !env->ExceptionCheck() && !was) {
                map_ = env->NewGlobalRef(map);
                key_ = env->NewGlobalRef(name);
            }
            if (was) env->DeleteLocalRef(was);
            if (map_cls) env->DeleteLocalRef(map_cls);
        }
        // Another JDK than the one shipped: the names bind once the load has returned, as before.
        env->ExceptionClear();
        for (jobject o : {impl, from, loader, libs, map, name}) {
            if (o) env->DeleteLocalRef(o);
        }
        for (jclass c : {ctx, impl_cls, class_cls}) {
            if (c) env->DeleteLocalRef(c);
        }
        if (pending) {
            env->Throw(pending);
            env->DeleteLocalRef(pending);
        }
    }

    JNIEnv* env_;
    winapk::GuestLibrary* lib_;
    jobject map_ = nullptr;  // global refs while listed
    jobject key_ = nullptr;
};

// System.loadLibrary's second half on Android: the library's own JNI_OnLoad,
// found with dlsym on its handle, called with the JavaVM; JNI_VERSION_1_6 when
// it has none. Once per library: a later module for the same ELF gets the
// version the first call returned.
jint run_jni_onload(winapk::GuestLibrary* lib, const std::string& so) {
    {
        std::lock_guard<std::mutex> hold(g_lock);
        auto it = g_onload_done.find(lib);
        if (it != g_onload_done.end()) return it->second;
    }
    using OnLoad = jint(WINAPK_GUEST_ABI*)(JavaVM*, void*);
    std::string error;
    auto on_load = reinterpret_cast<OnLoad>(g_linker->lookup(lib, "JNI_OnLoad", nullptr, &error));
    jint version = JNI_VERSION_1_6;
    if (on_load) {
        // Its FindClass calls get ART's semantics (guest_jni.cpp, guest_find_class).
        JNIEnv* env = nullptr;
        if (g_host_vm && g_host_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) env = nullptr;
        LoadingLibrary loading(env, lib);
        winapk::guest_jni_enter_onload();
        version = reinterpret_cast<OnLoad>(winapk::arm64::host_callable(reinterpret_cast<void*>(on_load)))(
            g_guest_vm, nullptr);
        winapk::guest_jni_leave_onload();
        // JNI_OnLoad's address lets a guest crash address in a log be mapped
        // back to the library (its st_value gives the load bias).
        std::fprintf(stderr, "[win.apk] %s: JNI_OnLoad (at %p) returned 0x%x\n", so.c_str(),
                     reinterpret_cast<void*>(on_load), static_cast<unsigned>(version));
    }
    std::lock_guard<std::mutex> hold(g_lock);
    g_onload_done.emplace(lib, version);
    return version;
}

// ---- the app library behind a stub module -----------------------------------

// The .so a stub copy stands for: the translator writes its path, UTF-8, to
// <stub>.source.
std::wstring source_of(const std::wstring& stub_path) {
    std::wstring so;
    FILE* f = _wfopen((stub_path + L".source").c_str(), L"rb");
    if (!f) return so;
    std::string bytes;
    char buf[1024];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) bytes.append(buf, n);
    std::fclose(f);
    while (!bytes.empty() && (bytes.back() == '\n' || bytes.back() == '\r')) bytes.pop_back();
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    so.resize(static_cast<size_t>(n > 0 ? n : 0));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), &so[0], n);
    return so;
}

// Loads (or finds) the guest library a stub copy stands for, with its
// dependencies and constructors, as the dynamic linker does.
winapk::GuestLibrary* open_for_stub(const std::wstring& stub_path) {
    const std::wstring so = source_of(stub_path);
    if (so.empty()) {
        std::fprintf(stderr, "[win.apk] %s names no .so (no .source file)\n", utf8(stub_path).c_str());
        return nullptr;
    }
    std::string error;
    winapk::GuestLibrary* lib =
        g_linker->open(utf8(so), winapk::GuestLinker::kRtldNow, &error);
    if (!lib) std::fprintf(stderr, "[win.apk] cannot load %s: %s\n", utf8(so).c_str(), error.c_str());
    return lib;
}

// jdk.internal.loader.NativeLibrary.findEntry0(long handle, String name): for
// a stub module, the guest library's symbol (its dependency tree included, as
// dlsym on an Android handle); the NativeMethodBind hook wraps it before
// HotSpot calls it. For any other module, the JDK's own lookup.
jlong JNICALL find_entry(JNIEnv* env, jclass cls, jlong handle, jstring name) {
    winapk::GuestLibrary* lib = nullptr;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        auto it = g_by_module.find(handle);
        if (it != g_by_module.end()) lib = it->second;
    }
    std::vector<winapk::GuestLibrary*> loading;
    if (!lib && handle == 0) {
        // A library whose JNI_OnLoad is running (LoadingLibrary): its handle is not set yet.
        std::lock_guard<std::mutex> hold(g_lock);
        loading = g_onloading;
    }
    if (!lib && loading.empty()) return g_jdk_find_entry ? g_jdk_find_entry(env, cls, handle, name) : 0;
    const char* n = env->GetStringUTFChars(name, nullptr);
    if (!n) return 0;
    std::string error;
    void* p = nullptr;
    if (lib) {
        p = g_linker->lookup(lib, n, nullptr, &error);
    } else {
        for (auto it = loading.rbegin(); it != loading.rend() && !p; ++it) p = g_linker->lookup(*it, n, nullptr, &error);
    }
    env->ReleaseStringUTFChars(name, n);
    return reinterpret_cast<jlong>(p);
}

std::wstring java_wstring(JNIEnv* env, jstring s) {
    std::wstring w;
    if (!s) return w;
    const jchar* chars = env->GetStringChars(s, nullptr);
    if (!chars) return w;
    w.assign(reinterpret_cast<const wchar_t*>(chars), static_cast<size_t>(env->GetStringLength(s)));
    env->ReleaseStringChars(s, chars);
    return w;
}

bool is_elf_file(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    unsigned char magic[4] = {};
    const bool elf = std::fread(magic, 1, sizeof(magic), f) == sizeof(magic) && magic[0] == 0x7f &&
                     magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
    std::fclose(f);
    return elf;
}

void throw_unsatisfied_link(JNIEnv* env, const std::string& message) {
    if (jclass c = env->FindClass("java/lang/UnsatisfiedLinkError")) env->ThrowNew(c, message.c_str());
}

// jdk.internal.loader.NativeLibraries.load(NativeLibraryImpl impl, String name,
// boolean isBuiltin, boolean throwExceptionIfFail), where every System.load
// and System.loadLibrary of the JDK ends. On Android, System.load takes the
// path of an ELF however it is called; here an ELF reaches this point when app
// code calls System.load or Runtime.load by reflection (obfuscated code can,
// with the names in encrypted strings) or from a class of a dex loader: calls
// the rewriter's Pattern N, which gives the others a stub, never sees. The
// ELF is then opened through the guest linker, as dlopen does, its JNI_OnLoad
// runs, and the JVM is handed the GuestLibrary as the library's handle, which
// find_entry and unload_library know. The library binds to the class loader
// the JDK chose, the caller's, as on Android. Anything else is the JDK's.
jboolean JNICALL load_library(JNIEnv* env, jclass cls, jobject impl, jstring name, jboolean builtin,
                              jboolean throw_if_fail) {
    const std::wstring path = builtin ? std::wstring() : java_wstring(env, name);
    if (path.empty() || !g_linker || !is_elf_file(path)) {
        return g_jdk_load(env, cls, impl, name, builtin, throw_if_fail);
    }
    const std::string so = utf8(path);
    std::string error;
    winapk::GuestLibrary* lib = g_linker->open(so, winapk::GuestLinker::kRtldNow, &error);
    if (!lib) {
        std::fprintf(stderr, "[win.apk] cannot load %s: %s\n", so.c_str(), error.c_str());
        if (throw_if_fail) throw_unsatisfied_link(env, "dlopen failed: " + error);
        return JNI_FALSE;
    }
    const jint version = run_jni_onload(lib, so);
    if (env->ExceptionCheck()) {
        g_linker->close(lib, nullptr);
        return JNI_FALSE;
    }
    // ART takes only these from JNI_OnLoad (java_vm_ext.cc, IsBadJniVersion).
    if (version != JNI_VERSION_1_2 && version != JNI_VERSION_1_4 && version != JNI_VERSION_1_6) {
        g_linker->close(lib, nullptr);
        if (throw_if_fail) {
            throw_unsatisfied_link(env, "Bad JNI version returned from JNI_OnLoad in \"" + so +
                                            "\": " + std::to_string(version));
        }
        return JNI_FALSE;
    }
    jclass impl_cls = env->GetObjectClass(impl);
    const jfieldID handle_field = impl_cls ? env->GetFieldID(impl_cls, "handle", "J") : nullptr;
    const jfieldID version_field = impl_cls ? env->GetFieldID(impl_cls, "jniVersion", "I") : nullptr;
    if (!handle_field || !version_field) {
        g_linker->close(lib, nullptr);
        return JNI_FALSE;
    }
    const jlong handle = reinterpret_cast<jlong>(lib);
    {
        std::lock_guard<std::mutex> hold(g_lock);
        g_by_module[handle] = lib;
        ++g_direct[handle];
    }
    std::fprintf(stderr, "[win.apk] System.load of the ELF %s: loaded by the guest linker\n", so.c_str());
    env->SetIntField(impl, version_field, version);
    env->SetLongField(impl, handle_field, handle);
    return JNI_TRUE;
}

// jdk.internal.loader.NativeLibraries.unload(String name, boolean isBuiltin,
// long handle): the class loader a library was bound to is gone. For a handle
// load_library gave, the library's JNI_OnUnload and dlclose, as ART does when
// it unloads a class loader's libraries; the JDK's for a module.
void JNICALL unload_library(JNIEnv* env, jclass cls, jstring name, jboolean builtin, jlong handle) {
    winapk::GuestLibrary* lib = nullptr;
    bool last = false;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        auto it = g_direct.find(handle);
        if (it != g_direct.end()) {
            lib = reinterpret_cast<winapk::GuestLibrary*>(handle);
            last = --it->second == 0;
            if (last) {
                g_direct.erase(it);
                g_by_module.erase(handle);
            }
        }
    }
    if (!lib) {
        g_jdk_unload(env, cls, name, builtin, handle);
        return;
    }
    using OnUnload = void(WINAPK_GUEST_ABI*)(JavaVM*, void*);
    std::string error;
    if (!last) {
        g_linker->close(lib, nullptr);
        return;
    }
    if (auto on_unload = reinterpret_cast<OnUnload>(g_linker->lookup(lib, "JNI_OnUnload", nullptr, &error))) {
        reinterpret_cast<OnUnload>(winapk::arm64::host_callable(reinterpret_cast<void*>(on_unload)))(g_guest_vm,
                                                                                                     nullptr);
    }
    g_linker->close(lib, nullptr);
}

// com.macapk.loader.NativeLinker.nativeLinkOnly(String path): null when mapped,
// else the error.
jstring JNICALL link_only(JNIEnv* env, jclass, jstring path) {
    if (!path) return env->NewStringUTF("null path");
    const jchar* chars = env->GetStringChars(path, nullptr);
    const std::wstring stub(reinterpret_cast<const wchar_t*>(chars), static_cast<size_t>(env->GetStringLength(path)));
    env->ReleaseStringChars(path, chars);
    return open_for_stub(stub) ? nullptr : env->NewStringUTF("cannot load the library (see the log)");
}

// The system libraries' functions: the Bionic shim's, then libEGL/libGLES and
// libandroid's native window, assets, activity, input and configuration,
// libOpenSLES and libaaudio, which live with the host's GL, window, Java and
// audio.
void* resolve_system(const char* name) {
    if (void* p = winapk::mini_bionic_resolve(name)) return p;
    if (void* p = winapk::host::guest_gl_resolve(name)) return p;
    if (void* p = winapk::host::guest_vk_resolve(name)) return p;
    if (void* p = winapk::host::native_window_resolve(name)) return p;
    if (void* p = winapk::host::native_asset_resolve(name)) return p;
    if (void* p = winapk::host::native_activity_resolve(name)) return p;
    if (void* p = winapk::host::native_aaudio_resolve(name)) return p;
    if (void* p = winapk::host::native_media_resolve(name)) return p;
    if (void* p = winapk::host::ndk_camera_resolve(name)) return p;
    if (void* p = winapk::host::native_sensor_resolve(name)) return p;
    if (void* p = winapk::host::native_opensles_resolve(name)) return p;
    return winapk::jnigraphics_resolve(name);
}

const winapk::host::HostMethod kLinkerMethods[] = {
    WINAPK_HOST_NATIVE("com.macapk.loader.NativeLinker", "nativeLinkOnly", "(Ljava/lang/String;)Ljava/lang/String;",
                       link_only),
};

// com.macapk.loader.CriticalNatives.isCritical, for jni_bind: whether ART
// would call a guest native as @CriticalNative.
jclass g_critical_cls = nullptr;
jmethodID g_critical_mid = nullptr;

bool is_critical_native(JNIEnv* env, jclass cls, const char* name, const char* sig) {
    if (!g_critical_cls || !g_critical_mid || !cls || !name || !sig) return false;
    // A pending exception (RegisterNatives' caller may have one) would make the
    // call fail; it is kept for the caller.
    jthrowable pending = env->ExceptionOccurred();
    if (pending) env->ExceptionClear();
    jstring jn = env->NewStringUTF(name);
    jstring js = env->NewStringUTF(sig);
    const bool critical = jn && js && env->CallStaticBooleanMethod(g_critical_cls, g_critical_mid, cls, jn, js);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (jn) env->DeleteLocalRef(jn);
    if (js) env->DeleteLocalRef(js);
    if (pending) {
        env->Throw(pending);
        env->DeleteLocalRef(pending);
    }
    return critical;
}

}  // namespace

namespace winapk::host {

bool guest_libs_init(JNIEnv* env, jvmtiEnv* jvmti) {
    if (jclass c = env->FindClass("com/macapk/loader/CriticalNatives")) {
        g_critical_cls = static_cast<jclass>(env->NewGlobalRef(c));
        g_critical_mid =
            env->GetStaticMethodID(c, "isCritical", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;)Z");
        env->DeleteLocalRef(c);
        if (g_critical_mid) winapk::set_critical_native_query(&is_critical_native);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);
    g_host_vm = vm;
    // Upstream's order: the guest JNI view and the bind hook exist before the
    // first guest library is loaded.
    winapk::guest_jni_set_signature_provider(winapk::jvmti_method_signature, jvmti);
    g_guest_vm = winapk::guest_jni_init(vm);
    if (winapk::install_native_method_bind(vm, is_guest_code) != JNI_OK) {
        std::fprintf(stderr, "[win.apk] cannot install the NativeMethodBind hook\n");
        return false;
    }

    winapk::LinkerConfig config;
    config.host = resolve_system;
    config.load.unresolved = missing_stub;
    config.load.unresolved_object = missing_object;
    g_linker = std::make_unique<winapk::GuestLinker>(config);
    g_linker->add_unload_hook(on_guest_unload, nullptr);

    // HotSpot's lookup of a native method by name ends in this static native.
    HMODULE java_dll = GetModuleHandleW(L"java.dll");
    g_jdk_find_entry = java_dll ? reinterpret_cast<FindEntry>(GetProcAddress(
                                      java_dll, "Java_jdk_internal_loader_NativeLibrary_findEntry0"))
                                : nullptr;
    jclass cls = env->FindClass("jdk/internal/loader/NativeLibrary");
    JNINativeMethod m{const_cast<char*>("findEntry0"), const_cast<char*>("(JLjava/lang/String;)J"),
                      reinterpret_cast<void*>(&find_entry)};
    if (!g_jdk_find_entry || !cls || env->RegisterNatives(cls, &m, 1) != JNI_OK) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        std::fprintf(stderr, "[win.apk] cannot hook NativeLibrary.findEntry0; app natives bound by name will not resolve\n");
        return false;
    }
    // System.load of an ELF however it is called (load_library). Without it
    // only the call sites the rewriter routes load one.
    g_jdk_load = java_dll ? reinterpret_cast<LoadLibraryNative>(
                                GetProcAddress(java_dll, "Java_jdk_internal_loader_NativeLibraries_load"))
                          : nullptr;
    g_jdk_unload = java_dll ? reinterpret_cast<UnloadLibraryNative>(
                                  GetProcAddress(java_dll, "Java_jdk_internal_loader_NativeLibraries_unload"))
                            : nullptr;
    jclass libs = env->FindClass("jdk/internal/loader/NativeLibraries");
    const JNINativeMethod load_methods[] = {
        {const_cast<char*>("load"),
         const_cast<char*>("(Ljdk/internal/loader/NativeLibraries$NativeLibraryImpl;Ljava/lang/String;ZZ)Z"),
         reinterpret_cast<void*>(&load_library)},
        {const_cast<char*>("unload"), const_cast<char*>("(Ljava/lang/String;ZJ)V"),
         reinterpret_cast<void*>(&unload_library)},
    };
    if (!g_jdk_load || !g_jdk_unload || !libs || env->RegisterNatives(libs, load_methods, 2) != JNI_OK) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        std::fprintf(stderr, "[win.apk] cannot hook NativeLibraries.load; System.load of an ELF works only where "
                             "the rewriter routes it\n");
    }
    return true;
}

HostMethods native_linker_methods() {
    return {kLinkerMethods, sizeof(kLinkerMethods) / sizeof(kLinkerMethods[0])};
}

void* guest_library_open(const std::wstring& path, std::string* error) {
    if (!g_linker) {
        if (error) *error = "the guest linker is not initialized";
        return nullptr;
    }
    // A stub copy names its ELF in <stub>.source; anything else is the ELF.
    const std::wstring so = source_of(path);
    if (!so.empty()) {
        winapk::GuestLibrary* lib = open_for_stub(path);
        if (!lib && error) *error = "cannot load " + utf8(so) + " (see the log)";
        return lib;
    }
    std::string err;
    winapk::GuestLibrary* lib = g_linker->open(utf8(path), winapk::GuestLinker::kRtldNow, &err);
    if (!lib) {
        std::fprintf(stderr, "[win.apk] cannot load %s: %s\n", utf8(path).c_str(), err.c_str());
        if (error) *error = err;
    }
    return lib;
}

void* guest_library_symbol(void* library, const char* name, std::string* error) {
    if (!g_linker || !library || !name) return nullptr;
    std::string err;
    void* p = g_linker->lookup(library, name, nullptr, &err);
    if (!p && error) *error = err;
    return p;
}

}  // namespace winapk::host

extern "C" jint winapk_guest_library_loaded(JavaVM*, void* module, const wchar_t* stub_path) {
    if (!g_linker) return JNI_ERR;
    winapk::GuestLibrary* lib = open_for_stub(stub_path);
    if (!lib) return JNI_ERR;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        g_by_module[reinterpret_cast<jlong>(module)] = lib;
    }
    return run_jni_onload(lib, utf8(source_of(stub_path)));
}
