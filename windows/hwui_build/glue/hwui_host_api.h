// hwui_host_api.h -- what the graphics DLL (AOSP's HWUI, windows/hwui_build) and winapk-runtime
// tell each other about the host window. C only: the host loads the DLL with LoadLibrary and finds
// these by name.
//
// The DLL composes the app's windows (Android's window layers) on the GPU, on HWUI's render thread
// with HWUI's own EGL context, into the host window (an ANGLE window surface on its HWND), as
// SurfaceFlinger composes layers into the display. The host says where in its client area the
// app's display goes and is told of every frame that reached the window.
#pragma once

#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Where the app's display (frame_w x frame_h px) is shown in the host window's client area.
typedef struct WinApkHwuiLayout {
    int client_w, client_h;  // the client area, px
    int x, y, w, h;          // the display's rectangle in it (top-left origin): scaled when w, h differ
    uint32_t background;     // 0xRRGGBB, under the display where no window covers it
} WinApkHwuiLayout;

typedef struct WinApkHwuiHost {
    uint32_t size;  // sizeof(WinApkHwuiHost)
    // The window to present into; NULL when there is none (headless).
    HWND (*window)(void);
    // Fills *out for a display of frame_w x frame_h px; 0 when nothing is shown (minimized).
    int (*layout)(int frame_w, int frame_h, WinApkHwuiLayout* out);
    // A composed frame was presented. Times in ns on one steady clock (QueryPerformanceCounter's):
    // the first window's frame start (its vsync), the compose's start, the swap's start and end.
    void (*presented)(int frame_w, int frame_h, int64_t start_ns, int64_t compose_ns, int64_t swap_ns,
                      int64_t end_ns);
    // Headless only: whether the host wants this frame's pixels (a frame file), and then the pixels
    // (0xAARRGGBB rows, top row first).
    int (*wants_pixels)(void);
    void (*pixels)(const uint32_t* argb, int w, int h);
} WinApkHwuiHost;

// The host's callbacks; before the first window is shown. Any thread.
__declspec(dllexport) void winapk_hwui_attach(const WinApkHwuiHost* host);
// Compose again with what each window last posted (the host window was resized or exposed).
__declspec(dllexport) void winapk_hwui_recompose(void);
// A frame the host has as pixels (an app's native GL or video surface read back, a SurfaceView's
// canvas), shown under every window (0xAARRGGBB, top row first, stretched over the display); NULL
// drops it. Copied.
__declspec(dllexport) void winapk_hwui_host_layer(const uint32_t* argb, int w, int h);
// The same over every window (a setZOrderOnTop surface, a native window on top): 0xAARRGGBB with
// alpha 0 where nothing is, stretched over the display; NULL drops it.
__declspec(dllexport) void winapk_hwui_host_layer_above(const uint32_t* argb, int w, int h);
// The app's display in px, as the window manager last gave it (0 x 0 before).
__declspec(dllexport) void winapk_hwui_display_size(int* w, int* h);
// The display's refresh rate, for the render thread's vsync.
__declspec(dllexport) void winapk_hwui_refresh_rate(double hz);

#ifdef __cplusplus
}
#endif
