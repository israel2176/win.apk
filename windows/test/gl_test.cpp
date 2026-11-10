// gl_test.cpp -- winapk-gl-test: ANGLE, as the runtime loads and uses it.
//
//   winapk-gl-test     (ANGLE's DLLs next to the executable)
//
// Initializes the framework context (gl_surface.h) on whatever backend this
// machine has, draws a clear and a scissored clear into a view's pbuffer and
// reads the frame back the way the window gets it.

#include <cstdio>
#include <vector>

#include <GLES2/gl2.h>

#include "gl/gl_surface.h"

namespace {

int g_failures = 0;

void check(const char* what, bool ok, unsigned got = 0) {
    std::printf("  [%s] %s", ok ? "PASS" : "FAIL", what);
    if (!ok) std::printf(": got 0x%08x", got);
    std::printf("\n");
    if (!ok) ++g_failures;
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

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures;
}
