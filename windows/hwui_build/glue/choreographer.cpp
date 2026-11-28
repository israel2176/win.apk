// choreographer.cpp -- the AChoreographer and ADisplay HWUI's render thread asks for.
//
// On Android the render thread's AChoreographer gets the display's vsync from SurfaceFlinger over a
// descriptor its Looper watches. Here one vsync thread ticks at the display's refresh period, and
// only while a callback is posted: it signals the choreographer's emulated descriptor, and the
// render thread's Looper then calls AChoreographer_handlePendingEvents, which runs the callbacks
// posted before the tick with its time (systemTime's clock, as the render thread's own).
#include <android/choreographer.h>
#include <apex/display.h>
#include <private/android/choreographer.h>
#include <utils/Timers.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "fd_emul.h"

namespace {

std::atomic<int64_t> g_period_ns{16666667};

}  // namespace

struct AChoreographerFrameCallbackData {
    int64_t frameTimeNanos;
    int64_t deadlineNanos;
    AVsyncId vsyncId;
};

struct AChoreographer {
    int fd = -1;
    std::mutex lock;
    std::condition_variable wake;
    std::vector<std::pair<AChoreographer_vsyncCallback, void*>> pending;
    std::vector<std::pair<AChoreographer_refreshRateCallback, void*>> rate;
    int64_t tick_ns = 0;  // the newest tick handlePendingEvents has not run yet
    AVsyncId vsync_id = 0;
    std::thread clock;
};

namespace {

// Sleeps to the next tick of the period on systemTime's clock, then signals the descriptor, while
// callbacks are waiting. A timer with 1 ms resolution (timeBeginPeriod is the host's).
void vsync_loop(AChoreographer* c) {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                          TIMER_ALL_ACCESS);
    for (;;) {
        {
            std::unique_lock<std::mutex> hold(c->lock);
            c->wake.wait(hold, [c] { return !c->pending.empty(); });
        }
        const int64_t period = g_period_ns.load();
        const int64_t now = systemTime(SYSTEM_TIME_MONOTONIC);
        const int64_t next = (now / period + 1) * period;
        const int64_t wait_ns = next - now;
        if (wait_ns > 0) {
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -(wait_ns / 100);
                if (due.QuadPart >= 0) due.QuadPart = -1;
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            } else {
                const DWORD wait_ms = static_cast<DWORD>(wait_ns / 1000000);
                if (wait_ms > 0) Sleep(wait_ms);
            }
        }
        {
            std::lock_guard<std::mutex> hold(c->lock);
            c->tick_ns = next;
            c->vsync_id++;
        }
        winapk::hwui::emulated_signal(c->fd);
    }
}

}  // namespace

extern "C" {

AChoreographer* AChoreographer_create() {
    auto* c = new AChoreographer();
    c->fd = winapk::hwui::emulated_eventfd();
    c->clock = std::thread(vsync_loop, c);
    c->clock.detach();
    return c;
}

int AChoreographer_getFd(const AChoreographer* c) { return c->fd; }

void AChoreographer_handlePendingEvents(AChoreographer* c, void*) {
    if (!winapk::hwui::emulated_drain(c->fd)) return;
    std::vector<std::pair<AChoreographer_vsyncCallback, void*>> run;
    AChoreographerFrameCallbackData data{};
    {
        std::lock_guard<std::mutex> hold(c->lock);
        run.swap(c->pending);
        data.frameTimeNanos = c->tick_ns;
        data.deadlineNanos = c->tick_ns + g_period_ns.load();
        data.vsyncId = c->vsync_id;
    }
    for (auto& [cb, d] : run) cb(&data, d);
}

void AChoreographer_postVsyncCallback(AChoreographer* c, AChoreographer_vsyncCallback callback, void* data) {
    {
        std::lock_guard<std::mutex> hold(c->lock);
        c->pending.emplace_back(callback, data);
    }
    c->wake.notify_one();
}

void AChoreographer_registerRefreshRateCallback(AChoreographer* c, AChoreographer_refreshRateCallback cb,
                                                void* data) {
    {
        std::lock_guard<std::mutex> hold(c->lock);
        c->rate.emplace_back(cb, data);
    }
    cb(g_period_ns.load(), data);
}

int64_t AChoreographerFrameCallbackData_getFrameTimeNanos(const AChoreographerFrameCallbackData* d) {
    return d->frameTimeNanos;
}
size_t AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(const AChoreographerFrameCallbackData*) {
    return 0;
}
AVsyncId AChoreographerFrameCallbackData_getFrameTimelineVsyncId(const AChoreographerFrameCallbackData* d, size_t) {
    return d->vsyncId;
}
int64_t AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(const AChoreographerFrameCallbackData* d,
                                                                      size_t) {
    return d->deadlineNanos;
}

}  // extern "C"

namespace android {

int64_t AChoreographer_getFrameInterval(const AChoreographer*) { return g_period_ns.load(); }

// One internal display, sRGB 8888: what DeviceInfo takes as the wide-gamut format too (no
// wide-colour surfaces are made).
struct ADisplay {};

int ADisplay_acquirePhysicalDisplays(ADisplay*** out) {
    static ADisplay display;
    static ADisplay* list[1] = {&display};
    ADisplay** copy = static_cast<ADisplay**>(std::malloc(sizeof(ADisplay*)));
    copy[0] = list[0];
    *out = copy;
    return 1;
}
void ADisplay_release(ADisplay** displays) { std::free(displays); }
ADisplayType ADisplay_getDisplayType(ADisplay*) { return DISPLAY_TYPE_INTERNAL; }
void ADisplay_getPreferredWideColorFormat(ADisplay*, ADataSpace* dataspace, AHardwareBuffer_Format* format) {
    *dataspace = ADATASPACE_SRGB;
    *format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
}

}  // namespace android

namespace winapk::hwui {

// The host's display refresh rate, in Hz (the monitor's; 60 when unknown).
void set_refresh_rate(double hz) {
    if (hz >= 20 && hz <= 500) g_period_ns = static_cast<int64_t>(1e9 / hz);
}

}  // namespace winapk::hwui
