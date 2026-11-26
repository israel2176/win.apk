// gl_test.cpp -- winapk-gl-test: ANGLE, as the runtime loads and uses it.
//
//   winapk-gl-test     (ANGLE's DLLs next to the executable)
//
// Opens the app's display (gl_surface.h) on whatever backend this machine has,
// makes a context of its own current on a pbuffer, as an app's GL thread does,
// draws a clear and a scissored clear and reads the frame back the way a
// window surface's swap does (gl_read_current). Then checks the extension
// list an app's native code is told (guest_gl_extensions.h) against this
// ANGLE: every extension in it is one ANGLE reports and every one of its
// functions resolves; ANGLE's and Chromium's own extensions pass when they do.

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "gl/gl_loader.h"
#include "gl/gl_surface.h"
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

// A context on the app's display, current on a w x h pbuffer of it on this thread.
bool make_current(int w, int h) {
    const EGLDisplay d = static_cast<EGLDisplay>(winapk::host::gl_shared_display());
    if (!d) return false;
    const EGLint config_attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                     EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                     EGL_NONE};
    EGLConfig config = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig(d, config_attribs, &config, 1, &n) || n < 1) return false;
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    const EGLContext ctx = eglCreateContext(d, config, EGL_NO_CONTEXT, ctx_attribs);
    const EGLint pb_attribs[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
    const EGLSurface s = ctx == EGL_NO_CONTEXT ? EGL_NO_SURFACE : eglCreatePbufferSurface(d, config, pb_attribs);
    return s != EGL_NO_SURFACE && eglMakeCurrent(d, s, s, ctx);
}

}  // namespace

int main() {
    std::printf("ANGLE through the runtime's GL\n");
    const bool ready = make_current(64, 32);
    check("context current on a 64x32 pbuffer of the app's display", ready);
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
    winapk::host::gl_read_current(&px, 64, 32);
    check("frame read back at the pbuffer's size", px.size() == static_cast<size_t>(64) * 32);
    if (px.size() == static_cast<size_t>(64) * 32) {
        check("top row is red (0xFFFF0000), rows flipped to top-down", px[0] == 0xffff0000u, px[0]);
        check("bottom row is blue (0xFF0000FF)", px[static_cast<size_t>(31) * 64] == 0xff0000ffu, px[31 * 64]);
    }
    check("no GL error", glGetError() == GL_NO_ERROR);

    std::printf("\nThe extensions an app's native code is told of\n");
    check_guest_extensions();

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures;
}
