// gl_surface.cpp -- see gl_surface.h.

#include "gl/gl_surface.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "gl/gl_loader.h"

namespace winapk::host {

namespace {

// EGL_ANGLE_platform_angle and its D3D / Vulkan / WARP / SwiftShader values.
constexpr EGLint kPlatformAngle = 0x3202, kPlatformType = 0x3203, kTypeD3D11 = 0x3208, kTypeVulkan = 0x3450,
                 kDeviceType = 0x3209, kDeviceWarp = 0x320B, kDeviceSwiftShader = 0x3487;
// EGL_ANGLE_platform_angle_d3d's native display for "Direct3D 11, never 9" (EGL_D3D11_ONLY_DISPLAY_ANGLE).
const EGLNativeDisplayType kD3D11OnlyDisplay = reinterpret_cast<EGLNativeDisplayType>(-3);

EGLDisplay g_display = EGL_NO_DISPLAY;
bool g_ready = false;
bool g_tried = false;
std::mutex g_init_lock;

EGLDisplay open_display(EGLint type, EGLint device) {
    using GetPlatformDisplay = EGLDisplay(EGLAPIENTRY*)(EGLenum, void*, const EGLint*);
    auto get = reinterpret_cast<GetPlatformDisplay>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!get) return EGL_NO_DISPLAY;
    // A display of the app's GL's own, not the graphics DLL's (hwui_build/glue/layers.cpp), on which
    // HWUI's render thread draws every window: ANGLE keeps one Direct3D 11 device with one immediate
    // context per display and takes no lock around the GL calls of contexts that share nothing, so
    // an app's GL thread drawing on the render thread's display corrupted ANGLE's state and crashed
    // it. On a phone the app's EGL contexts and HWUI's are clients of a driver that is safe across
    // threads; here each gets its own device. ANGLE hands out one display per native display,
    // platform type, device and power preference: the native display that says "Direct3D 11 only",
    // which a Direct3D 11 display is anyway, makes this one another than the DLL's
    // (EGL_DEFAULT_DISPLAY). Nothing crosses between the two: the app's frames reach the compositor
    // as pixels (gl_read_current).
    EGLint attribs[] = {kPlatformType, type, EGL_NONE, EGL_NONE, EGL_NONE};
    if (device) {
        attribs[2] = kDeviceType;
        attribs[3] = device;
    }
    EGLDisplay d = get(kPlatformAngle, type == kTypeD3D11 ? kD3D11OnlyDisplay : EGL_DEFAULT_DISPLAY, attribs);
    if (d == EGL_NO_DISPLAY) return d;
    EGLint major = 0, minor = 0;
    if (!eglInitialize(d, &major, &minor)) return EGL_NO_DISPLAY;
    return d;
}

bool init() {
    std::lock_guard<std::mutex> hold(g_init_lock);
    if (g_tried) return g_ready;
    g_tried = true;
    if (!gl_available()) {
        std::fprintf(stderr, "[win.apk] GL: ANGLE (libEGL.dll, libGLESv2.dll) is not next to the runtime\n");
        return false;
    }
    // A GPU first, then Direct3D's software rasterizer (WARP, part of every
    // Windows), then ANGLE's own (SwiftShader).
    const char* used = "Direct3D 11";
    g_display = open_display(kTypeD3D11, 0);
    if (g_display == EGL_NO_DISPLAY) {
        g_display = open_display(kTypeD3D11, kDeviceWarp);
        used = "Direct3D 11 WARP (software)";
    }
    if (g_display == EGL_NO_DISPLAY) {
        g_display = open_display(kTypeVulkan, kDeviceSwiftShader);
        used = "SwiftShader (software Vulkan)";
    }
    if (g_display == EGL_NO_DISPLAY) {
        std::fprintf(stderr, "[win.apk] GL: ANGLE found no usable backend\n");
        return false;
    }
    const EGLint config_attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                     EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                     EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig(g_display, config_attribs, &config, 1, &n) || n < 1) {
        std::fprintf(stderr, "[win.apk] GL: no RGBA8888/D24S8 config\n");
        return false;
    }
    // Whether the backend makes a context, and which version: ES 3 when it has it (a superset an ES 2
    // app runs on), else ES 2. The context is only that answer: the display is the app's alone, and a
    // context of the host's made current on it would draw beside the app's threads (native_cocoa.cpp,
    // gl_tick), so it goes before the app is handed the display.
    for (EGLint version : {3, 2}) {
        const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE};
        const EGLContext probe = eglCreateContext(g_display, config, EGL_NO_CONTEXT, ctx_attribs);
        if (probe != EGL_NO_CONTEXT) {
            eglDestroyContext(g_display, probe);
            std::fprintf(stderr, "[win.apk] GL: ANGLE on %s, OpenGL ES %d (%s)\n", used, version,
                         reinterpret_cast<const char*>(eglQueryString(g_display, EGL_VERSION)));
            g_ready = true;
            return true;
        }
    }
    std::fprintf(stderr, "[win.apk] GL: eglCreateContext failed (0x%x)\n", eglGetError());
    return false;
}

// GL rows run bottom-up; the RGBA bytes of `rgba` (tightly packed, W x H)
// become top-row-first ARGB (opaque: nothing is behind a GL frame in the
// window). `rgba` is a glReadPixels result in client memory or a mapped pixel
// pack buffer; both are 4-byte aligned and unpadded.
void argb_from_rgba(const uint32_t* rgba, uint32_t* argb, int W, int H) {
    for (int y = 0; y < H; ++y) {
        const uint32_t* src = rgba + static_cast<size_t>(H - 1 - y) * W;
        uint32_t* dst = argb + static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x) {
            const uint32_t p = src[x];  // bytes R, G, B, A
            dst[x] = 0xff000000u | ((p & 0xff) << 16) | (p & 0xff00) | ((p >> 16) & 0xff);
        }
    }
}

}  // namespace

void gl_read_current(std::vector<uint32_t>* argb, int W, int H) {
    thread_local std::vector<uint32_t> rgba;
    rgba.resize(static_cast<size_t>(W) * H);
    // Whatever the app left bound stays its own: the frame is FBO 0, read
    // into client memory (no pixel pack buffer, ES 3 only) with no row padding.
    EGLint version = 2;
    eglQueryContext(eglGetCurrentDisplay(), eglGetCurrentContext(), EGL_CONTEXT_CLIENT_VERSION, &version);
    GLint fbo = 0, align = 4, pbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    if (version >= 3) glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    if (fbo) glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (pbo) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (align != 4) glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    if (align != 4) glPixelStorei(GL_PACK_ALIGNMENT, align);
    if (pbo) glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pbo));
    if (fbo) glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fbo));
    argb->resize(rgba.size());
    argb_from_rgba(rgba.data(), argb->data(), W, H);
}

void* gl_shared_display() { return init() ? g_display : nullptr; }

}  // namespace winapk::host
