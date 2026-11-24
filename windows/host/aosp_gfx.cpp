// aosp_gfx.cpp -- see aosp_gfx.h.
#include "aosp_gfx.h"

#include "aosp_gfx_redirect.h"

#include <chrono>
#include <cstdio>

#include <jvmti.h>
#include <windows.h>

namespace fs = std::filesystem;

namespace winapk::host {
namespace {

AospGfxFiles g_files;
bool g_on = false;

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

jstring jstring_of(JNIEnv* env, const std::wstring& s) {
    return env->NewString(reinterpret_cast<const jchar*>(s.c_str()), static_cast<jsize>(s.size()));
}

}  // namespace

bool aosp_gfx_prepare(const fs::path& lib, const fs::path& bin) {
    g_on = false;
    AospGfxFiles f;
    f.jar = lib / "aosp-graphics.jar";
    f.dll = bin / "robolectric-nativeruntime.dll";
    f.icu_data = bin / "aosp" / "icu" / "icudt68l.dat";
    f.fonts = bin / "aosp" / "fonts";
    f.hyphen = bin / "aosp" / "hyphen-data";
    std::error_code ec;
    for (const fs::path& p : {f.jar, f.dll, f.icu_data, f.fonts / "fonts.xml"}) {
        if (!fs::is_regular_file(p, ec)) {
            std::fprintf(stderr, "[win.apk] error: AOSP graphics: %s is missing (a runtime staged without "
                                 "windows/scripts/build_aosp_gfx.sh's files, or a damaged install: reinstall "
                                 "Win.apk); nothing can draw without it\n",
                         utf8(p.wstring()).c_str());
            return false;
        }
    }
    if (!fs::is_directory(f.hyphen, ec)) f.hyphen.clear();  // no hyphenation then, nothing else lost
    // Loaded now, before the JVM, to know it can be: a DLL that cannot load (a missing UCRT on an
    // old Windows) would otherwise leave AOSP's classes on the class path with no natives. It
    // stays loaded; System.load finds it so.
    if (!LoadLibraryExW(f.dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
        std::fprintf(stderr, "[win.apk] error: AOSP graphics: %s does not load (error %lu: a damaged install, "
                             "or a Windows without the Universal C Runtime); nothing can draw without it\n",
                     utf8(f.dll.wstring()).c_str(), GetLastError());
        return false;
    }
    g_files = f;
    g_on = true;
    std::fprintf(stderr, "[win.apk] AOSP graphics: on: %s first on the class path\n",
                 utf8(f.jar.wstring()).c_str());
    return true;
}

const AospGfxFiles* aosp_gfx_files() { return g_on ? &g_files : nullptr; }

namespace {
AospResFiles g_res_files;
bool g_res_on = false;
}  // namespace

bool aosp_res_prepare(const fs::path& lib, const fs::path& bin) {
    g_res_on = false;
    // The switch is gone (aosp_gfx.h): say so to whoever still sets it, once.
    if (GetEnvironmentVariableW(L"WINAPK_AOSP_RES", nullptr, 0) > 0 ||
        GetEnvironmentVariableW(L"WINAPK_AOSP_VIEWS", nullptr, 0) > 0) {
        std::fprintf(stderr, "[win.apk] WINAPK_AOSP_RES and WINAPK_AOSP_VIEWS are ignored: AOSP's resources and views "
                             "are the only ones\n");
    }
    if (!g_on) {
        std::fprintf(stderr, "[win.apk] AOSP resources: off, they need AOSP's graphics: an app cannot start "
                             "without them\n");
        return false;
    }
    AospResFiles f;
    f.jar = lib / "aosp-resources.jar";
    f.framework = bin / "aosp" / "framework-res-34.apk";
    std::error_code ec;
    for (const fs::path& p : {f.jar, f.framework}) {
        if (!fs::is_regular_file(p, ec)) {
            std::fprintf(stderr, "[win.apk] AOSP resources: off, %s is missing (a runtime staged without "
                                 "windows/scripts/build_aosp_res.sh's files): an app cannot start without them\n",
                         utf8(p.wstring()).c_str());
            return false;
        }
    }
    g_res_files = f;
    g_res_on = true;
    std::fprintf(stderr, "[win.apk] AOSP resources: on: %s first on the class path\n", utf8(f.jar.wstring()).c_str());
    return true;
}

const AospResFiles* aosp_res_files() { return g_res_on ? &g_res_files : nullptr; }

namespace {
std::filesystem::path g_views_jar;
bool g_views_on = false;
}  // namespace

bool aosp_views_prepare(const fs::path& lib) {
    g_views_on = false;
    if (!g_res_on) {
        std::fprintf(stderr, "[win.apk] AOSP views: off, they need AOSP's graphics and resources: an app cannot "
                             "start without them\n");
        return false;
    }
    const fs::path jar = lib / "aosp-views.jar";
    std::error_code ec;
    if (!fs::is_regular_file(jar, ec)) {
        std::fprintf(stderr, "[win.apk] AOSP views: off, %s is missing (a runtime staged without "
                             "windows/scripts/build_aosp_views.sh's files): an app cannot start without them\n",
                     utf8(jar.wstring()).c_str());
        return false;
    }
    g_views_jar = jar;
    g_views_on = true;
    std::fprintf(stderr, "[win.apk] AOSP views: on: %s first on the class path\n", utf8(jar.wstring()).c_str());
    return true;
}

const std::filesystem::path* aosp_views_jar() { return g_views_on ? &g_views_jar : nullptr; }

namespace {
std::filesystem::path g_app_jar;
bool g_app_on = false;
}  // namespace

bool aosp_app_prepare(const fs::path& lib) {
    g_app_on = false;
    if (!g_views_on) {
        std::fprintf(stderr, "[win.apk] AOSP app layer: off, it needs AOSP's views: an app cannot start "
                             "without it\n");
        return false;
    }
    const fs::path jar = lib / "aosp-app.jar";
    std::error_code ec;
    if (!fs::is_regular_file(jar, ec)) {
        std::fprintf(stderr, "[win.apk] AOSP app layer: off, %s is missing (a runtime staged without "
                             "windows/scripts/build_aosp_app.sh's jar): an app cannot start without it\n",
                     utf8(jar.wstring()).c_str());
        return false;
    }
    g_app_jar = jar;
    g_app_on = true;
    std::fprintf(stderr, "[win.apk] AOSP app layer: on: %s first on the class path\n", utf8(jar.wstring()).c_str());
    return true;
}

const std::filesystem::path* aosp_app_jar() { return g_app_on ? &g_app_jar : nullptr; }

std::string aosp_gfx_locale() {
    // WINAPK_LOCALES (runtime_main.cpp): the device's first system language over Windows'.
    if (const char* l = std::getenv("WINAPK_LOCALES"); l && *l) {
        const std::string s(l);
        return s.substr(0, s.find(','));
    }
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) return utf8(name);
    return "en-US";
}

bool aosp_gfx_load(JavaVM* vm, JNIEnv* env) {
    if (!g_on) return false;
    const auto t0 = std::chrono::steady_clock::now();
    jvmtiEnv* jvmti = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK || !jvmti) {
        std::fprintf(stderr, "[win.apk] error: AOSP graphics: no JVMTI; the DLL's natives are not bound\n");
        return false;
    }
    // The library's JNI_OnLoad finds classes through the class loader of the class that loads it,
    // so it is loaded for one of aosp-graphics.jar's (the class path's): Runtime.load0(Class,
    // String) is System.load with the caller named. RuntimeEnvironment, whose static
    // initializer is empty: FindClass initializes, and android.graphics' would call natives
    // not yet bound.
    jclass from = env->FindClass("org/robolectric/RuntimeEnvironment");
    jclass runtime_cls = env->FindClass("java/lang/Runtime");
    jmethodID get = runtime_cls ? env->GetStaticMethodID(runtime_cls, "getRuntime", "()Ljava/lang/Runtime;") : nullptr;
    jmethodID load0 =
        runtime_cls ? env->GetMethodID(runtime_cls, "load0", "(Ljava/lang/Class;Ljava/lang/String;)V") : nullptr;
    jobject runtime = get ? env->CallStaticObjectMethod(runtime_cls, get) : nullptr;
    if (!from || !load0 || !runtime) {
        if (env->ExceptionCheck()) env->ExceptionDescribe();
        env->ExceptionClear();
        std::fprintf(stderr, "[win.apk] error: AOSP graphics: cannot load the DLL (no %s); its natives are not bound\n",
                     !from ? "org.robolectric.RuntimeEnvironment" : "Runtime.load0");
        return false;
    }
    if (!aosp_gfx::redirect_begin(jvmti, env)) {
        std::fprintf(stderr, "[win.apk] error: AOSP graphics: the JNI function table cannot be swapped; natives not "
                             "bound\n");
        return false;
    }
    env->CallVoidMethod(runtime, load0, from, jstring_of(env, g_files.dll.wstring()));
    const bool threw = env->ExceptionCheck();
    if (threw) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    const bool restored = aosp_gfx::redirect_end(jvmti);
    const aosp_gfx::RedirectCounts c = aosp_gfx::redirect_counts();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr,
                 "[win.apk] AOSP graphics: %s %s in %.0f ms: natives bound %d (%d through API 34 adapters), "
                 "skipped %d, refused %d (Win.apk's own: database, Surface, ImageReader, PropertyValuesHolder; "
                 "%d classes), lookups on Win.apk's classes that found nothing %d%s\n",
                 utf8(g_files.dll.filename().wstring()).c_str(), threw ? "failed to load" : "loaded", ms,
                 c.bound + c.adapted, c.adapted, c.skipped, c.refused, c.classes_unbound, c.faked,
                 restored ? "" : "; the original JNI table could not be put back");
    return !threw;
}

}  // namespace winapk::host
