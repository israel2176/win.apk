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

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

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

    // The asynchronous readback (double pixel pack buffer): the first swap
    // buffers this frame and presents nothing; the next presents the previous
    // frame, one behind. The pbuffer still holds the red/blue frame, so the
    // first async read issues it; then draw green, and the second async read
    // should hand back that earlier red/blue frame, not the green one.
    std::vector<uint32_t> apx;
    int pw = 0, ph = 0;
    const bool first = winapk::host::gl_read_current_async(&apx, 64, 32, &pw, &ph);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);  // green: the frame the async read must NOT hand back yet
    glClear(GL_COLOR_BUFFER_BIT);
    const bool second = winapk::host::gl_read_current_async(&apx, 64, 32, &pw, &ph);
    check("async readback presents a frame at the pbuffer size", second && pw == 64 && ph == 32);
    if (!first && second) {  // first == false means the PBO path is active (not the sync fallback)
        check("async readback is one frame behind: top row still red", apx[0] == 0xffff0000u, apx[0]);
    }
    check("no GL error after async readback", glGetError() == GL_NO_ERROR);

    std::printf("\nThe extensions an app's native code is told of\n");
    check_guest_extensions();

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures;
}
