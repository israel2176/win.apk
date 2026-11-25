// jni_onload.cpp -- the graphics DLL's JNI_OnLoad: what a device's zygote and libandroid_runtime set
// up for HWUI, here in one place (AOSP 14's core/jni/LayoutlibLoader.cpp does the same for
// Layoutlib): ICU's data, HWUI's own JNI registered on AOSP's classes (register_android_graphics_
// classes), the few of libandroid_runtime's natives the graphics classes call (VirtualRefBasePtr,
// Hyphenator), libcore's NativeAllocationRegistry.applyFreeFunction (ART's), and the window
// layers' natives.
//
// Read from Java's system properties, as winapk-runtime passes them: icu.data.path (ICU's common
// data file), icu.locale.default, winapk.aosp.hyphen (Android's hyphenation patterns).
#include <jni.h>
#include <log/log.h>
#include <minikin/Hyphenator.h>
#include <unicode/putil.h>
#include <unicode/udata.h>
#include <unicode/uloc.h>
#include <utils/LightRefBase.h>
#include <windows.h>

#include <algorithm>
#include <string>

extern "C" int register_android_graphics_classes(JNIEnv* env);  // hwui/apex/jni_runtime.cpp
extern "C" void init_android_graphics();

namespace winapk::hwui {
void register_layers(JNIEnv* env);  // layers.cpp
}

namespace {

std::string java_property(JNIEnv* env, const char* name) {
    jclass system = env->FindClass("java/lang/System");
    jmethodID get = env->GetStaticMethodID(system, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;");
    jstring key = env->NewStringUTF(name);
    auto value = static_cast<jstring>(env->CallStaticObjectMethod(system, get, key));
    std::string out;
    if (value) {
        const char* s = env->GetStringUTFChars(value, nullptr);
        out = s;
        env->ReleaseStringUTFChars(value, s);
        env->DeleteLocalRef(value);
    }
    env->DeleteLocalRef(key);
    env->DeleteLocalRef(system);
    return out;
}

// A read-only view of a whole file, kept for the process's life (ICU's data, hyphenation patterns).
const uint8_t* map_file(const std::string& utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
    HANDLE f = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(f);
    if (!m) return nullptr;
    void* p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(m);
    return static_cast<const uint8_t*>(p);
}

// --- android.text.Hyphenator.nInit (core/jni/android_text_Hyphenator.cpp's list, AOSP 14) ---

std::string g_hyphen_dir;

void add_hyphenator(const std::string& locale, int min_prefix, int min_suffix) {
    std::string lower = locale;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    const uint8_t* p = g_hyphen_dir.empty() ? nullptr : map_file(g_hyphen_dir + "\\hyph-" + lower + ".hyb");
    if (!p) return;  // as on a device without that language's patterns
    minikin::addHyphenator(locale, minikin::Hyphenator::loadBinary(p, min_prefix, min_suffix, locale));
}

void JNICALL hyphenator_init(JNIEnv*, jclass) {
    constexpr int kIndic = 2;
    const struct {
        const char* locale;
        int prefix, suffix;
    } kPatterns[] = {
        {"af", 1, 1}, {"am", 1, 1}, {"as", kIndic, kIndic}, {"be", 2, 2}, {"bg", 2, 2}, {"bn", kIndic, kIndic},
        {"cs", 2, 2}, {"cu", 1, 2}, {"cy", 2, 3}, {"da", 2, 2}, {"de-1901", 2, 2}, {"de-1996", 2, 2},
        {"de-CH-1901", 2, 2}, {"el", 1, 1}, {"en-GB", 2, 3}, {"en-US", 2, 3}, {"es", 2, 2}, {"et", 2, 3},
        {"eu", 2, 2}, {"fr", 2, 3}, {"ga", 2, 3}, {"gl", 2, 2}, {"gu", kIndic, kIndic}, {"hi", kIndic, kIndic},
        {"hr", 2, 2}, {"hu", 2, 2}, {"hy", 2, 2}, {"it", 2, 2}, {"ka", 1, 2}, {"kn", kIndic, kIndic},
        {"la", 2, 2}, {"lt", 2, 2}, {"lv", 2, 2}, {"ml", kIndic, kIndic}, {"mn-Cyrl", 2, 2},
        {"mr", kIndic, kIndic}, {"nb", 2, 2}, {"nl", 2, 2}, {"nn", 2, 2}, {"or", kIndic, kIndic},
        {"pa", kIndic, kIndic}, {"pl", 2, 2}, {"pt", 2, 3}, {"ru", 2, 2}, {"sk", 2, 2}, {"sl", 2, 2},
        {"sq", 2, 2}, {"sv", 1, 2}, {"ta", kIndic, kIndic}, {"te", kIndic, kIndic}, {"tk", 2, 2},
        {"uk", 2, 2}, {"und-Ethi", 1, 1},
    };
    for (const auto& p : kPatterns) add_hyphenator(p.locale, p.prefix, p.suffix);
    minikin::addHyphenator("ca", minikin::Hyphenator::loadBinary(nullptr, 2, 2, "ca"));
    const char* const kAliases[][2] = {
        {"en-AS", "en-US"}, {"en-GU", "en-US"}, {"en-MH", "en-US"}, {"en-MP", "en-US"}, {"en-PR", "en-US"},
        {"en-UM", "en-US"}, {"en-VI", "en-US"}, {"en", "en-GB"}, {"de", "de-1996"}, {"de-LI-1901", "de-CH-1901"},
        {"no", "nb"}, {"mn", "mn-Cyrl"}, {"am", "und-Ethi"}, {"byn", "und-Ethi"}, {"gez", "und-Ethi"},
        {"ti", "und-Ethi"}, {"wal", "und-Ethi"}, {"und-Beng", "bn"}, {"und-Deva", "hi"}, {"und-Gujr", "gu"},
        {"und-Guru", "pa"}, {"und-Knda", "kn"}, {"und-Mlym", "ml"}, {"und-Orya", "or"}, {"und-Taml", "ta"},
        {"und-Telu", "te"},
    };
    for (const auto& a : kAliases) minikin::addHyphenatorAlias(a[0], a[1]);
}

// --- com.android.internal.util.VirtualRefBasePtr ---

void JNICALL vrb_inc(JNIEnv*, jclass, jlong p) { reinterpret_cast<android::VirtualLightRefBase*>(p)->incStrong(nullptr); }
void JNICALL vrb_dec(JNIEnv*, jclass, jlong p) { reinterpret_cast<android::VirtualLightRefBase*>(p)->decStrong(nullptr); }

// --- libcore.util.NativeAllocationRegistry.applyFreeFunction (ART's) ---

void JNICALL apply_free(JNIEnv*, jclass, jlong f, jlong p) {
    reinterpret_cast<void (*)(void*)>(static_cast<intptr_t>(f))(reinterpret_cast<void*>(static_cast<intptr_t>(p)));
}

void register_soft(JNIEnv* env, const char* cls, const JNINativeMethod* m, int n) {
    jclass c = env->FindClass(cls);
    if (!c) {
        env->ExceptionClear();
        return;
    }
    if (env->RegisterNatives(c, m, n) != 0) {
        env->ExceptionClear();
        ALOGE("%s: natives not bound", cls);
    }
    env->DeleteLocalRef(c);
}

}  // namespace

extern "C" __declspec(dllexport) jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

    const std::string icu = java_property(env, "icu.data.path");
    if (const uint8_t* data = icu.empty() ? nullptr : map_file(icu)) {
        UErrorCode err = U_ZERO_ERROR;
        udata_setCommonData(data, &err);
        if (U_FAILURE(err)) ALOGE("ICU data %s: %s", icu.c_str(), u_errorName(err));
    } else {
        ALOGE("ICU data %s cannot be read: text will not lay out", icu.c_str());
    }
    const std::string locale = java_property(env, "icu.locale.default");
    if (!locale.empty()) {
        UErrorCode err = U_ZERO_ERROR;
        uloc_setDefault(locale.c_str(), &err);
    }
    g_hyphen_dir = java_property(env, "winapk.aosp.hyphen");

    init_android_graphics();
    if (register_android_graphics_classes(env) < 0) return JNI_ERR;

    static const JNINativeMethod kVrb[] = {
        {const_cast<char*>("nIncStrong"), const_cast<char*>("(J)V"), reinterpret_cast<void*>(vrb_inc)},
        {const_cast<char*>("nDecStrong"), const_cast<char*>("(J)V"), reinterpret_cast<void*>(vrb_dec)},
    };
    register_soft(env, "com/android/internal/util/VirtualRefBasePtr", kVrb, 2);
    static const JNINativeMethod kHyph[] = {
        {const_cast<char*>("nInit"), const_cast<char*>("()V"), reinterpret_cast<void*>(hyphenator_init)},
    };
    register_soft(env, "android/text/Hyphenator", kHyph, 1);
    static const JNINativeMethod kNar[] = {
        {const_cast<char*>("applyFreeFunction"), const_cast<char*>("(JJ)V"), reinterpret_cast<void*>(apply_free)},
    };
    register_soft(env, "libcore/util/NativeAllocationRegistry", kNar, 1);
    winapk::hwui::register_layers(env);
    return JNI_VERSION_1_6;
}
