// redirect_agent -- binds Robolectric's native runtime (robolectric-nativeruntime.dll / .so: AOSP's
// libhwui, Skia, minikin, HarfBuzz, FreeType, ICU, built for the host) straight onto AOSP's own
// android.graphics / android.text classes, as android-all ships them.
//
// The library's JNI_OnLoad registers its natives with RegisterMethodsOrDie on Robolectric's
// "org/robolectric/nativeruntime/<X>Natives" classes (it rewrites "android/graphics/<X>" to that
// name). We are not Robolectric: there are no *Natives classes. So, for as long as the library
// loads, the JNI function table's FindClass maps such a name back to the AOSP class (found without
// running its static initializer, which would need the natives being registered), and
// RegisterNatives registers one method at a time, skipping a native the AOSP class lacks (a part
// AOSP has moved or dropped, or the variant of another API level the library also carries) instead
// of failing the whole class, and adapting the four signatures API 34 changed against the library's.
//
// Two ways in, one code path:
//   System.load(<this library>)  -- JNI_OnLoad takes a JVMTI environment in the live phase and
//                                  swaps the table at once (what Win.apk's runtime would do: it
//                                  owns the JavaVM);
//   -agentpath:<this library>    -- Agent_OnLoad swaps it at VMInit (a plain `java` run).
// Then System.load(<native runtime>) does the rest; AospGfx.agentRestore (below) puts the original
// table back.
//
// Portable C for Linux (gcc/clang) and Windows (clang, MSYS2 CLANG64); JNIEXPORT/JNICALL come from
// the JDK's jni_md.h. Set AGENT_VERBOSE=1 for each skip and miss, AGENT_REGLOG=<file> for a list
// of every method bound.
#include <jvmti.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jniNativeInterface *orig;  // a copy of the table before ours
static int installed;
static int ok_count, skip_count, adapted_count, class_miss_count;

static int verbose(void) {
    const char *v = getenv("AGENT_VERBOSE");
    return v && *v && strcmp(v, "0") != 0;
}

// Library class names whose AOSP class is not "<package>/<X>" for X = name minus "Natives".
static const char *special[][2] = {
    {"FontsFontFamily", "android/graphics/fonts/FontFamily"},
    {"FontFamilyBuilder", "android/graphics/fonts/FontFamily$Builder"},
    {"FontBuilder", "android/graphics/fonts/Font$Builder"},
    {"Font", "android/graphics/fonts/Font"},
    {"FontFileUtil", "android/graphics/fonts/FontFileUtil"},
    {"MeasuredTextBuilder", "android/graphics/text/MeasuredText$Builder"},
    {"ColorSpaceRgb", "android/graphics/ColorSpace$Rgb"},
    {"ImageReaderSurfaceImage", "android/media/ImageReader$SurfaceImage"},
    {NULL, NULL}};
static const char *pkgs[] = {"android/graphics/", "android/graphics/text/", "android/graphics/drawable/",
                             "android/graphics/animation/", "android/database/", "android/database/sqlite/",
                             "android/animation/", "android/text/", "android/view/", "android/util/",
                             "android/media/", "libcore/util/", "com/android/internal/util/", "android/os/", NULL};

// Class.forName(name, false, <system loader>): loaded and linked, not initialized.
static jclass find_noinit(JNIEnv *env, const char *slash) {
    char dots[512];
    snprintf(dots, sizeof dots, "%s", slash);
    for (char *q = dots; *q; q++)
        if (*q == '/') *q = '.';
    jclass cls = orig->FindClass(env, "java/lang/Class");
    jclass cl = orig->FindClass(env, "java/lang/ClassLoader");
    if (!cls || !cl) return NULL;
    jmethodID gs = orig->GetStaticMethodID(env, cl, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = orig->CallStaticObjectMethodA(env, cl, gs, NULL);
    jmethodID fn = orig->GetStaticMethodID(env, cls, "forName",
                                           "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    jvalue args[3];
    args[0].l = orig->NewStringUTF(env, dots);
    args[1].z = JNI_FALSE;
    args[2].l = loader;
    jobject r = orig->CallStaticObjectMethodA(env, cls, fn, args);
    if (orig->ExceptionCheck(env)) {
        orig->ExceptionClear(env);
        return NULL;
    }
    return (jclass)r;
}

static jclass JNICALL my_FindClass(JNIEnv *env, const char *name) {
    static const char pre[] = "org/robolectric/nativeruntime/";
    const size_t pl = sizeof pre - 1, nl = strlen(name);
    if (strncmp(name, pre, pl) == 0 && nl > pl + 7 && strcmp(name + nl - 7, "Natives") == 0) {
        char simple[256], buf[512];
        snprintf(simple, sizeof simple, "%.*s", (int)(nl - pl - 7), name + pl);
        for (int i = 0; special[i][0]; i++)
            if (strcmp(simple, special[i][0]) == 0) {
                jclass c = find_noinit(env, special[i][1]);
                if (c) return c;
            }
        for (int i = 0; pkgs[i]; i++) {
            snprintf(buf, sizeof buf, "%s%s", pkgs[i], simple);
            jclass c = find_noinit(env, buf);
            if (c) return c;
        }
        class_miss_count++;
        if (verbose()) fprintf(stderr, "[agent] no AOSP class for %s\n", name);
        // Fall through: FindClass fails with NoClassDefFoundError and RegisterMethodsOrDie aborts,
        // so a class the library needs and android-all lacks shows up as that name, not a guess.
    }
    return orig->FindClass(env, name);
}

// API 34 changed four signatures against the ones the library carries (it was built for an older
// AOSP). Each adapter takes API 34's arguments and forwards the ones the library knows; what it
// drops is new in 34 and has a default the library already uses.
typedef jlong(JNICALL *nbuild4_t)(JNIEnv *, jclass, jlong, jstring, jint, jboolean);
static nbuild4_t nbuild4;
// FontFamily.Builder.nBuild: + isDefaultFallback (only the system's own fallback list sets it).
static jlong JNICALL nbuild5(JNIEnv *e, jclass c, jlong b, jstring l, jint v, jboolean custom, jboolean dflt) {
    (void)dflt;
    return nbuild4(e, c, b, l, v, custom);
}
typedef jlong(JNICALL *bshader5_t)(JNIEnv *, jclass, jlong, jlong, jint, jint, jboolean);
static bshader5_t bshader5;
// BitmapShader.nativeCreate: + isGainmapEnabled (HDR gainmaps: none here).
static jlong JNICALL bshader6(JNIEnv *e, jclass c, jlong m, jlong bmp, jint tx, jint ty, jboolean filter,
                              jboolean gainmap) {
    (void)gainmap;
    return bshader5(e, c, m, bmp, tx, ty, filter);
}
typedef void(JNICALL *styler5_t)(JNIEnv *, jclass, jlong, jlong, jint, jint, jboolean);
static styler5_t styler5;
// MeasuredText.Builder.nAddStyleRun: + lineBreakStyle, lineBreakWordStyle (LineBreakConfig's
// strict/loose and phrase breaking for CJK; the library breaks as "none", Android's default).
static void JNICALL styler7(JNIEnv *e, jclass c, jlong b, jlong paint, jint lbStyle, jint lbWordStyle, jint start,
                            jint end, jboolean rtl) {
    (void)lbStyle;
    (void)lbWordStyle;
    styler5(e, c, b, paint, start, end, rtl);
}
typedef jlong(JNICALL *mtbuild5_t)(JNIEnv *, jclass, jlong, jlong, jcharArray, jboolean, jboolean);
static mtbuild5_t mtbuild5;
// MeasuredText.Builder.nBuildMeasuredText: + fastHyphenationMode (a speed hint).
static jlong JNICALL mtbuild6(JNIEnv *e, jclass c, jlong b, jlong hint, jcharArray text, jboolean hyph,
                              jboolean layout, jboolean fastHyph) {
    (void)fastHyph;
    return mtbuild5(e, c, b, hint, text, hyph, layout);
}

static const struct adapter {
    const char *name, *lib_sig, *aosp_sig;
    void **lib_fn;
    void *fn;
} adapters[] = {
    {"nBuild", "(JLjava/lang/String;IZ)J", "(JLjava/lang/String;IZZ)J", (void **)&nbuild4, (void *)nbuild5},
    {"nativeCreate", "(JJIIZ)J", "(JJIIZZ)J", (void **)&bshader5, (void *)bshader6},
    {"nAddStyleRun", "(JJIIZ)V", "(JJIIIIZ)V", (void **)&styler5, (void *)styler7},
    {"nBuildMeasuredText", "(JJ[CZZ)J", "(JJ[CZZZ)J", (void **)&mtbuild5, (void *)mtbuild6},
    {NULL, NULL, NULL, NULL, NULL}};

static FILE *reglog(void) {
    static FILE *log;
    static int tried;
    if (!tried) {
        tried = 1;
        const char *p = getenv("AGENT_REGLOG");
        if (p && *p) log = fopen(p, "w");
    }
    return log;
}

static void log_bound(JNIEnv *env, jclass c, const JNINativeMethod *m) {
    FILE *log = reglog();
    if (!log) return;
    jclass k = orig->GetObjectClass(env, c);
    jmethodID gn = orig->GetMethodID(env, k, "getName", "()Ljava/lang/String;");
    jstring s = (jstring)orig->CallObjectMethodA(env, c, gn, NULL);
    const char *u = s ? orig->GetStringUTFChars(env, s, NULL) : NULL;
    fprintf(log, "%s %s%s\n", u ? u : "?", m->name, m->signature);
    if (u) orig->ReleaseStringUTFChars(env, s, u);
}

static jint JNICALL my_RegisterNatives(JNIEnv *env, jclass c, const JNINativeMethod *m, jint n) {
    for (jint i = 0; i < n; i++) {
        if (orig->RegisterNatives(env, c, &m[i], 1) == 0) {
            ok_count++;
            log_bound(env, c, &m[i]);
            continue;
        }
        orig->ExceptionClear(env);
        JNINativeMethod a;
        memset(&a, 0, sizeof a);
        for (const struct adapter *ad = adapters; ad->name; ad++) {
            if (!strcmp(m[i].name, ad->name) && !strcmp(m[i].signature, ad->lib_sig)) {
                *ad->lib_fn = m[i].fnPtr;
                a.name = (char *)ad->name;
                a.signature = (char *)ad->aosp_sig;
                a.fnPtr = ad->fn;
                break;
            }
        }
        if (a.name && orig->RegisterNatives(env, c, &a, 1) == 0) {
            adapted_count++;
            log_bound(env, c, &a);
            continue;
        }
        orig->ExceptionClear(env);
        skip_count++;
        if (verbose()) fprintf(stderr, "[agent] skip %s%s\n", m[i].name, m[i].signature);
    }
    return 0;
}

static int install(jvmtiEnv *jvmti) {
    if (installed) return 1;
    jniNativeInterface *tab = NULL;
    if ((*jvmti)->GetJNIFunctionTable(jvmti, &tab) != JVMTI_ERROR_NONE || !tab) {
        fprintf(stderr, "[agent] GetJNIFunctionTable failed\n");
        return 0;
    }
    orig = (jniNativeInterface *)malloc(sizeof *orig);
    memcpy(orig, tab, sizeof *orig);
    tab->FindClass = my_FindClass;
    tab->RegisterNatives = my_RegisterNatives;
    jvmtiError err = (*jvmti)->SetJNIFunctionTable(jvmti, tab);
    (*jvmti)->Deallocate(jvmti, (unsigned char *)tab);
    if (err != JVMTI_ERROR_NONE) {
        fprintf(stderr, "[agent] SetJNIFunctionTable failed: %d\n", (int)err);
        return 0;
    }
    installed = 1;
    return 1;
}

static jvmtiEnv *g_jvmti;

static void JNICALL vm_init(jvmtiEnv *jvmti, JNIEnv *env, jthread t) {
    (void)env;
    (void)t;
    install(jvmti);
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *vm, char *opts, void *reserved) {
    (void)opts;
    (void)reserved;
    if ((*vm)->GetEnv(vm, (void **)&g_jvmti, JVMTI_VERSION_1_2) != JNI_OK) return JNI_ERR;
    jvmtiEventCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.VMInit = vm_init;
    (*g_jvmti)->SetEventCallbacks(g_jvmti, &cb, sizeof cb);
    (*g_jvmti)->SetEventNotificationMode(g_jvmti, JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, NULL);
    return JNI_OK;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    if (!g_jvmti && (*vm)->GetEnv(vm, (void **)&g_jvmti, JVMTI_VERSION_1_2) != JNI_OK) {
        fprintf(stderr, "[agent] no JVMTI environment in the live phase\n");
        return JNI_ERR;
    }
    if (!install(g_jvmti)) return JNI_ERR;
    return JNI_VERSION_1_8;
}

// AospGfx's natives (the test class): how the redirect went, and the original table back once the
// native runtime has loaded, so later JNI calls pay nothing for it.
JNIEXPORT jintArray JNICALL Java_AospGfx_agentCounts(JNIEnv *env, jclass c) {
    (void)c;
    jint v[5] = {installed, ok_count, adapted_count, skip_count, class_miss_count};
    jintArray a = (*env)->NewIntArray(env, 5);
    if (a) (*env)->SetIntArrayRegion(env, a, 0, 5, v);
    return a;
}

JNIEXPORT jboolean JNICALL Java_AospGfx_agentRestore(JNIEnv *env, jclass c) {
    (void)env;
    (void)c;
    if (!installed || !g_jvmti) return JNI_FALSE;
    if ((*g_jvmti)->SetJNIFunctionTable(g_jvmti, orig) != JVMTI_ERROR_NONE) return JNI_FALSE;
    installed = 0;
    return JNI_TRUE;
}

// dalvik.system.VMRuntime (the stand-in in src/): a direct buffer's address, so the stand-in
// needs no --add-exports for sun.nio.ch.DirectBuffer.
JNIEXPORT jlong JNICALL Java_dalvik_system_VMRuntime_bufferAddress(JNIEnv *env, jclass c, jobject buf) {
    (void)c;
    return (jlong)(intptr_t)(*env)->GetDirectBufferAddress(env, buf);
}
