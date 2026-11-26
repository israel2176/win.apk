// gl_test.cpp -- winapk-gl-test: ANGLE, as the runtime loads and uses it.
//
//   winapk-gl-test     (ANGLE's DLLs next to the executable)
//
// Initializes the framework context (gl_surface.h) on whatever backend this
// machine has, draws a clear and a scissored clear into a view's pbuffer and
// reads the frame back the way the window gets it. Then checks the extension
// list an app's native code is told (guest_gl_extensions.h) against this
// ANGLE: every extension in it is one ANGLE reports and every one of its
// functions resolves; ANGLE's and Chromium's own extensions pass when they do.
// Last, two threads with a context each on the app's display (gl_turn.h): one
// draws, the other makes its context current, the first draws again without
// making its own current, as Telegram's welcome screen did beside a second GL
// view (issue #195: an access violation in StateManager11::updateState); then
// both draw at once, many times. Each must see its own colour.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "gl/gl_loader.h"
#include "gl/gl_surface.h"
#include "gl/gl_turn.h"
#include "gl/guest_gl_extensions.h"

namespace {

int g_failures = 0;

void check(const char* what, bool ok, unsigned got = 0) {
    std::printf("  [%s] %s", ok ? "PASS" : "FAIL", what);
    if (!ok) std::printf(": got 0x%08x", got);
    std::printf("\n");
    if (!ok) ++g_failures;
}

std::vector<std::string> words(const char* s) {
    std::vector<std::string> out;
    std::istringstream in(s ? s : "");
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

// Whether a guest could call every function of `extension` (a known one) here.
bool resolves(const std::string& extension) {
    const char* fns = winapk::host::guest_extension_functions(extension);
    if (!fns) return false;
    for (const std::string& fn : words(fns)) {
        const auto lib = fn.compare(0, 3, "egl") == 0 ? winapk::host::GlLib::Egl : winapk::host::GlLib::Gles;
        if (!winapk::host::gl_proc(fn.c_str(), lib)) return false;
    }
    return true;
}

bool has(const std::vector<std::string>& v, const char* s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// The guest's GL_EXTENSIONS: a subset of ANGLE's in its order, exactly the
// known ones whose functions resolve, ANGLE's and Chromium's own among them.
void check_guest_extensions() {
    const char* driver = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    check("ANGLE reports GL_EXTENSIONS", driver != nullptr);
    if (!driver) return;
    const std::vector<std::string> all = words(driver);
    const std::vector<std::string> guest = words(winapk::host::guest_extensions_filtered(driver));
    size_t at = 0, expected = 0, angle_own = 0;
    bool ordered = true, exact = true;
    for (const std::string& e : guest) {
        while (at < all.size() && all[at] != e) ++at;
        if (at == all.size()) ordered = false;
        if (!resolves(e)) exact = false;
        if (e.rfind("GL_ANGLE_", 0) == 0 || e.rfind("GL_CHROMIUM_", 0) == 0) ++angle_own;
    }
    for (const std::string& e : all) {
        if (resolves(e)) {
            ++expected;
        } else if (winapk::host::guest_extension_functions(e)) {
            std::printf("  (held back, a function missing in this ANGLE: %s)\n", e.c_str());
        } else {
            std::printf("  (held back, no header declares it: %s)\n", e.c_str());
        }
    }
    std::printf("  ANGLE reports %zu GL extensions; a guest is told of %zu, %zu of them ANGLE's or Chromium's\n",
                all.size(), guest.size(), angle_own);
    check("the guest's list is a subset of ANGLE's, in its order", ordered);
    check("every extension a guest is told of has all its functions in ANGLE", exact);
    check("every known extension whose functions resolve is passed on", guest.size() == expected,
          static_cast<unsigned>(guest.size()));
    // Skia's (Flutter's) two: told exactly when ANGLE reports them and has their functions.
    for (const char* e : {"GL_CHROMIUM_bind_uniform_location", "GL_ANGLE_base_vertex_base_instance"}) {
        const bool reported = has(all, e);
        const bool told = has(guest, e);
        const std::string what = std::string(e) + (reported ? ", reported by ANGLE, " : ", not reported by ANGLE, ") +
                                 (told ? "is passed on" : "is held back");
        check(what.c_str(), told == (reported && resolves(e)));
    }
    check("an unknown extension is not passed on", !winapk::host::guest_extension_allowed("GL_WINAPK_no_such"));
}

// One GL thread of an app: its own context on the app's display and a 16x16
// pbuffer, a program that fills the surface with one colour from a buffer
// object's triangle strip (the shape of Telegram's draw_textured_shape).
struct GlThread {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface pbuffer = EGL_NO_SURFACE;
    GLuint program = 0, vbo = 0;
    GLint color = -1;

    bool setup(EGLDisplay d) {
        dpy = d;
        const EGLint config_attribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                         EGL_NONE};
        EGLConfig config = nullptr;
        EGLint n = 0;
        if (!eglChooseConfig(dpy, config_attribs, &config, 1, &n) || n < 1) return false;
        const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attribs);
        const EGLint pb_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        pbuffer = eglCreatePbufferSurface(dpy, config, pb_attribs);
        if (ctx == EGL_NO_CONTEXT || pbuffer == EGL_NO_SURFACE) return false;
        if (!eglMakeCurrent(dpy, pbuffer, pbuffer, ctx)) return false;
        const char* vs = "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }";
        const char* fs = "precision mediump float; uniform vec4 c; void main() { gl_FragColor = c; }";
        GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(v, 1, &vs, nullptr);
        glShaderSource(f, 1, &fs, nullptr);
        glCompileShader(v);
        glCompileShader(f);
        program = glCreateProgram();
        glAttachShader(program, v);
        glAttachShader(program, f);
        glBindAttribLocation(program, 0, "p");
        glLinkProgram(program);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        color = glGetUniformLocation(program, "c");
        const float strip[] = {-1, -1, 1, -1, -1, 1, 1, 1};
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(strip), strip, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glViewport(0, 0, 16, 16);
        return linked == GL_TRUE && vbo != 0;
    }

    // Draws `rgba` and reads the middle pixel back: whether it is that colour.
    bool draw(uint32_t rgba) {
        glUseProgram(program);
        glUniform4f(color, (rgba >> 24 & 0xff) / 255.0f, (rgba >> 16 & 0xff) / 255.0f, (rgba >> 8 & 0xff) / 255.0f,
                    (rgba & 0xff) / 255.0f);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        unsigned char px[4] = {};
        glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        const uint32_t got = uint32_t{px[0]} << 24 | uint32_t{px[1]} << 16 | uint32_t{px[2]} << 8 | px[3];
        return got == rgba && glGetError() == GL_NO_ERROR;
    }

    void end() {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (pbuffer != EGL_NO_SURFACE) eglDestroySurface(dpy, pbuffer);
        if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
    }
};

void check_two_gl_threads() {
    EGLDisplay dpy = winapk::host::gl_shared_display();
    check("the app's display is up", dpy != nullptr);
    if (!dpy) return;
    constexpr uint32_t kRed = 0xff0000ffu, kGreen = 0x00ff00ffu;
    std::atomic<int> step{0};
    bool a_setup = false, a_first = false, a_after = false, b_setup = false, b_first = false, b_after = false;
    std::atomic<int> a_wrong{0}, b_wrong{0};
    auto wait_for = [&](int s) {
        while (step.load() < s) std::this_thread::yield();
    };
    const unsigned long rebinds_before = winapk::host::gl_turn_rebinds();
    std::thread a([&] {
        GlThread t;
        a_setup = t.setup(dpy);
        a_first = a_setup && t.draw(kRed);
        step = 1;
        wait_for(2);  // B made its context current: the renderer holds B's state
        a_after = a_setup && t.draw(kRed);
        step = 3;
        wait_for(4);
        for (int i = 0; i < 300 && a_setup; ++i) {
            if (!t.draw(kRed)) a_wrong.fetch_add(1);
        }
        t.end();
    });
    std::thread b([&] {
        wait_for(1);
        GlThread t;
        b_setup = t.setup(dpy);
        b_first = b_setup && t.draw(kGreen);
        step = 2;
        wait_for(3);
        b_after = b_setup && t.draw(kGreen);
        step = 4;
        for (int i = 0; i < 300 && b_setup; ++i) {
            if (!t.draw(kGreen)) b_wrong.fetch_add(1);
        }
        t.end();
    });
    a.join();
    b.join();
    check("thread A: a context, a pbuffer, a program and a buffer object", a_setup);
    check("thread A draws red", a_first);
    check("thread B: its own context on the same display", b_setup);
    check("thread B draws green", b_first);
    check("thread A draws red again after B made its context current (issue #195)", a_after);
    check("thread B draws green again after A", b_after);
    check("A's context was made current again for it", winapk::host::gl_turn_rebinds() > rebinds_before);
    check("both drawing at once, 300 frames each: every frame A's red", a_wrong == 0, a_wrong.load());
    check("  ... and every frame B's green", b_wrong == 0, b_wrong.load());
}

}  // namespace

int main() {
    std::printf("ANGLE through the runtime's GL\n");
    const bool ready = winapk::host::gl_frame_begin(1, 64, 32);
    check("context current on a 64x32 pbuffer", ready);
    if (!ready) {
        std::printf("\nFAILED: 1 failure(s)\n");
        return 1;
    }
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);  // red
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 64, 8);  // GL's bottom rows
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);  // blue
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    const bool read = winapk::host::gl_frame_read(&px, &w, &h);
    check("frame read back at the pbuffer's size", read && w == 64 && h == 32);
    if (read) {
        check("top row is red (0xFFFF0000), rows flipped to top-down", px[0] == 0xffff0000u, px[0]);
        check("bottom row is blue (0xFF0000FF)", px[static_cast<size_t>(31) * 64] == 0xff0000ffu, px[31 * 64]);
    }
    check("no GL error", glGetError() == GL_NO_ERROR);

    std::printf("\nThe extensions an app's native code is told of\n");
    check_guest_extensions();

    std::printf("\nTwo GL threads on the app's display\n");
    check_two_gl_threads();

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures;
}
