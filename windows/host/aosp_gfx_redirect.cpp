// aosp_gfx_redirect.cpp -- see aosp_gfx_redirect.h.
#include "aosp_gfx_redirect.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace winapk::host::aosp_gfx {
namespace {

jniNativeInterface g_orig;  // the table before ours
bool g_installed = false;
JNIEnv* g_env = nullptr;  // the loading thread's: every other thread's calls go through as they were
std::atomic<int> g_bound{0}, g_adapted{0}, g_skipped{0}, g_refused{0}, g_unbound{0}, g_faked{0};

bool verbose() {
    const char* v = std::getenv("WINAPK_AOSP_GFX_VERBOSE");
    return v && *v && std::strcmp(v, "0") != 0;
}

// What may take the library's natives. Everything else it registers (android.database and
// android.database.sqlite, android.view.Surface, android.media.ImageReader,
// android.animation.PropertyValuesHolder) is Win.apk's own and stays so.
const char* const kAllowedPrefixes[] = {"android.graphics.", "android.text."};
const char* const kAllowedClasses[] = {
    "android.util.PathParser",
    // How android.graphics frees its native objects (applyFreeFunction), and RenderNode
    // animators' reference counting.
    "libcore.util.NativeAllocationRegistry",
    "com.android.internal.util.VirtualRefBasePtr",
};

// Library class names whose AOSP class is not "<package>/<X>" for X = name minus "Natives".
const char* const kSpecial[][2] = {
    {"FontsFontFamily", "android/graphics/fonts/FontFamily"},
    {"FontFamilyBuilder", "android/graphics/fonts/FontFamily$Builder"},
    {"FontBuilder", "android/graphics/fonts/Font$Builder"},
    {"Font", "android/graphics/fonts/Font"},
    {"FontFileUtil", "android/graphics/fonts/FontFileUtil"},
    {"MeasuredTextBuilder", "android/graphics/text/MeasuredText$Builder"},
    {"ColorSpaceRgb", "android/graphics/ColorSpace$Rgb"},
};
// Real class names the library looks up whose natives stay Win.apk's (refused below).
const char* const kKeptByWinApk[] = {
    "android/database/", "android/media/ImageReader", "android/view/Surface", "android/animation/PropertyValuesHolder",
    "android/os/OperationCanceledException",
};
// Where the rest are looked for, in order. Only allowed packages: a class of another package
// is Win.apk's (the library's CursorWindowNatives would find Win.apk's CursorWindow), and the
// library gets a stand-in for it instead (below).
const char* const kPackages[] = {
    "android/graphics/",  "android/graphics/text/", "android/graphics/drawable/", "android/graphics/animation/",
    "android/graphics/fonts/", "android/text/", "android/util/", "libcore/util/", "com/android/internal/util/",
};

// Class.forName(name, false, <system loader>): loaded and linked, not initialized; a static
// initializer could call the very natives being registered.
jclass find_noinit(JNIEnv* env, const char* slash) {
    char dots[512];
    std::snprintf(dots, sizeof dots, "%s", slash);
    for (char* q = dots; *q; q++)
        if (*q == '/') *q = '.';
    jclass cls = g_orig.FindClass(env, "java/lang/Class");
    jclass cl = g_orig.FindClass(env, "java/lang/ClassLoader");
    if (!cls || !cl) return nullptr;
    jmethodID gs = g_orig.GetStaticMethodID(env, cl, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = g_orig.CallStaticObjectMethodA(env, cl, gs, nullptr);
    jmethodID fn =
        g_orig.GetStaticMethodID(env, cls, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    jvalue args[3];
    args[0].l = g_orig.NewStringUTF(env, dots);
    args[1].z = JNI_FALSE;
    args[2].l = loader;
    jobject r = g_orig.CallStaticObjectMethodA(env, cls, fn, args);
    if (g_orig.ExceptionCheck(env)) {
        g_orig.ExceptionClear(env);
        return nullptr;
    }
    return static_cast<jclass>(r);
}

bool allowed_slash(const char* slash) {
    char dots[512];
    std::snprintf(dots, sizeof dots, "%s", slash);
    for (char* q = dots; *q; q++)
        if (*q == '/') *q = '.';
    return allowed_class(dots);
}

// The class a refused registration lands on: the library then registers on it, and
// RegisterNatives (below) drops every method. java.lang.Void: a class the library never asks for
// by name, so nothing it registers lands there by chance.
jclass stand_in(JNIEnv* env) { return g_orig.FindClass(env, "java/lang/Void"); }

bool is_stand_in(JNIEnv* env, jclass c) {
    jclass v = stand_in(env);
    const bool same = v && g_orig.IsSameObject(env, v, c);
    if (v) g_orig.DeleteLocalRef(env, v);
    return same;
}

// The class's binary name, into buf.
void class_name(JNIEnv* env, jclass c, char* buf, size_t n) {
    buf[0] = '\0';
    jclass k = g_orig.GetObjectClass(env, c);
    jmethodID gn = k ? g_orig.GetMethodID(env, k, "getName", "()Ljava/lang/String;") : nullptr;
    jstring s = gn ? static_cast<jstring>(g_orig.CallObjectMethodA(env, c, gn, nullptr)) : nullptr;
    if (g_orig.ExceptionCheck(env)) g_orig.ExceptionClear(env);
    if (!s) return;
    if (const char* u = g_orig.GetStringUTFChars(env, s, nullptr)) {
        std::snprintf(buf, n, "%s", u);
        g_orig.ReleaseStringUTFChars(env, s, u);
    }
    g_orig.DeleteLocalRef(env, s);
}

jclass JNICALL my_FindClass(JNIEnv* env, const char* name) {
    if (env != g_env) return g_orig.FindClass(env, name);
    static const char pre[] = "org/robolectric/nativeruntime/";
    const size_t pl = sizeof pre - 1, nl = std::strlen(name);
    if (std::strncmp(name, pre, pl) == 0 && nl > pl + 7 && std::strcmp(name + nl - 7, "Natives") == 0) {
        char simple[256], buf[512];
        std::snprintf(simple, sizeof simple, "%.*s", static_cast<int>(nl - pl - 7), name + pl);
        for (const auto& s : kSpecial)
            if (std::strcmp(simple, s[0]) == 0) {
                if (jclass c = find_noinit(env, s[1])) return c;
            }
        for (const char* p : kPackages) {
            std::snprintf(buf, sizeof buf, "%s%s", p, simple);
            if (!allowed_slash(buf)) continue;
            if (jclass c = find_noinit(env, buf)) return c;
        }
        // Not ours to bind (CursorWindow, SQLiteConnection, Surface, ImageReader,
        // PropertyValuesHolder...), or a class aosp-graphics.jar lacks: the library aborts
        // the process when FindClass fails, so it gets the stand-in and binds nothing.
        g_unbound++;
        if (verbose()) std::fprintf(stderr, "[win.apk] aosp-gfx: %s binds nothing\n", name);
        return stand_in(env);
    }
    // The classes whose natives are Win.apk's, by their own names: the library looks their
    // fields and methods up as it registers (GetFieldIDOrDie: abort when one is missing), and a
    // method lookup would run Win.apk's static initializer before Boot. The stand-in instead,
    // whose lookups (below) answer an ID nothing uses.
    for (const char* p : kKeptByWinApk)
        if (std::strncmp(name, p, std::strlen(p)) == 0) return stand_in(env);
    jclass r = g_orig.FindClass(env, name);
    if (!r && g_orig.ExceptionCheck(env) && !allowed_slash(name)) {
        // A class of Win.apk's framework the library wants and it lacks: an ID nothing
        // registered here may use without crashing, but the load goes on.
        g_orig.ExceptionClear(env);
        g_faked++;
        std::fprintf(stderr, "[win.apk] aosp-gfx: no %s in Win.apk's framework\n", name);
        return stand_in(env);
    }
    return r;
}

// A field or method ID for a lookup that must not fail and is never used: on the stand-in (the
// natives registered on it were dropped), or a member Win.apk's own class lacks. Distinct per
// call is not needed; non-null is.
char g_fake_id;

template <typename Id>
Id fake_id() { return reinterpret_cast<Id>(&g_fake_id); }

// Lookups on the stand-in, and on Win.apk's classes the library reaches from an allowed class
// (HardwareRenderer reads android.view.Surface's mNativeObject): Win.apk's class may not have the
// member. An allowed (AOSP) class keeps the lookup as it is: a miss there is a real mismatch.
template <typename Id, Id(JNICALL* jniNativeInterface::*Fn)(JNIEnv*, jclass, const char*, const char*)>
Id JNICALL lookup(JNIEnv* env, jclass c, const char* name, const char* sig) {
    if (env != g_env) return (g_orig.*Fn)(env, c, name, sig);
    if (is_stand_in(env, c)) return fake_id<Id>();
    Id id = (g_orig.*Fn)(env, c, name, sig);
    if (id || !g_orig.ExceptionCheck(env)) return id;
    char cname[512];
    class_name(env, c, cname, sizeof cname);
    if (allowed_class(cname)) return id;
    g_orig.ExceptionClear(env);
    g_faked++;
    std::fprintf(stderr, "[win.apk] aosp-gfx: Win.apk's %s has no %s %s\n", cname, name, sig);
    return fake_id<Id>();
}

// API 34 changed four signatures against the ones the library carries (it was built for an older
// AOSP). Each adapter takes API 34's arguments and forwards the ones the library knows; what it
// drops is new in 34 and has a default the library already uses.
using nbuild4_t = jlong(JNICALL*)(JNIEnv*, jclass, jlong, jstring, jint, jboolean);
nbuild4_t g_nbuild4;
// FontFamily.Builder.nBuild: + isDefaultFallback (only the system's own fallback list sets it).
jlong JNICALL nbuild5(JNIEnv* e, jclass c, jlong b, jstring l, jint v, jboolean custom, jboolean) {
    return g_nbuild4(e, c, b, l, v, custom);
}
using bshader5_t = jlong(JNICALL*)(JNIEnv*, jclass, jlong, jlong, jint, jint, jboolean);
bshader5_t g_bshader5;
// BitmapShader.nativeCreate: + isGainmapEnabled (HDR gainmaps: none here).
jlong JNICALL bshader6(JNIEnv* e, jclass c, jlong m, jlong bmp, jint tx, jint ty, jboolean filter, jboolean) {
    return g_bshader5(e, c, m, bmp, tx, ty, filter);
}
using styler5_t = void(JNICALL*)(JNIEnv*, jclass, jlong, jlong, jint, jint, jboolean);
styler5_t g_styler5;
// MeasuredText.Builder.nAddStyleRun: + lineBreakStyle, lineBreakWordStyle (LineBreakConfig's
// strict/loose and phrase breaking for CJK; the library breaks as "none", Android's default).
void JNICALL styler7(JNIEnv* e, jclass c, jlong b, jlong paint, jint, jint, jint start, jint end, jboolean rtl) {
    g_styler5(e, c, b, paint, start, end, rtl);
}
using mtbuild5_t = jlong(JNICALL*)(JNIEnv*, jclass, jlong, jlong, jcharArray, jboolean, jboolean);
mtbuild5_t g_mtbuild5;
// MeasuredText.Builder.nBuildMeasuredText: + fastHyphenationMode (a speed hint).
jlong JNICALL mtbuild6(JNIEnv* e, jclass c, jlong b, jlong hint, jcharArray text, jboolean hyph, jboolean layout,
                       jboolean) {
    return g_mtbuild5(e, c, b, hint, text, hyph, layout);
}

struct Adapter {
    const char *name, *lib_sig, *aosp_sig;
    void** lib_fn;
    void* fn;
};
const Adapter kAdapters[] = {
    {"nBuild", "(JLjava/lang/String;IZ)J", "(JLjava/lang/String;IZZ)J", reinterpret_cast<void**>(&g_nbuild4),
     reinterpret_cast<void*>(&nbuild5)},
    {"nativeCreate", "(JJIIZ)J", "(JJIIZZ)J", reinterpret_cast<void**>(&g_bshader5),
     reinterpret_cast<void*>(&bshader6)},
    {"nAddStyleRun", "(JJIIZ)V", "(JJIIIIZ)V", reinterpret_cast<void**>(&g_styler5),
     reinterpret_cast<void*>(&styler7)},
    {"nBuildMeasuredText", "(JJ[CZZ)J", "(JJ[CZZZ)J", reinterpret_cast<void**>(&g_mtbuild5),
     reinterpret_cast<void*>(&mtbuild6)},
};

jint JNICALL my_RegisterNatives(JNIEnv* env, jclass c, const JNINativeMethod* m, jint n) {
    if (env != g_env) return g_orig.RegisterNatives(env, c, m, n);
    if (is_stand_in(env, c)) {
        g_refused += n;
        return JNI_OK;  // as if bound: the library aborts the process on an error
    }
    char cname[512];
    class_name(env, c, cname, sizeof cname);
    // Any other registration on this thread meanwhile (the JDK's own Perf, as a jar opens) as it was.
    if (!allowed_class(cname)) return g_orig.RegisterNatives(env, c, m, n);
    for (jint i = 0; i < n; i++) {
        if (g_orig.RegisterNatives(env, c, &m[i], 1) == JNI_OK) {
            g_bound++;
            continue;
        }
        g_orig.ExceptionClear(env);
        JNINativeMethod a{};
        for (const Adapter& ad : kAdapters) {
            if (!std::strcmp(m[i].name, ad.name) && !std::strcmp(m[i].signature, ad.lib_sig)) {
                *ad.lib_fn = m[i].fnPtr;
                a.name = const_cast<char*>(ad.name);
                a.signature = const_cast<char*>(ad.aosp_sig);
                a.fnPtr = ad.fn;
                break;
            }
        }
        if (a.name && g_orig.RegisterNatives(env, c, &a, 1) == JNI_OK) {
            g_adapted++;
            continue;
        }
        g_orig.ExceptionClear(env);
        g_skipped++;
        if (verbose()) std::fprintf(stderr, "[win.apk] aosp-gfx: skip %s.%s%s\n", cname, m[i].name, m[i].signature);
    }
    return JNI_OK;
}

}  // namespace

bool allowed_class(const char* name) {
    if (!name) return false;
    for (const char* p : kAllowedPrefixes)
        if (std::strncmp(name, p, std::strlen(p)) == 0) return true;
    for (const char* c : kAllowedClasses)
        if (std::strcmp(name, c) == 0) return true;
    return false;
}

bool redirect_begin(jvmtiEnv* jvmti, JNIEnv* env) {
    if (g_installed) return true;
    g_env = env;
    jniNativeInterface* tab = nullptr;
    if (jvmti->GetJNIFunctionTable(&tab) != JVMTI_ERROR_NONE || !tab) return false;
    std::memcpy(&g_orig, tab, sizeof g_orig);
    tab->FindClass = my_FindClass;
    tab->RegisterNatives = my_RegisterNatives;
    tab->GetFieldID = lookup<jfieldID, &jniNativeInterface::GetFieldID>;
    tab->GetStaticFieldID = lookup<jfieldID, &jniNativeInterface::GetStaticFieldID>;
    tab->GetMethodID = lookup<jmethodID, &jniNativeInterface::GetMethodID>;
    tab->GetStaticMethodID = lookup<jmethodID, &jniNativeInterface::GetStaticMethodID>;
    const jvmtiError err = jvmti->SetJNIFunctionTable(tab);
    jvmti->Deallocate(reinterpret_cast<unsigned char*>(tab));
    if (err != JVMTI_ERROR_NONE) return false;
    g_installed = true;
    return true;
}

bool redirect_end(jvmtiEnv* jvmti) {
    if (!g_installed) return true;
    if (jvmti->SetJNIFunctionTable(&g_orig) != JVMTI_ERROR_NONE) return false;
    g_installed = false;
    return true;
}

RedirectCounts redirect_counts() {
    RedirectCounts r;
    r.bound = g_bound;
    r.adapted = g_adapted;
    r.skipped = g_skipped;
    r.refused = g_refused;
    r.classes_unbound = g_unbound;
    r.faked = g_faked;
    return r;
}

}  // namespace winapk::host::aosp_gfx
