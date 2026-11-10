// window_test.cpp -- winapk-window-test: the app window's geometry
// (host/win32_window.h), without a window.
//
//   winapk-window-test
//
// Checks the two pure decisions the window makes for every frame: the box the
// app draws in (the whole client area when the app follows the window, a
// centred box of the phone's aspect when it fixed the other orientation or
// does not follow), and how a frame is shown in it (pixel for pixel when it is
// the box's size, scaled when the app is meant to draw at another size,
// unscaled at the top left when it is from before a resize).

#include <cstdio>

#include "../host/win32_window.h"
#include "../host/window_fit.h"

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

bool is(const winapk::host::AppBox& b, int x, int y, int w, int h) {
    const bool ok = b.x == x && b.y == y && b.w == w && b.h == h;
    if (!ok) std::printf("    got %d,%d %dx%d, want %d,%d %dx%d\n", b.x, b.y, b.w, b.h, x, y, w, h);
    return ok;
}

}  // namespace

int main() {
    using winapk::host::FramePresent;
    using winapk::host::window_app_box;
    using winapk::host::window_frame_present;

    std::printf("the box the app draws in\n");
    // A 360x800 dp phone at 2 px/dp: the fitted window is the box.
    check("a fitted window is the whole client area", is(window_app_box(720, 1600, 360, 800, 0, true, true), 0, 0, 720, 1600));
    // The user made the window wide: the app follows, so the box is all of it.
    check("a resized window is the whole client area", is(window_app_box(1300, 900, 360, 800, 0, true, true), 0, 0, 1300, 900));
    // A portrait-locked app in a wide (maximized) window: a centred column of
    // the phone's aspect, the client's full height.
    check("portrait lock, wide window: a centred column of the phone's aspect",
          is(window_app_box(2560, 1392, 360, 800, 1, true, true), (2560 - 626) / 2, 0, 626, 1392));
    check("portrait lock, tall window: the whole client area", is(window_app_box(500, 900, 360, 800, 1, true, true), 0, 0, 500, 900));
    // A landscape-locked app in a tall window: the full width.
    check("landscape lock, tall window: a centred row of the phone's aspect",
          is(window_app_box(800, 1400, 800, 360, 2, true, true), 0, (1400 - 360) / 2, 800, 360));
    check("landscape lock, wide window: the whole client area", is(window_app_box(1500, 800, 800, 360, 2, true, true), 0, 0, 1500, 800));
    // WINAPK_RESIZABLE=0: the phone keeps its size, letterboxed, and a box a
    // pixel or two off the client is the client.
    check("not resizable: the largest box of the phone's aspect", is(window_app_box(1920, 1040, 360, 800, 0, false, true), (1920 - 468) / 2, 0, 468, 1040));
    check("not resizable: a client a pixel off the aspect is the box", is(window_app_box(721, 1600, 360, 800, 0, false, true), 0, 0, 721, 1600));
    check("MACAPK_LETTERBOX=0: always the whole client area", is(window_app_box(2560, 1392, 360, 800, 1, false, false), 0, 0, 2560, 1392));
    check("an empty client area is an empty box", is(window_app_box(0, 0, 360, 800, 1, true, true), 0, 0, 0, 0));

    std::printf("how a frame is shown\n");
    check("a frame of the box's size is copied", window_frame_present(1300, 900, 1300, 900, 1300, 900) == FramePresent::kExact);
    check("a pixel of rounding is still copied, not scaled", window_frame_present(451, 1000, 450, 1000, 451, 1000) == FramePresent::kExact);
    check("the app's size on a monitor at another scale is scaled", window_frame_present(720, 1600, 540, 1200, 720, 1600) == FramePresent::kScaled);
    check("the phone letterboxed (WINAPK_RESIZABLE=0) is scaled", window_frame_present(720, 1600, 468, 1040, 720, 1600) == FramePresent::kScaled);
    check("a frame from before a resize is not stretched", window_frame_present(720, 1600, 1300, 900, 1300, 900) == FramePresent::kStale);
    check("a frame from before a resize, smaller box", window_frame_present(1300, 900, 720, 1600, 720, 1600) == FramePresent::kStale);

    // The size a window opens at (window_fit.h), which the launcher's starting window takes too.
    std::printf("the size a window opens at\n");
    using winapk::host::window_density_for;
    using winapk::host::window_fit_scale;
    using winapk::host::window_launch_client;
    auto opens = [](int pw, int ph, int dpi, int aw, int ah, int w, int h) {
        int cw = 0, ch = 0;
        window_launch_client(pw, ph, dpi, aw, ah, &cw, &ch);
        const bool ok = cw == w && ch == h;
        if (!ok) std::printf("    got %dx%d, want %dx%d\n", cw, ch, w, h);
        return ok;
    };
    check("100%, room to spare: a pixel per dp", opens(800, 360, 96, 1900, 1000, 800, 360));
    check("150%: one and a half", opens(800, 360, 144, 1900, 1000, 1200, 540));
    check("200%: two", opens(360, 800, 192, 3800, 2000, 720, 1600));
    check("a tall phone on a short screen at 100%: made to fit, at a whole density", opens(360, 800, 96, 1900, 700, 315, 700));
    check("as wide as the room allows", opens(1280, 800, 96, 1000, 1000, 1000, 625));
    check("never over the room", [] {
        for (int ah = 300; ah < 1400; ah += 7) {
            int cw = 0, ch = 0;
            window_launch_client(360, 800, 144, 2000, ah, &cw, &ch);
            if (ch > ah || cw > 2000) return false;
        }
        return true;
    }());
    check("the density is whole and within Android's buckets",
          window_density_for(1.0f) == 160 && window_density_for(1.5f) == 240 && window_density_for(0.3f) == 120 &&
              window_density_for(9.0f) == 640);
    check("the fit is the tighter side, and never below a quarter",
          window_fit_scale(720, 800, 360, 800) == 1.0f && window_fit_scale(10, 10, 360, 800) == 0.25f &&
              window_fit_scale(0, 500, 360, 800) == 0.25f);

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures;
}
