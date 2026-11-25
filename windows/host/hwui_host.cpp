// hwui_host.cpp -- see hwui_host.h.
#include "hwui_host.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

#include "../hwui_build/glue/hwui_host_api.h"
#include "win32_window.h"

namespace winapk::host {

// native_cocoa.cpp: a composed frame was shown (counted, timed, the first one logged); headless,
// whether a frame file wants its pixels, and the pixels.
void hwui_frame_presented(int w, int h, int64_t start_ns, int64_t compose_ns, int64_t swap_ns, int64_t end_ns);
bool hwui_wants_frame_pixels();
void hwui_frame_pixels(const uint32_t* argb, int w, int h);
// win32_window.cpp: where the display goes in the window, and that a frame reached it.
int window_gpu_layout(int frame_w, int frame_h, WinApkHwuiLayout* out);

namespace {

using attach_t = void (*)(const WinApkHwuiHost*);
using recompose_t = void (*)();
using host_layer_t = void (*)(const uint32_t*, int, int);
using refresh_t = void (*)(double);

std::atomic<bool> g_active{false};
recompose_t g_recompose = nullptr;
host_layer_t g_host_layer = nullptr;

HWND host_window() { return window_running() ? static_cast<HWND>(window_handle()) : nullptr; }

int layout(int frame_w, int frame_h, WinApkHwuiLayout* out) { return window_gpu_layout(frame_w, frame_h, out); }

void presented(int w, int h, int64_t start_ns, int64_t compose_ns, int64_t swap_ns, int64_t end_ns) {
    hwui_frame_presented(w, h, start_ns, compose_ns, swap_ns, end_ns);
}

int wants_pixels() { return hwui_wants_frame_pixels() ? 1 : 0; }

void pixels(const uint32_t* argb, int w, int h) { hwui_frame_pixels(argb, w, h); }

const WinApkHwuiHost kHost = {sizeof(WinApkHwuiHost), host_window, layout, presented, wants_pixels, pixels};

}  // namespace

bool hwui_attach() {
    HMODULE dll = GetModuleHandleW(L"aosp-hwui.dll");
    auto attach = dll ? reinterpret_cast<attach_t>(GetProcAddress(dll, "winapk_hwui_attach")) : nullptr;
    g_recompose = dll ? reinterpret_cast<recompose_t>(GetProcAddress(dll, "winapk_hwui_recompose")) : nullptr;
    g_host_layer = dll ? reinterpret_cast<host_layer_t>(GetProcAddress(dll, "winapk_hwui_host_layer")) : nullptr;
    auto refresh = dll ? reinterpret_cast<refresh_t>(GetProcAddress(dll, "winapk_hwui_refresh_rate")) : nullptr;
    if (!attach || !g_recompose || !g_host_layer) return false;
    if (refresh) {
        DEVMODEW mode{};
        mode.dmSize = sizeof mode;
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) {
            refresh(static_cast<double>(mode.dmDisplayFrequency));
        }
    }
    attach(&kHost);
    g_active = true;
    return true;
}

bool hwui_active() { return g_active.load(); }

void hwui_recompose() {
    if (g_active && g_recompose) g_recompose();
}

void hwui_host_layer(const uint32_t* argb, int w, int h) {
    if (g_active && g_host_layer) g_host_layer(argb, w, h);
}

}  // namespace winapk::host
