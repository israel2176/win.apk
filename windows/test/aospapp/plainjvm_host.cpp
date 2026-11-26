// What winapk-runtime.exe gives a JVM, for a plain-JVM test of AOSP's app layer (AospAppLifecycleTest,
// MagnifierTest): the host natives they reach (the log funnel, setenv; windows/host binds the real ones),
// the graphics DLL's that the native runtime lacks, as tokens, and the
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
#include <map>
#include <mutex>
#include <utility>
#include <vector>

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

// Choreographer's constructor sets the render thread's FPS divisor (HardwareRenderer.setFPSDivisor),
// a native Robolectric's runtime lacks: with no render thread here (the harnesses draw in software)
// there is no animation on it to enable or disable.
JNIEXPORT void JNICALL Java_android_graphics_HardwareRenderer_nSetRtAnimationsEnabled(JNIEnv*, jclass, jboolean) {}

// The rest of HardwareRenderer's natives that Robolectric's runtime lacks (the graphics DLL has AOSP's): a
// Magnifier's popup is drawn by a ThreadedRenderer.SimpleRenderer even when the windows draw in software
// (MagnifierTest), whose construction gives HWUI the display and the process's kind (ProcessInitializer)
// and whose frames reach the frame callbacks. Nothing to tell a render thread here.
#define HR(name) Java_android_graphics_HardwareRenderer_##name
JNIEXPORT void JNICALL HR(nInitDisplayInfo)(JNIEnv*, jclass, jint, jint, jfloat, jint, jlong, jlong, jboolean, jboolean) {}
JNIEXPORT void JNICALL HR(nSetIsLowRam)(JNIEnv*, jclass, jboolean) {}
JNIEXPORT void JNICALL HR(nSetIsSystemOrPersistent)(JNIEnv*, jclass, jboolean) {}
JNIEXPORT void JNICALL HR(setupShadersDiskCache)(JNIEnv*, jclass, jstring, jstring) {}
JNIEXPORT jboolean JNICALL HR(isWebViewOverlaysEnabled)(JNIEnv*, jclass) { return JNI_FALSE; }
JNIEXPORT void JNICALL HR(preload)(JNIEnv*, jclass) {}
JNIEXPORT void JNICALL HR(disableVsync)(JNIEnv*, jclass) {}
JNIEXPORT void JNICALL HR(nTrimCaches)(JNIEnv*, jclass, jint) {}
JNIEXPORT void JNICALL HR(nSetDrawingEnabled)(JNIEnv*, jclass, jboolean) {}
JNIEXPORT void JNICALL HR(nSetTargetSdrHdrRatio)(JNIEnv*, jclass, jlong, jfloat) {}
JNIEXPORT void JNICALL HR(nForceDrawNextFrame)(JNIEnv*, jclass, jlong) {}
JNIEXPORT void JNICALL HR(nSetFrameCommitCallback)(JNIEnv*, jclass, jlong, jobject) {}
JNIEXPORT void JNICALL HR(nNotifyCallbackPending)(JNIEnv*, jclass, jlong) {}
JNIEXPORT void JNICALL HR(nNotifyExpensiveFrame)(JNIEnv*, jclass, jlong) {}
#undef HR

// The GPU DLL's natives the window path reaches (com.winapk.aospviews.HwuiLayers: a window's layer and
// the compositor's scene; RenderNode's position updates for a SurfaceView): this harness has no DLL and
// shows nothing, so a layer is a number, with its size kept, and the last scene is kept for a test to read
// (MagnifierTest.scene, layerSize).
namespace {
std::mutex g_layers_lock;
jlong g_next_layer = 1;
std::map<jlong, std::pair<jint, jint>> g_layer_sizes;
std::vector<jlong> g_scene_layers;
std::vector<jint> g_scene_xy;
}  // namespace

JNIEXPORT jlong JNICALL Java_com_winapk_aospviews_HwuiLayers_nCreate(JNIEnv*, jclass, jint w, jint h) {
    std::lock_guard<std::mutex> hold(g_layers_lock);
    const jlong l = g_next_layer++;
    g_layer_sizes[l] = {w, h};
    return l;
}
JNIEXPORT void JNICALL Java_com_winapk_aospviews_HwuiLayers_nResize(JNIEnv*, jclass, jlong l, jint w, jint h) {
    std::lock_guard<std::mutex> hold(g_layers_lock);
    g_layer_sizes[l] = {w, h};
}
JNIEXPORT void JNICALL Java_com_winapk_aospviews_HwuiLayers_nRelease(JNIEnv*, jclass, jlong l) {
    std::lock_guard<std::mutex> hold(g_layers_lock);
    g_layer_sizes.erase(l);
}
JNIEXPORT void JNICALL Java_com_winapk_aospviews_HwuiLayers_nPostBitmap(JNIEnv*, jclass, jlong, jlong) {}
JNIEXPORT void JNICALL Java_com_winapk_aospviews_HwuiLayers_nSetScene(JNIEnv* env, jclass, jlongArray layers, jintArray xy,
                                                                     jintArray, jint, jint, jboolean) {
    std::lock_guard<std::mutex> hold(g_layers_lock);
    g_scene_layers.resize(env->GetArrayLength(layers));
    g_scene_xy.resize(env->GetArrayLength(xy));
    env->GetLongArrayRegion(layers, 0, static_cast<jsize>(g_scene_layers.size()), g_scene_layers.data());
    env->GetIntArrayRegion(xy, 0, static_cast<jsize>(g_scene_xy.size()), g_scene_xy.data());
}

// MagnifierTest's view of the above: the last scene, as {layer, x, y} triples bottom to top, and a layer's
// size ({} once released).
JNIEXPORT jlongArray JNICALL Java_MagnifierTest_scene(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> hold(g_layers_lock);
    std::vector<jlong> out;
    for (size_t i = 0; i < g_scene_layers.size(); i++) {
        out.push_back(g_scene_layers[i]);
        out.push_back(g_scene_xy[2 * i]);
        out.push_back(g_scene_xy[2 * i + 1]);
    }
    jlongArray a = env->NewLongArray(static_cast<jsize>(out.size()));
    env->SetLongArrayRegion(a, 0, static_cast<jsize>(out.size()), out.data());
    return a;
}
JNIEXPORT jintArray JNICALL Java_MagnifierTest_layerSize(JNIEnv* env, jclass, jlong l) {
    std::lock_guard<std::mutex> hold(g_layers_lock);
    auto it = g_layer_sizes.find(l);
    if (it == g_layer_sizes.end()) return env->NewIntArray(0);
    const jint wh[2] = {it->second.first, it->second.second};
    jintArray a = env->NewIntArray(2);
    env->SetIntArrayRegion(a, 0, 2, wh);
    return a;
}
JNIEXPORT void JNICALL Java_android_graphics_RenderNode_nRequestPositionUpdates(JNIEnv*, jclass, jlong, jobject) {}
JNIEXPORT jboolean JNICALL Java_android_graphics_HardwareRenderer_nIsDrawingEnabled(JNIEnv*, jclass) { return JNI_TRUE; }

// HardwareRenderer's natives that take a Surface, rebound (MagnifierTest.bindSurfaceNatives, after the
// native runtime is loaded): Robolectric's read its own Surface's native window, which Win.apk's Surface
// does not have (the graphics DLL finds a Surface's layer by its layer post: layers.cpp), and crashed the
// JVM. This harness's layers are numbers with no pixels: a renderer is given no window to draw into, and a
// copy of a window's pixels (PixelCopy, which a Magnifier asks for its content) never finishes.
namespace {
void JNICALL set_surface_none(JNIEnv*, jclass, jlong, jobject, jboolean) {}
void JNICALL copy_surface_never(JNIEnv*, jclass, jobject, jint, jint, jint, jint, jobject) {}
}  // namespace

JNIEXPORT jboolean JNICALL Java_MagnifierTest_bindSurfaceNatives(JNIEnv* env, jclass) {
    jclass c = env->FindClass("android/graphics/HardwareRenderer");
    if (!c) return JNI_FALSE;
    const JNINativeMethod m[] = {
        {const_cast<char*>("nSetSurface"), const_cast<char*>("(JLandroid/view/Surface;Z)V"),
         reinterpret_cast<void*>(set_surface_none)},
        {const_cast<char*>("nCopySurfaceInto"),
         const_cast<char*>("(Landroid/view/Surface;IIIILandroid/graphics/HardwareRenderer$CopyRequest;)V"),
         reinterpret_cast<void*>(copy_surface_never)},
    };
    return env->RegisterNatives(c, m, 2) == 0 ? JNI_TRUE : JNI_FALSE;
}

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
