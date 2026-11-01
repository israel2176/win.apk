// win32_window.h -- the one top-level window an app runs in.
//
// Upstream draws into an NSWindow per app; this is its Win32 counterpart, kept
// apart from the JNI code in native_cocoa.cpp. The window has its own UI thread
// with a message loop, so nothing here depends on which Java thread calls in.
//
// Frames arrive as ARGB pixels at the app's surface size, which is the pixel
// size of the box the window shows the app in (the whole client area, or a
// letterbox inside it), so a frame at the right size is copied pixel for
// pixel; GDI scales only when the app is meant to draw at another size (a
// monitor with another scale, WINAPK_RESIZABLE=0). Mouse and keyboard input
// go back out through WindowInput, already translated to Android's MotionEvent
// and KeyEvent terms.

#pragma once

#include <cstdint>
#include <string>

namespace winapk::host {

// Called on the window's UI thread.
class WindowInput {
public:
    virtual ~WindowInput() = default;
    // MotionEvent action (DOWN 0, UP 1, MOVE 2, CANCEL 3); x and y in client
    // pixels, origin at the top left, of a client area view_w by view_h.
    virtual void pointer(int action, float x, float y, float view_w, float view_h) = 0;
    // KeyEvent action (DOWN 0, UP 1), KEYCODE_*, the character or 0, META_*.
    virtual void key(int action, int key_code, int unicode, int meta) = 0;
    // The user closed the window.
    virtual void close_requested() = 0;
    // The window came back from being minimized (taskbar, Alt+Tab): upstream's
    // unhide observer, which brings back a task the app sent to the back.
    virtual void restored() {}
    // The user gave the window another size (an edge dragged, maximized,
    // snapped): the app's display is now w by h dp, as an Android desktop
    // window's is, and its frames are expected w by h pixels. Called for
    // every step of a drag.
    virtual void resized(int /*w_dp*/, int /*h_dp*/, int /*w_px*/, int /*h_px*/) {}
    // A size the user settled on (a drag ended, a snap), to keep for the
    // app's next start; not a maximized window's.
    virtual void remember_size(int /*w_dp*/, int /*h_dp*/) {}
};

// The box, in client pixels, that the app draws in, for a client area of cw by
// ch pixels showing a phone of phone_w by phone_h (its aspect is all that
// counts): the whole client area when the app follows the window; the largest
// box of the phone's aspect, centred, when the app fixed the other orientation
// (lock: 0 any, 1 portrait, 2 landscape) or does not follow the window
// (resizable false). With letterbox false the box is always the whole client
// area (MACAPK_LETTERBOX=0). Pure; exposed for the window test.
struct AppBox {
    int x = 0, y = 0, w = 0, h = 0;
};
AppBox window_app_box(int cw, int ch, int phone_w, int phone_h, int lock, bool resizable, bool letterbox);

// How a frame of fw by fh pixels is shown in a box of bw by bh when the app was
// asked to draw sw by sh: copied pixel for pixel (the frame fills the box, up
// to a pixel or two of rounding, centred), scaled to the box (the app draws at
// the size it was asked to and the box has another: a monitor with another
// scale, WINAPK_RESIZABLE=0), or copied unscaled at the box's top left (the
// frame is from before the last resize; the app has not drawn at the new size
// yet, and a stretched frame would only pretend it has). Pure.
enum class FramePresent { kExact, kScaled, kStale };
FramePresent window_frame_present(int fw, int fh, int bw, int bh, int sw, int sh);

// The scale, in pixels per dp, the window will open at for a phone of phone_w
// by phone_h dp: the scale of the monitor the app is started on, or less, so
// that the whole window fits that monitor's work area (the screen without the
// taskbar) with a small margin. Remembered for window_start. The runtime
// makes the app's density from it (window_density_dpi), so the frames come at
// the window's own pixel size. WINAPK_TEST_WORKAREA=<w>x<h>[+<x>+<y>], in
// pixels, stands in for the work area (testing; see win32_window.cpp).
float window_plan(int phone_w, int phone_h);
// The density, in dpi, for a scale in px per dp: scale x 160, rounded down
// so the window still fits, within the 120..640 the Java side accepts.
int window_density_dpi(float scale);

// Creates the window (hidden) for a phone of phone_w by phone_h points, whose
// frames come at surface_scale pixels per point (the app's density / 160).
// The window is fitted to its monitor's work area, at launch and whenever the
// monitor, the work area or the phone's orientation changes. When the user
// resizes it, the app follows (WindowInput::resized).
// Returns false when there is no desktop to put a window on.
bool window_start(int phone_w, int phone_h, float surface_scale, WindowInput* input);
bool window_running();

void window_show();
void window_set_title(const std::string& utf8);
// The app's own phone size, in dp (it turned the phone, or asked for a size):
// the window takes that shape, and the frames are expected at that size times
// the surface scale.
void window_set_phone_size(int phone_w, int phone_h);
// The orientation the app asked for: 0 any, 1 portrait, 2 landscape. A window
// resized to the other shape keeps the app's size, letterboxed, as Android
// letterboxes a fixed-orientation app on a large screen.
void window_set_orientation_lock(int lock);

// The app moved its task to the back or front (Activity.moveTaskToBack).
// The app window, for dialogs it owns (nullptr headless or before start).
void* window_handle();
// Where in the client area the phone is shown, in client pixels: all of it,
// or a centred rect with the phone's aspect when the window has another shape
// (maximized), with black bars around it. False when there is no window.
bool window_phone_rect(int* x, int* y, int* w, int* h);
void window_minimize();
void window_hide();
void window_restore();
// The window and taskbar icon, from a PNG (the app's rendered launcher icon).
// Returns false if the file cannot be read or decoded.
bool window_set_icon_png(const std::wstring& path);
// The color shown until the first frame (the theme's windowBackground).
void window_set_background(uint32_t argb);
// Copies the frame; it is painted on the UI thread. A frame at the size the
// app was asked to draw is shown pixel for pixel (window_frame_present).
void window_present(const uint32_t* argb, int w, int h);
// A layer drawn over the frame (dialogs over GL or surface content), scaled to
// the frame, alpha-blended; cleared with window_clear_overlay.
void window_set_overlay(const uint32_t* argb, int w, int h);
void window_clear_overlay();
// Keeps the window out of screenshots and screen sharing (FLAG_SECURE).
void window_set_secure(bool secure);

// Translates a Windows virtual key to an Android KEYCODE_* (0 if none), as
// upstream's host_keymap.h does for macOS key codes.
int android_key_code(unsigned vk, wchar_t ch);

}  // namespace winapk::host
