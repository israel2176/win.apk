// swap_pacing.h -- when an app's presenting eglSwapBuffers may return. No Windows call, so
// test/swap_pacing_test.cpp drives it.
//
// On Android an EGL window surface is a BufferQueue that SurfaceFlinger empties once per vsync:
// with the swap interval at 1 (EGL's default, and what SDL, libGDX and NativeActivity samples
// leave it at) a producer that draws faster than the display blocks in eglSwapBuffers, the next
// dequeueBuffer waiting for a free buffer, so a game loop runs at the display's rate. Win.apk's
// window surface is a pbuffer read back and composed by the host (host/gl/guest_gl.cpp), which
// nothing paces: before this, an SDL game sitting on its menu (Tensy) swapped as fast as the GPU
// and the readback allowed, two cores busy at idle (212% of a core, measured on windows-latest).
//
// The pacer gives the swap the queue's effect: frame n returns no sooner than n vsync periods
// (times the swap interval) after the stream began, so the steady rate is the display's
// divided by the interval. A producer slower than that never waits: once it is more than one
// step behind, the schedule restarts from now (no burst to catch up, as a queue that ran dry
// does not hand out frames faster afterwards); up to one step behind it is let through, the
// slack a second queued buffer gives. Interval 0 (eglSwapInterval(0)) never waits, as on
// Android, where it makes the queue drop frames instead of blocking.

#pragma once

#include <cstdint>

namespace winapk::host {

// The refresh rate the app's display reports, the vsync clock Choreographer runs on
// (NativeDisplayEventReceiver over NativeCocoa.displayHz) and swaps are paced to.
constexpr int kDisplayHz = 60;
constexpr int64_t kVsyncPeriodNs = 1'000'000'000LL / kDisplayHz;

class SwapPacer {
public:
    // A presenting swap at `now_ns` (a monotonic clock) with swap interval `interval`: how
    // long it waits before returning, in ns (0: return at once).
    int64_t wait_ns(int64_t now_ns, int interval, int64_t period_ns = kVsyncPeriodNs) {
        if (interval <= 0) {
            next_ns_ = kNone;
            return 0;
        }
        const int64_t step = period_ns * interval;
        if (next_ns_ == kNone || now_ns - next_ns_ > step) next_ns_ = now_ns;
        const int64_t wait = next_ns_ > now_ns ? next_ns_ - now_ns : 0;
        next_ns_ += step;
        return wait;
    }

private:
    static constexpr int64_t kNone = INT64_MIN;
    int64_t next_ns_ = kNone;
};

}  // namespace winapk::host
