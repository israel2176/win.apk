// gl_bridges.cpp -- android.opengl.GLES10-32, GLUtils and EGL14/15/Ext on
// Windows: upstream's own bindings, compiled unchanged.
//
// They are the stretch of runtime/src/native_methods.cpp that calls GL
// (gl_upstream.inc, cut out at build time by cmake/extract_gl_bridges.cmake)
// plus runtime/src/egl14_bridges.inc. Each binding calls ANGLE the way AOSP's
// android_opengl_GLES20.cpp calls the driver; on Windows the gl* and egl*
// names resolve to the forwarders in gl_stubs.cpp, so a machine without
// ANGLE still boots (its GL calls do nothing). The only Mac call in the
// stretch, dlsym(RTLD_DEFAULT, ...) for GLES3 entry points, becomes a lookup
// in ANGLE's DLLs.

#include "host_natives.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "gl/gl_loader.h"
#include "gl/gl_surface.h"
#include "native_window.h"

#define MACAPK_HAS_ANGLE 1

namespace {

constexpr void* RTLD_DEFAULT = nullptr;
void* dlsym(void*, const char* name) {
    if (void* p = winapk::host::gl_proc(name, winapk::host::GlLib::Gles)) return p;
    return winapk::host::gl_proc(name, winapk::host::GlLib::Egl);
}

}  // namespace

namespace macapk {
namespace {

#include "gl_upstream.inc"

// egl14_bridges.inc registers through Bind() on the Mac; here its tables are
// handed to host_natives instead (below), so Bind is never reached.
[[maybe_unused]] void Bind(JNIEnv*, const char*, const JNINativeMethod*, jint) {}

#include "egl14_bridges.inc"

}  // namespace
}  // namespace macapk

namespace winapk::host {

namespace {

// EGL14 window surfaces on a SurfaceView, as MacApkEGL10.viewWindow makes them for EGL10. Upstream's
// eglCreateWindowSurface puts a SurfaceView's Surface on the Mac's native-EGL layer, which Windows
// has no counterpart of (it answered EGL_NO_SURFACE, and a GL thread on EGL14 died there); here the
// window is the host's window surface on the SurfaceView (surface_view_window_create: a pbuffer of
// the native window's size, read back and shown under the window at each swap), the same one an
// EGL10 thread or a native engine gets. Every other window (a SurfaceTexture's) is upstream's.
jclass g_egl10 = nullptr;
jmethodID g_sv_surface = nullptr, g_sv_made = nullptr;

bool egl10_upcalls(JNIEnv* env) {
    static bool looked = false;
    if (looked) return g_sv_surface != nullptr;
    looked = true;
    jclass c = env->FindClass("javax/microedition/khronos/egl/MacApkEGL10");
    if (c) {
        g_egl10 = static_cast<jclass>(env->NewGlobalRef(c));
        g_sv_surface = env->GetStaticMethodID(c, "macapkSurfaceViewSurface", "(Ljava/lang/Object;)Landroid/view/Surface;");
        g_sv_made = env->GetStaticMethodID(c, "macapkSurfaceViewWindowMade", "(Landroid/view/Surface;)V");
        env->DeleteLocalRef(c);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!g_sv_surface || !g_sv_made) g_sv_surface = nullptr;
    return g_sv_surface != nullptr;
}

jobject JNICALL egl14_create_window_surface(JNIEnv* env, jclass cls, jobject dpy, jobject cfg, jobject win,
                                            jintArray attribs, jint off) {
    if (win && egl10_upcalls(env)) {
        jobject sur = env->CallStaticObjectMethod(g_egl10, g_sv_surface, win);
        if (env->ExceptionCheck()) return nullptr;
        if (sur) {
            void* s = surface_view_window_create(macapk::egl_h(env, dpy), macapk::egl_h(env, cfg));
            if (s) {
                env->CallStaticVoidMethod(g_egl10, g_sv_made, sur);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            env->DeleteLocalRef(sur);
            return macapk::boxSurface(env, s);
        }
    }
    return macapk::EGL14_eglCreateWindowSurface(env, cls, dpy, cfg, win, attribs, off);
}

jboolean JNICALL egl14_swap_buffers(JNIEnv* env, jclass cls, jobject dpy, jobject surf) {
    const int r = surface_view_window_swap(macapk::egl_h(env, dpy), macapk::egl_h(env, surf));
    if (r >= 0) return r == 1 ? JNI_TRUE : JNI_FALSE;
    return macapk::EGL14_eglSwapBuffers(env, cls, dpy, surf);
}

jboolean JNICALL egl14_destroy_surface(JNIEnv* env, jclass cls, jobject dpy, jobject surf) {
    surface_view_window_destroyed(macapk::egl_h(env, surf));
    return macapk::EGL14_eglDestroySurface(env, cls, dpy, surf);
}

// EGL14.eglGetDisplay(EGL_DEFAULT_DISPLAY): the app's display, the one its native code gets from
// eglGetDisplay (guest_gl.cpp) and EGL10 from MacApkEGL10 (both gl_shared_display's), as Android's
// libEGL has one default display for every API of a process. Upstream answered ANGLE's
// eglGetDisplay(EGL_DEFAULT_DISPLAY), which ANGLE keys as the graphics DLL's display (a Direct3D 11
// display of the default native display, hwui_build/glue/layers.cpp): an app's GL thread on EGL14
// then drew on the display HWUI's render thread composes on, whose contexts ANGLE does not lock
// against each other (gl_surface.cpp, open_display), and the render thread crashed in libGLESv2 as
// the GL thread made its surface (windows/test/glthread, Fork14Activity).
jobject JNICALL egl14_get_display(JNIEnv* env, jclass cls, jint id) {
    if (id == 0 /*EGL_DEFAULT_DISPLAY*/) {
        if (void* d = gl_shared_display()) {
            macapk::egl_err_reset();
            return macapk::boxDisplay(env, d);
        }
    }
    return macapk::EGL14_eglGetDisplay(env, cls, id);
}

// The app's display stays up: the framework's views draw on it too (gl_surface.cpp), and on a
// device eglTerminate is reference-counted, so the app's terminate does not reach the driver while
// another client holds the display (guest_eglTerminate answers native code the same).
jboolean JNICALL egl14_terminate(JNIEnv* env, jclass cls, jobject dpy) {
    if (macapk::egl_h(env, dpy) == gl_shared_display()) return JNI_TRUE;
    return macapk::EGL14_eglTerminate(env, cls, dpy);
}

// The EGL14 natives this file answers itself, by name.
const void* egl14_override(const char* name) {
    if (std::strcmp(name, "eglGetDisplay") == 0) return reinterpret_cast<const void*>(egl14_get_display);
    if (std::strcmp(name, "eglTerminate") == 0) return reinterpret_cast<const void*>(egl14_terminate);
    if (std::strcmp(name, "eglCreateWindowSurface") == 0) return reinterpret_cast<const void*>(egl14_create_window_surface);
    if (std::strcmp(name, "eglSwapBuffers") == 0) return reinterpret_cast<const void*>(egl14_swap_buffers);
    if (std::strcmp(name, "eglDestroySurface") == 0) return reinterpret_cast<const void*>(egl14_destroy_surface);
    return nullptr;
}

struct Table {
    const char* clazz;
    const JNINativeMethod* methods;
    size_t count;
};
#define WINAPK_GL_TABLE(clazz, table) {clazz, macapk::table, sizeof(macapk::table) / sizeof(macapk::table[0])}
const Table kTables[] = {
    WINAPK_GL_TABLE("android.opengl.GLES20", kGles20Methods),
    WINAPK_GL_TABLE("android.opengl.GLUtils", kGLUtilsMethods),
    WINAPK_GL_TABLE("android.opengl.GLES30", kGl3GLES30Methods),
    WINAPK_GL_TABLE("android.opengl.GLES31", kGl3GLES31Methods),
    WINAPK_GL_TABLE("android.opengl.GLES31Ext", kGl3GLES31ExtMethods),
    WINAPK_GL_TABLE("android.opengl.GLES32", kGl3GLES32Methods),
    WINAPK_GL_TABLE("android.opengl.GLES10", kGl1GLES10Methods),
    WINAPK_GL_TABLE("android.opengl.GLES11", kGl1GLES11Methods),
    WINAPK_GL_TABLE("android.opengl.EGL14", kEgl14Methods),
    WINAPK_GL_TABLE("android.opengl.EGL15", kEgl15Methods),
    WINAPK_GL_TABLE("android.opengl.EGLExt", kEglExtMethods),
};

}  // namespace

HostMethods native_gl_methods() {
    static const std::vector<HostMethod> all = [] {
        std::vector<HostMethod> v;
        for (const Table& t : kTables) {
            for (size_t i = 0; i < t.count; ++i) {
                JNINativeMethod m = t.methods[i];
                if (t.methods == macapk::kEgl14Methods) {
                    if (const void* own = egl14_override(m.name)) m.fnPtr = const_cast<void*>(own);
                }
                v.push_back({t.clazz, m});
            }
        }
        return v;
    }();
    return {all.data(), all.size()};
}

}  // namespace winapk::host
