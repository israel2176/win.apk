// layers.cpp -- the app's windows as layers, and their compositor (Win.apk's SurfaceFlinger).
//
// Every Android window (AospWindows' window, its SurfaceControl and Surface) has a Layer here: an
// ANativeWindow HWUI renders into (ANativeWindow_fromSurface gives it to ThreadedRenderer) and,
// for a window that draws in software (an activity with hardwareAccelerated="false"), the pixels
// it posts from Surface.unlockCanvasAndPost. HWUI's EglManager makes each window's EGL surface a
// pbuffer of the window's size (createWindowSurface); when a window swaps, the compositor runs on
// the render thread, in HWUI's own EGL context: each layer, bottom to top, at its place in the
// display (Java's Compositor gives the order, the places and the dim behind a dialog), the pbuffer
// bound as a texture (eglBindTexImage) and drawn into the host window's EGL window surface, then
// one swap. Headless, the display is a pbuffer and is read back only when the host asks.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/native_window.h>
#include <apex/window.h>
#include <hwui/Bitmap.h>
#include <jni.h>
#include <log/log.h>
#include <renderthread/RenderThread.h>
#include <DeviceInfo.h>
#include <system/window.h>
#include <utils/Timers.h>
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdarg>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "GrDirectContext.h"
#include "hwui_host_api.h"
#include "window_surface.h"

using android::uirenderer::renderthread::RenderThread;

namespace winapk::hwui {
void set_refresh_rate(double hz);  // choreographer.cpp
void attach_blob_cache(EGLDisplay display);  // blob_cache.cpp
}

namespace {

struct Layer;
void layer_unref(Layer* l);

// The process's memory now (Windows' counters, as Task Manager shows them), in the run's log.
void log_memory_now(const char* when) {
    PROCESS_MEMORY_COUNTERS_EX m{};
    m.cb = sizeof m;
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof m)) return;
    ALOGW("graphics memory, %s: working set %.1f MB, private %.1f MB", when, m.WorkingSetSize / 1048576.0,
          m.PrivateUsage / 1048576.0);
}
bool g_logged_first = false;

// A window's layer. ANativeWindow first: HWUI holds it as one (sp<ANativeWindow>, refcounted
// through common.incRef/decRef).
struct Layer : ANativeWindow {
    std::atomic<int> refs{1};
    std::atomic<int> width{0}, height{0};  // the window's size, px (its SurfaceControl's)
    std::atomic<int32_t> dataspace{ADATASPACE_SRGB};  // a window's buffers are sRGB
    // Render thread only.
    EGLSurface pbuffer = EGL_NO_SURFACE;
    int pb_w = 0, pb_h = 0;
    GLuint tex = 0;  // the pbuffer's texture name, bound with eglBindTexImage while drawn
    // A software frame (unlockCanvasAndPost), under sw_lock.
    std::mutex sw_lock;
    std::vector<uint32_t> sw_pixels;
    int sw_w = 0, sw_h = 0;
    bool sw_bgra = true;
    bool sw_dirty = false;
    GLuint sw_tex = 0;
    // Which of the two the window last posted (render thread reads, any thread writes).
    std::atomic<bool> software{false};
    std::atomic<bool> has_frame{false};
};

Layer* from(ANativeWindow* w) { return static_cast<Layer*>(w); }
Layer* from(const ANativeWindow* w) { return static_cast<Layer*>(const_cast<ANativeWindow*>(w)); }

void inc_ref(android_native_base_t* base) {
    from(reinterpret_cast<ANativeWindow*>(base))->refs.fetch_add(1);
}
void dec_ref(android_native_base_t* base) { layer_unref(from(reinterpret_cast<ANativeWindow*>(base))); }

int set_swap_interval(ANativeWindow*, int) { return 0; }

int query(const ANativeWindow* window, int what, int* value) {
    const Layer* l = from(window);
    switch (what) {
        case NATIVE_WINDOW_WIDTH:
        case NATIVE_WINDOW_DEFAULT_WIDTH:
            *value = l->width.load();
            return 0;
        case NATIVE_WINDOW_HEIGHT:
        case NATIVE_WINDOW_DEFAULT_HEIGHT:
            *value = l->height.load();
            return 0;
        case NATIVE_WINDOW_FORMAT:
            *value = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
            return 0;
        case NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS:
            *value = 1;
            return 0;
        case NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER:
            *value = 1;
            return 0;
        case NATIVE_WINDOW_CONCRETE_TYPE:
            *value = NATIVE_WINDOW_SURFACE;
            return 0;
        case NATIVE_WINDOW_TRANSFORM_HINT:
        case NATIVE_WINDOW_CONSUMER_RUNNING_BEHIND:
        case NATIVE_WINDOW_CONSUMER_USAGE_BITS:
        case NATIVE_WINDOW_STICKY_TRANSFORM:
        case NATIVE_WINDOW_CONSUMER_IS_PROTECTED:
        case NATIVE_WINDOW_FRAME_TIMESTAMPS_SUPPORTS_PRESENT:
            *value = 0;
            return 0;
        case NATIVE_WINDOW_DEFAULT_DATASPACE:
        case NATIVE_WINDOW_DATASPACE:
            *value = l->dataspace.load();
            return 0;
        case NATIVE_WINDOW_BUFFER_AGE:
            *value = 0;  // every frame redrawn whole: the pbuffer's content is not kept
            return 0;
        case NATIVE_WINDOW_LAYER_COUNT:
            *value = 1;
            return 0;
        case NATIVE_WINDOW_IS_VALID:
            *value = 1;
            return 0;
        case NATIVE_WINDOW_MAX_BUFFER_COUNT:
            *value = 3;
            return 0;
    }
    return -EINVAL;
}

// Settings take effect or do not matter (no buffer queue behind); the getters, which write
// through their variadic arguments, are not supported.
int perform(ANativeWindow* window, int op, ...) {
    switch (op) {
        case NATIVE_WINDOW_SET_BUFFERS_DATASPACE: {
            va_list ap;
            va_start(ap, op);
            from(window)->dataspace = va_arg(ap, int);
            va_end(ap);
            return 0;
        }
        case NATIVE_WINDOW_GET_NEXT_FRAME_ID: {
            static std::atomic<uint64_t> next{1};
            va_list ap;
            va_start(ap, op);
            *va_arg(ap, uint64_t*) = next++;
            va_end(ap);
            return 0;
        }
        case NATIVE_WINDOW_GET_REFRESH_CYCLE_DURATION:
        case NATIVE_WINDOW_GET_COMPOSITOR_TIMING:
        case NATIVE_WINDOW_GET_FRAME_TIMESTAMPS:
        case NATIVE_WINDOW_GET_LAST_DEQUEUE_START:
        case NATIVE_WINDOW_GET_LAST_DEQUEUE_DURATION:
        case NATIVE_WINDOW_GET_LAST_QUEUE_DURATION:
        case NATIVE_WINDOW_LOCK:
        case NATIVE_WINDOW_UNLOCK_AND_POST:
            return -ENOSYS;
    }
    return 0;
}

int dequeue_buffer(ANativeWindow*, ANativeWindowBuffer**, int* fence) {
    if (fence) *fence = -1;
    return -ENOSYS;
}
int queue_buffer(ANativeWindow*, ANativeWindowBuffer*, int) { return -ENOSYS; }
int cancel_buffer(ANativeWindow*, ANativeWindowBuffer*, int) { return -ENOSYS; }

// Every live layer, for a pbuffer going away (windowSurfaceDestroyed).
std::mutex g_layers_lock;
std::vector<Layer*> g_layers;

Layer* layer_new(int w, int h) {
    auto* l = new Layer();
    {
        std::lock_guard<std::mutex> hold(g_layers_lock);
        g_layers.push_back(l);
    }
    l->common.incRef = inc_ref;
    l->common.decRef = dec_ref;
    l->setSwapInterval = set_swap_interval;
    l->query = query;
    l->perform = perform;
    l->dequeueBuffer = dequeue_buffer;
    l->queueBuffer = queue_buffer;
    l->cancelBuffer = cancel_buffer;
    const_cast<int&>(l->minSwapInterval) = 0;
    const_cast<int&>(l->maxSwapInterval) = 1;
    l->width = w;
    l->height = h;
    return l;
}

// The GL names a layer holds go with its last reference, on the render thread (the context's).
void layer_unref(Layer* l) {
    if (l->refs.fetch_sub(1) != 1) return;
    const GLuint tex = l->tex, sw = l->sw_tex;
    {
        std::lock_guard<std::mutex> hold(g_layers_lock);
        g_layers.erase(std::remove(g_layers.begin(), g_layers.end(), l), g_layers.end());
    }
    delete l;
    if ((tex || sw) && RenderThread::hasInstance()) {
        RenderThread::getInstance().queue().post([tex, sw] {
            if (tex) glDeleteTextures(1, &tex);
            if (sw) glDeleteTextures(1, &sw);
        });
    }
}

// --- the scene Java's Compositor sets: the layers bottom to top, where each goes ---

struct Placed {
    Layer* layer;
    int x, y;
    int dim;  // alpha (0..255) of the black drawn over the display before it (FLAG_DIM_BEHIND)
};

std::mutex g_scene_lock;
std::vector<Placed> g_scene;  // each holds a reference
int g_display_w = 0, g_display_h = 0;
bool g_clear_below = false;  // nothing under the windows is black, unless a surface shows through

// The host's frames: under every window (a SurfaceView's surface, a native GL view read back) and
// over them (a setZOrderOnTop surface, a native window on top), each stretched over the display.
struct HostLayer {
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    bool dirty = false;
    GLuint tex = 0;  // render thread
};
std::mutex g_host_lock;
HostLayer g_host_below, g_host_above;

const WinApkHwuiHost* g_host = nullptr;
std::atomic<bool> g_compose_pending{false};
std::atomic<int64_t> g_first_vsync{0};

EGLDisplay g_display = EGL_NO_DISPLAY;

// --- the compositor's GL, render thread only ---

struct Gl {
    bool ready = false;
    GLuint tex_prog = 0, color_prog = 0;
    GLint tex_rect = -1, tex_flip = -1, tex_alpha = -1, tex_sampler = -1;
    GLint color_rect = -1, color_value = -1;
    EGLSurface screen = EGL_NO_SURFACE;
    bool interval_set = false;  // eglSwapInterval(0) on the screen surface
    bool flip_y = false;        // the screen surface is upside down (its first row is the top)
    HWND screen_hwnd = nullptr;
    int screen_w = 0, screen_h = 0;  // a headless pbuffer's size
    EGLConfig config = nullptr;
};
Gl g_gl;

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        ALOGE("compositor shader: %s", log);
    }
    return s;
}

GLuint link(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "corner");
    glLinkProgram(p);
    return p;
}

// One quad, its corner (0..1, 0..1) mapped to rect = (x0, y0, x1, y1) in clip space.
const char* kVs =
    "attribute vec2 corner;\n"
    "uniform vec4 rect;\n"
    "uniform float flip;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "  uv = vec2(corner.x, mix(corner.y, 1.0 - corner.y, flip));\n"
    "  gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);\n"
    "}\n";
const char* kTexFs =
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "uniform sampler2D sampler;\n"
    "uniform float alpha;\n"
    "void main() { gl_FragColor = texture2D(sampler, uv) * alpha; }\n";
const char* kColorVs =
    "attribute vec2 corner;\n"
    "uniform vec4 rect;\n"
    "void main() { gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0); }\n";
const char* kColorFs =
    "precision mediump float;\n"
    "uniform vec4 color;\n"
    "void main() { gl_FragColor = color; }\n";

bool gl_ready() {
    if (g_gl.ready) return true;
    g_gl.tex_prog = link(kVs, kTexFs);
    g_gl.tex_rect = glGetUniformLocation(g_gl.tex_prog, "rect");
    g_gl.tex_flip = glGetUniformLocation(g_gl.tex_prog, "flip");
    g_gl.tex_alpha = glGetUniformLocation(g_gl.tex_prog, "alpha");
    g_gl.tex_sampler = glGetUniformLocation(g_gl.tex_prog, "sampler");
    g_gl.color_prog = link(kColorVs, kColorFs);
    g_gl.color_rect = glGetUniformLocation(g_gl.color_prog, "rect");
    g_gl.color_value = glGetUniformLocation(g_gl.color_prog, "color");
    g_gl.ready = true;
    return true;
}

EGLConfig screen_config(EGLDisplay d) {
    if (g_gl.config) return g_gl.config;
    const EGLint attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                              EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_SURFACE_TYPE,
                              EGL_WINDOW_BIT | EGL_PBUFFER_BIT, EGL_NONE};
    EGLint n = 0;
    eglChooseConfig(d, attribs, &g_gl.config, 1, &n);
    return n ? g_gl.config : nullptr;
}

// The surface the display is presented on: the host window's, or a pbuffer headless.
bool ensure_screen(EGLDisplay d, int frame_w, int frame_h) {
    HWND hwnd = g_host && g_host->window ? g_host->window() : nullptr;
    if (g_gl.screen != EGL_NO_SURFACE) {
        const bool same = hwnd ? hwnd == g_gl.screen_hwnd
                               : (!g_gl.screen_hwnd && g_gl.screen_w == frame_w && g_gl.screen_h == frame_h);
        if (same) return true;
        eglDestroySurface(d, g_gl.screen);
        g_gl.screen = EGL_NO_SURFACE;
    }
    EGLConfig config = screen_config(d);
    if (!config) return false;
    if (hwnd) {
        // Upside down (EGL_ANGLE_surface_orientation): ANGLE's Direct3D 11 swap chain then takes
        // the frame in its back buffer itself. Upright, ANGLE keeps an offscreen colour buffer of
        // the window's size and copies it into the back buffer at every present.
        const EGLint orientation[] = {0x33A8 /*EGL_SURFACE_ORIENTATION_ANGLE*/,
                                      0x0002 /*EGL_SURFACE_ORIENTATION_INVERT_Y_ANGLE*/, EGL_NONE};
        g_gl.screen = eglCreateWindowSurface(d, config, hwnd, orientation);
        g_gl.flip_y = g_gl.screen != EGL_NO_SURFACE;
        if (g_gl.screen == EGL_NO_SURFACE) g_gl.screen = eglCreateWindowSurface(d, config, hwnd, nullptr);
    } else {
        g_gl.flip_y = false;
        const EGLint pa[] = {EGL_WIDTH, frame_w, EGL_HEIGHT, frame_h, EGL_NONE};
        g_gl.screen = eglCreatePbufferSurface(d, config, pa);
    }
    if (g_gl.screen == EGL_NO_SURFACE) {
        ALOGE("compositor: no surface to present on (EGL error 0x%x)", eglGetError());
        return false;
    }
    g_gl.interval_set = false;
    g_gl.screen_hwnd = hwnd;
    g_gl.screen_w = frame_w;
    g_gl.screen_h = frame_h;
    return true;
}

// Draws texture `tex` (currently bound) over client pixels [x, x+w) x [y, y+h) (top-left origin)
// of a client area cw x ch. flip: the texture's first row is the top (CPU pixels); a pbuffer's is
// the bottom (GL's).
void draw_tex(int x, int y, int w, int h, int cw, int ch, bool top_first, float alpha) {
    const float x0 = 2.0f * x / cw - 1.0f, x1 = 2.0f * (x + w) / cw - 1.0f;
    float y0 = 1.0f - 2.0f * (y + h) / ch, y1 = 1.0f - 2.0f * y / ch;
    if (g_gl.flip_y) {
        y0 = -y0;
        y1 = -y1;
    }
    glUseProgram(g_gl.tex_prog);
    glUniform4f(g_gl.tex_rect, x0, y0, x1, y1);
    glUniform1f(g_gl.tex_flip, top_first ? 1.0f : 0.0f);
    glUniform1f(g_gl.tex_alpha, alpha);
    glUniform1i(g_gl.tex_sampler, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void draw_color(int x, int y, int w, int h, int cw, int ch, float r, float g, float b, float a) {
    const float x0 = 2.0f * x / cw - 1.0f, x1 = 2.0f * (x + w) / cw - 1.0f;
    float y0 = 1.0f - 2.0f * (y + h) / ch, y1 = 1.0f - 2.0f * y / ch;
    if (g_gl.flip_y) {
        y0 = -y0;
        y1 = -y1;
    }
    glUseProgram(g_gl.color_prog);
    glUniform4f(g_gl.color_rect, x0, y0, x1, y1);
    glUniform4f(g_gl.color_value, r * a, g * a, b * a, a);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

GLuint new_texture() {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

// CPU pixels (0xAARRGGBB words: BGRA bytes) into texture *tex, sized w x h.
void upload(GLuint* tex, const uint32_t* px, int w, int h, bool bgra) {
    if (!*tex) *tex = new_texture();
    glBindTexture(GL_TEXTURE_2D, *tex);
    const GLenum fmt = bgra ? GL_BGRA_EXT : GL_RGBA;
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, px);
}

void compose_now() {
    g_compose_pending = false;
    RenderThread& rt = RenderThread::getInstance();
    rt.requireGlContext();
    std::vector<Placed> scene;
    int dw, dh;
    bool clear_below;
    {
        std::lock_guard<std::mutex> hold(g_scene_lock);
        scene = g_scene;
        for (Placed& p : scene) p.layer->refs.fetch_add(1);
        dw = g_display_w;
        dh = g_display_h;
        clear_below = g_clear_below;
    }
    struct Unref {
        std::vector<Placed>& s;
        ~Unref() {
            for (Placed& p : s) layer_unref(p.layer);
        }
    } unref{scene};
    if (dw <= 0 || dh <= 0) return;

    const int64_t compose_start = systemTime(SYSTEM_TIME_MONOTONIC);
    EGLDisplay d = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();
    EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW), prev_read = eglGetCurrentSurface(EGL_READ);
    if (!ensure_screen(d, dw, dh)) return;
    if (!eglMakeCurrent(d, g_gl.screen, g_gl.screen, ctx)) {
        ALOGE("compositor: eglMakeCurrent 0x%x", eglGetError());
        return;
    }
    // The present does not wait for the display's vsync: the render thread is the one thread that
    // draws every window, and a swap that blocked it until the next vblank kept the windows' next
    // frames from being drawn in time (on a real GPU, a frame missed in five). Frames are already
    // paced by Choreographer's vsync; DWM composes the window, so nothing tears.
    if (!g_gl.interval_set) {
        eglSwapInterval(d, 0);
        g_gl.interval_set = true;
    }
    gl_ready();

    WinApkHwuiLayout lay{};
    if (g_gl.screen_hwnd) {
        if (!g_host->layout || !g_host->layout(dw, dh, &lay)) {
            eglMakeCurrent(d, prev_draw, prev_read, ctx);
            rt.getGrContext()->resetContext();
            return;
        }
    } else {
        lay = WinApkHwuiLayout{dw, dh, 0, 0, dw, dh, 0};
    }
    const int cw = std::max(1, lay.client_w), ch = std::max(1, lay.client_h);
    const double sx = static_cast<double>(lay.w) / dw, sy = static_cast<double>(lay.h) / dh;

    glViewport(0, 0, cw, ch);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    static const float kCorners[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kCorners);
    glActiveTexture(GL_TEXTURE0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_BLEND);
    // The display: the background where no window is, black when nothing is (as SurfaceFlinger's
    // empty display), clear when a surface shows through the windows.
    {
        const float r = ((lay.background >> 16) & 255) / 255.0f, g = ((lay.background >> 8) & 255) / 255.0f,
                    b = (lay.background & 255) / 255.0f;
        if (clear_below) draw_color(lay.x, lay.y, lay.w, lay.h, cw, ch, 0, 0, 0, 0);
        else draw_color(lay.x, lay.y, lay.w, lay.h, cw, ch, scene.empty() ? r : 0, scene.empty() ? g : 0,
                        scene.empty() ? b : 0, 1);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    // The host's frame under the windows.
    auto draw_host = [&](HostLayer& l, bool opaque) {
        std::lock_guard<std::mutex> hold(g_host_lock);
        if (l.w <= 0 || l.h <= 0) return;
        if (l.dirty) {
            upload(&l.tex, l.px.data(), l.w, l.h, true);
            l.dirty = false;
        }
        glBindTexture(GL_TEXTURE_2D, l.tex);
        if (opaque) glDisable(GL_BLEND);
        draw_tex(lay.x, lay.y, lay.w, lay.h, cw, ch, true, 1.0f);
        if (opaque) glEnable(GL_BLEND);
    };
    draw_host(g_host_below, true);
    for (Placed& p : scene) {
        Layer* l = p.layer;
        if (p.dim > 0) draw_color(lay.x, lay.y, lay.w, lay.h, cw, ch, 0, 0, 0, p.dim / 255.0f);
        if (!l->has_frame) continue;
        const int px = lay.x + static_cast<int>(p.x * sx + (p.x >= 0 ? 0.5 : -0.5));
        const int py = lay.y + static_cast<int>(p.y * sy + (p.y >= 0 ? 0.5 : -0.5));
        if (l->software) {
            std::lock_guard<std::mutex> hold(l->sw_lock);
            if (l->sw_w <= 0) continue;
            if (l->sw_dirty) {
                upload(&l->sw_tex, l->sw_pixels.data(), l->sw_w, l->sw_h, l->sw_bgra);
                l->sw_dirty = false;
            }
            glBindTexture(GL_TEXTURE_2D, l->sw_tex);
            draw_tex(px, py, static_cast<int>(l->sw_w * sx + 0.5), static_cast<int>(l->sw_h * sy + 0.5), cw, ch,
                     true, 1.0f);
        } else if (l->pbuffer != EGL_NO_SURFACE) {
            if (!l->tex) l->tex = new_texture();
            glBindTexture(GL_TEXTURE_2D, l->tex);
            if (!eglBindTexImage(d, l->pbuffer, EGL_BACK_BUFFER)) {
                ALOGE("compositor: eglBindTexImage 0x%x", eglGetError());
                continue;
            }
            draw_tex(px, py, static_cast<int>(l->pb_w * sx + 0.5), static_cast<int>(l->pb_h * sy + 0.5), cw, ch,
                     false, 1.0f);
            eglReleaseTexImage(d, l->pbuffer, EGL_BACK_BUFFER);
        }
    }
    draw_host(g_host_above, false);  // straight alpha 0 or 255: the same premultiplied
    glDisableVertexAttribArray(0);
    if (!g_gl.screen_hwnd && g_host && g_host->wants_pixels && g_host->wants_pixels() && g_host->pixels) {
        std::vector<uint32_t> rgba(static_cast<size_t>(dw) * dh), argb(rgba.size());
        glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        for (int y = 0; y < dh; y++) {
            const uint32_t* s = &rgba[static_cast<size_t>(dh - 1 - y) * dw];
            uint32_t* o = &argb[static_cast<size_t>(y) * dw];
            for (int x = 0; x < dw; x++) {
                const uint32_t v = s[x];  // bytes R, G, B, A
                o[x] = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu);
            }
        }
        g_host->pixels(argb.data(), dw, dh);
    }
    if (!g_logged_first) {
        g_logged_first = true;
        log_memory_now("first composed frame");
    }
    const int64_t swap_start = systemTime(SYSTEM_TIME_MONOTONIC);
    eglSwapBuffers(d, g_gl.screen);
    const int64_t end = systemTime(SYSTEM_TIME_MONOTONIC);
    eglMakeCurrent(d, prev_draw, prev_read, ctx);
    if (GrDirectContext* gr = rt.getGrContext()) gr->resetContext();
    int64_t start = g_first_vsync.exchange(0);
    if (start <= 0 || start > compose_start) start = compose_start;
    if (g_host && g_host->presented) g_host->presented(dw, dh, start, compose_start, swap_start, end);
    // Once, a few seconds in: what Skia's GPU caches hold against their limit (HWUI's CacheManager).
    static int composed = 0;
    ++composed;
    // The first frames are drawn: what only they needed goes. The shader compiler (ANGLE's
    // translator, d3dcompiler_47's state) until a new program needs it, Skia's scratch resources,
    // and the heaps' free blocks given back to Windows (the compiles' scratch is freed by then).
    if (composed == 240) {
        log_memory_now("before the trim");
        glReleaseShaderCompiler();
        if (GrDirectContext* gr = rt.getGrContext()) gr->purgeUnlockedResources(/*scratchResourcesOnly=*/true);
        HANDLE heaps[64];
        const DWORD n = GetProcessHeaps(64, heaps);
        for (DWORD i = 0; i < n && i < 64; i++) HeapCompact(heaps[i], 0);
        log_memory_now("after the trim");
    }
    if (composed == 600) {
        log_memory_now("600 frames");
        if (GrDirectContext* gr = rt.getGrContext()) {
            int count = 0;
            size_t bytes = 0;
            gr->getResourceCacheUsage(&count, &bytes);
            ALOGW("compositor: %d layers composed 600 frames; Skia's GPU cache holds %d resources, %.1f MB "
                  "(limit %.1f MB, for a %dx%d display)", static_cast<int>(scene.size()), count, bytes / 1048576.0,
                  gr->getResourceCacheLimit() / 1048576.0, android::uirenderer::DeviceInfo::getWidth(),
                  android::uirenderer::DeviceInfo::getHeight());
        }
    }
}

void request_compose() {
    if (!g_compose_pending.exchange(true)) RenderThread::getInstance().queue().post([] { compose_now(); });
}

}  // namespace

// --- HWUI's side (window_surface.h) ---

namespace winapk::hwui {

void log_memory(const char* when) { log_memory_now(when); }

bool readWindow(ANativeWindow* window, std::vector<uint32_t>* rgba, int* width, int* height) {
    Layer* l = from(window);
    if (!l->has_frame.load()) return false;
    if (l->software.load()) {
        std::lock_guard<std::mutex> hold(l->sw_lock);
        if (l->sw_w <= 0 || l->sw_h <= 0) return false;
        rgba->assign(l->sw_pixels.begin(), l->sw_pixels.end());
        if (l->sw_bgra) {
            for (uint32_t& v : *rgba) v = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu);
        }
        *width = l->sw_w;
        *height = l->sw_h;
        return true;
    }
    if (l->pbuffer == EGL_NO_SURFACE || l->pb_w <= 0 || l->pb_h <= 0) return false;
    EGLDisplay d = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();
    EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW), prev_read = eglGetCurrentSurface(EGL_READ);
    if (!eglMakeCurrent(d, l->pbuffer, l->pbuffer, ctx)) return false;
    const int w = l->pb_w, h = l->pb_h;
    std::vector<uint32_t> up(static_cast<size_t>(w) * h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, up.data());
    eglMakeCurrent(d, prev_draw, prev_read, ctx);
    if (GrDirectContext* gr = RenderThread::getInstance().getGrContext()) gr->resetContext();
    rgba->resize(up.size());
    for (int y = 0; y < h; y++) {  // GL's rows are bottom first
        std::copy_n(&up[static_cast<size_t>(h - 1 - y) * w], w, &(*rgba)[static_cast<size_t>(y) * w]);
    }
    *width = w;
    *height = h;
    return true;
}

EGLDisplay eglDisplay() {
    if (g_display != EGL_NO_DISPLAY) return g_display;
    auto get_platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    // ANGLE on Direct3D 11: the GPU, else WARP (Windows' software rasterizer).
    const EGLint hw[] = {0x3203 /*EGL_PLATFORM_ANGLE_TYPE_ANGLE*/, 0x3208 /*D3D11*/, EGL_NONE};
    const EGLint warp[] = {0x3203, 0x3208, 0x3209 /*DEVICE_TYPE*/, 0x320B /*WARP*/, EGL_NONE};
    for (const EGLint* attrs : {hw, warp}) {
        EGLDisplay d = get_platform ? get_platform(0x3202 /*EGL_PLATFORM_ANGLE_ANGLE*/, EGL_DEFAULT_DISPLAY, attrs)
                                    : eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint major = 0, minor = 0;
        if (d != EGL_NO_DISPLAY && eglInitialize(d, &major, &minor)) {
            g_display = d;
            attach_blob_cache(d);
            log_memory_now("EGL display up (ANGLE, Direct3D 11)");
            return d;
        }
    }
    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    return g_display;
}

EGLSurface createWindowSurface(EGLDisplay display, EGLConfig config, ANativeWindow* window, const EGLint* attribs) {
    Layer* l = from(window);
    const int w = std::max(1, l->width.load()), h = std::max(1, l->height.load());
    std::vector<EGLint> a = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA, EGL_TEXTURE_TARGET,
                             EGL_TEXTURE_2D};
    for (const EGLint* p = attribs; p && *p != EGL_NONE; p += 2) {
        a.push_back(p[0]);
        a.push_back(p[1]);
    }
    a.push_back(EGL_NONE);
    EGLSurface s = eglCreatePbufferSurface(display, config, a.data());
    if (s != EGL_NO_SURFACE) {
        l->pbuffer = s;
        l->pb_w = w;
        l->pb_h = h;
    }
    return s;
}

bool windowResized(ANativeWindow* window, EGLSurface surface) {
    Layer* l = from(window);
    return window && surface == l->pbuffer && (std::max(1, l->width.load()) != l->pb_w ||
                                               std::max(1, l->height.load()) != l->pb_h);
}

void windowPosted(ANativeWindow* window, EGLSurface surface, long long vsyncNs) {
    Layer* l = from(window);
    if (!window || surface != l->pbuffer) return;
    l->software = false;
    l->has_frame = true;
    int64_t zero = 0;
    g_first_vsync.compare_exchange_strong(zero, vsyncNs);
    request_compose();
}

void windowSurfaceDestroyed(EGLSurface surface) {
    std::lock_guard<std::mutex> hold(g_layers_lock);
    for (Layer* l : g_layers) {
        if (l->pbuffer == surface) {
            l->pbuffer = EGL_NO_SURFACE;
            l->has_frame = l->software.load();
        }
    }
}

}  // namespace winapk::hwui

// --- the NDK's ANativeWindow (what HWUI calls), on a Layer ---

extern "C" {

void ANativeWindow_acquire(ANativeWindow* w) { from(w)->refs.fetch_add(1); }
void ANativeWindow_release(ANativeWindow* w) { layer_unref(from(w)); }
int32_t ANativeWindow_getWidth(ANativeWindow* w) { return from(w)->width; }
int32_t ANativeWindow_getHeight(ANativeWindow* w) { return from(w)->height; }
int32_t ANativeWindow_getFormat(ANativeWindow*) { return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM; }
int32_t ANativeWindow_setBuffersDataSpace(ANativeWindow* w, int32_t dataSpace) {
    from(w)->dataspace = dataSpace;
    return 0;
}
int32_t ANativeWindow_getBuffersDataSpace(ANativeWindow* w) { return from(w)->dataspace; }
int32_t ANativeWindow_setBuffersTransform(ANativeWindow*, int32_t) { return 0; }
void ANativeWindow_tryAllocateBuffers(ANativeWindow*) {}
int ANativeWindow_cancelBuffer(ANativeWindow*, ANativeWindowBuffer*, int) { return -ENOSYS; }
int ANativeWindow_dequeueBuffer(ANativeWindow*, ANativeWindowBuffer**, int* fence) {
    if (fence) *fence = -1;
    return -ENOSYS;
}
int ANativeWindow_setCancelBufferInterceptor(ANativeWindow*, ANativeWindow_cancelBufferInterceptor, void*) {
    return 0;
}
int ANativeWindow_setDequeueBufferInterceptor(ANativeWindow*, ANativeWindow_dequeueBufferInterceptor, void*) {
    return 0;
}
int ANativeWindow_setPerformInterceptor(ANativeWindow*, ANativeWindow_performInterceptor, void*) { return 0; }
int ANativeWindow_setQueueBufferInterceptor(ANativeWindow*, ANativeWindow_queueBufferInterceptor, void*) {
    return 0;
}
int64_t ANativeWindow_getLastDequeueDuration(ANativeWindow*) { return 0; }
int64_t ANativeWindow_getLastQueueDuration(ANativeWindow*) { return 0; }
int64_t ANativeWindow_getLastDequeueStartTime(ANativeWindow*) { return 0; }
int ANativeWindow_setDequeueTimeout(ANativeWindow*, int64_t) { return 0; }

// --- Java: com.winapk.aospviews.HwuiLayers (the window manager's side) ---

static jclass g_layers_class = nullptr;
static jmethodID g_window_of = nullptr;

// The Layer behind a window's Surface: its layer post (Surface.mLayerPost, which copyFrom takes
// from the window's SurfaceControl) is the key HwuiLayers keeps it under. Acquired, as Android's.
ANativeWindow* ANativeWindow_fromSurface(JNIEnv* env, jobject surface) {
    if (!surface || !g_layers_class) return nullptr;
    jclass sc = env->GetObjectClass(surface);
    jfieldID f = env->GetFieldID(sc, "mLayerPost", "Landroid/view/Surface$MacapkLayerPost;");
    env->DeleteLocalRef(sc);
    if (!f) {
        env->ExceptionClear();
        return nullptr;
    }
    jobject post = env->GetObjectField(surface, f);
    const jlong h = post ? env->CallStaticLongMethod(g_layers_class, g_window_of, post) : 0;
    if (post) env->DeleteLocalRef(post);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    if (!h) return nullptr;
    Layer* l = reinterpret_cast<Layer*>(h);
    l->refs.fetch_add(1);
    return l;
}

}  // extern "C"

namespace {

jlong JNICALL layers_create(JNIEnv*, jclass, jint w, jint h) { return reinterpret_cast<jlong>(layer_new(w, h)); }

void JNICALL layers_resize(JNIEnv*, jclass, jlong h, jint w, jint ht) {
    Layer* l = reinterpret_cast<Layer*>(h);
    l->width = w;
    l->height = ht;
}

void JNICALL layers_release(JNIEnv*, jclass, jlong h) { layer_unref(reinterpret_cast<Layer*>(h)); }

// A software frame: the Bitmap's pixels (an android::Bitmap, Bitmap.getNativeInstance()).
void JNICALL layers_post_bitmap(JNIEnv*, jclass, jlong h, jlong bitmap) {
    Layer* l = reinterpret_cast<Layer*>(h);
    auto* b = reinterpret_cast<android::Bitmap*>(bitmap);
    const SkImageInfo& info = b->info();
    const int w = info.width(), ht = info.height();
    if (w <= 0 || ht <= 0 || info.bytesPerPixel() != 4) return;
    {
        std::lock_guard<std::mutex> hold(l->sw_lock);
        l->sw_pixels.resize(static_cast<size_t>(w) * ht);
        const auto* src = static_cast<const uint8_t*>(b->pixels());
        for (int y = 0; y < ht; y++) {
            std::memcpy(&l->sw_pixels[static_cast<size_t>(y) * w], src + y * b->rowBytes(), static_cast<size_t>(w) * 4);
        }
        l->sw_w = w;
        l->sw_h = ht;
        l->sw_bgra = info.colorType() == kBGRA_8888_SkColorType;
        l->sw_dirty = true;
    }
    l->software = true;
    l->has_frame = true;
    request_compose();
}

// The scene: layers bottom to top, their (x, y) in the display, the dim before each, the display.
void JNICALL layers_set_scene(JNIEnv* env, jclass, jlongArray handles, jintArray xy, jintArray dim, jint dw, jint dh,
                              jboolean clear_below) {
    const jsize n = env->GetArrayLength(handles);
    std::vector<jlong> hs(n);
    std::vector<jint> pos(2 * n), dims(n);
    env->GetLongArrayRegion(handles, 0, n, hs.data());
    env->GetIntArrayRegion(xy, 0, 2 * n, pos.data());
    env->GetIntArrayRegion(dim, 0, n, dims.data());
    std::vector<Placed> scene;
    for (jsize i = 0; i < n; i++) {
        Layer* l = reinterpret_cast<Layer*>(hs[i]);
        l->refs.fetch_add(1);
        scene.push_back(Placed{l, pos[2 * i], pos[2 * i + 1], dims[i]});
    }
    {
        std::lock_guard<std::mutex> hold(g_scene_lock);
        g_scene.swap(scene);
        g_display_w = dw;
        g_display_h = dh;
        g_clear_below = clear_below;
    }
    for (Placed& p : scene) layer_unref(p.layer);
    request_compose();
}

const JNINativeMethod kLayersMethods[] = {
    {const_cast<char*>("nCreate"), const_cast<char*>("(II)J"), reinterpret_cast<void*>(layers_create)},
    {const_cast<char*>("nResize"), const_cast<char*>("(JII)V"), reinterpret_cast<void*>(layers_resize)},
    {const_cast<char*>("nRelease"), const_cast<char*>("(J)V"), reinterpret_cast<void*>(layers_release)},
    {const_cast<char*>("nPostBitmap"), const_cast<char*>("(JJ)V"), reinterpret_cast<void*>(layers_post_bitmap)},
    {const_cast<char*>("nSetScene"), const_cast<char*>("([J[I[IIIZ)V"), reinterpret_cast<void*>(layers_set_scene)},
};

}  // namespace

namespace winapk::hwui {

// JNI_OnLoad's: HwuiLayers' natives, when the window manager's class is on the class path (an
// app's run; not --gms or --verify-apk, which have no windows).
void register_layers(JNIEnv* env) {
    jclass c = env->FindClass("com/winapk/aospviews/HwuiLayers");
    if (!c) {
        env->ExceptionClear();
        return;
    }
    g_layers_class = static_cast<jclass>(env->NewGlobalRef(c));
    g_window_of = env->GetStaticMethodID(c, "windowOf", "(Ljava/lang/Object;)J");
    if (!g_window_of || env->RegisterNatives(c, kLayersMethods, sizeof kLayersMethods / sizeof kLayersMethods[0]) != 0) {
        env->ExceptionClear();
        ALOGE("HwuiLayers' natives not bound");
    }
    env->DeleteLocalRef(c);
}

}  // namespace winapk::hwui

// --- the host's side (hwui_host_api.h) ---

extern "C" {

void winapk_hwui_attach(const WinApkHwuiHost* host) { g_host = host; }

void winapk_hwui_recompose(void) {
    if (RenderThread::hasInstance()) request_compose();
}

static void set_host_layer(HostLayer& l, const uint32_t* argb, int w, int h) {
    {
        std::lock_guard<std::mutex> hold(g_host_lock);
        if (!argb || w <= 0 || h <= 0) {
            l.w = l.h = 0;
            l.px.clear();
        } else {
            l.px.assign(argb, argb + static_cast<size_t>(w) * h);
            l.w = w;
            l.h = h;
            l.dirty = true;
        }
    }
    if (RenderThread::hasInstance()) request_compose();
}

void winapk_hwui_host_layer(const uint32_t* argb, int w, int h) { set_host_layer(g_host_below, argb, w, h); }
void winapk_hwui_host_layer_above(const uint32_t* argb, int w, int h) { set_host_layer(g_host_above, argb, w, h); }

void winapk_hwui_display_size(int* w, int* h) {
    std::lock_guard<std::mutex> hold(g_scene_lock);
    *w = g_display_w;
    *h = g_display_h;
}

void winapk_hwui_refresh_rate(double hz) { winapk::hwui::set_refresh_rate(hz); }

}  // extern "C"
