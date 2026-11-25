// swap_pacing_test.cpp -- winapk-swap-pacing-test: when an app's presenting eglSwapBuffers returns
// (host/swap_pacing.h). No window and no Windows call: a simulated clock drives producers of several
// speeds through the pacer, as guest_eglSwapBuffers does.
//
//   winapk-swap-pacing-test

#include <cstdint>
#include <cstdio>

#include "swap_pacing.h"

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

using winapk::host::kVsyncPeriodNs;
using winapk::host::SwapPacer;

constexpr int64_t kMs = 1'000'000;

// A producer that spends `work_ns` on each frame then swaps, for `seconds`: the frames it
// presents per second. `waited` gets the time the swaps spent blocked.
double run(int64_t work_ns, int interval, double seconds, int64_t* waited = nullptr) {
    SwapPacer pacer;
    int64_t now = 123 * kMs, total_wait = 0;
    const int64_t end = now + static_cast<int64_t>(seconds * 1e9);
    long frames = 0;
    while (now < end) {
        now += work_ns;
        const int64_t w = pacer.wait_ns(now, interval);
        total_wait += w;
        now += w;
        ++frames;
    }
    if (waited) *waited = total_wait;
    return frames / seconds;
}

bool about(double fps, double want) { return fps > want - 1.0 && fps < want + 1.0; }

}  // namespace

int main() {
    std::printf("a producer faster than the display runs at the display's rate\n");
    {
        check("a spinning loop (0.1 ms a frame) presents 60 frames a second", about(run(kMs / 10, 1, 10), 60));
        check("so does one at 5 ms a frame", about(run(5 * kMs, 1, 10), 60));
        check("swap interval 2: 30 frames a second", about(run(kMs, 2, 10), 30));
        check("swap interval 0: never paced", run(kMs, 0, 1) > 900);
    }

    std::printf("a producer slower than the display is never held back\n");
    {
        int64_t waited = 0;
        check("20 ms a frame: 50 frames a second", about(run(20 * kMs, 1, 10, &waited), 50));
        check("  and its swaps never wait", waited == 0);
        check("17 ms a frame (just slower than a vsync): ~58.8 a second", about(run(17 * kMs, 1, 10, &waited), 58.8));
        check("  and its swaps never wait", waited == 0);
    }

    std::printf("the schedule\n");
    {
        SwapPacer p;
        const int64_t t0 = 1000 * kMs;
        check("the first swap returns at once", p.wait_ns(t0, 1) == 0);
        check("the next, 1 ms later, waits for the next vsync slot", p.wait_ns(t0 + kMs, 1) == kVsyncPeriodNs - kMs);
        // The producer was a little late (a frame took 20 ms): within a step it is let through,
        // and the slot after stays on the original phase.
        const int64_t late = t0 + 2 * kVsyncPeriodNs + 3 * kMs;
        check("a frame up to one step late does not wait", p.wait_ns(late, 1) == 0);
        check("  and the next slot keeps the phase", p.wait_ns(late + kMs, 1) == t0 + 3 * kVsyncPeriodNs - late - kMs);
        // A pause (the app stopped drawing for a second): no burst of unpaced frames after it.
        const int64_t back = t0 + 1000 * kMs;
        check("after a pause the first swap returns at once", p.wait_ns(back, 1) == 0);
        check("  and the second waits a whole period, no catch-up", p.wait_ns(back, 1) == kVsyncPeriodNs);
        check("interval 0 then 1: starts afresh", p.wait_ns(back + kMs, 0) == 0 && p.wait_ns(back + 2 * kMs, 1) == 0);
    }

    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
