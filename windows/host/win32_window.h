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
// monitor with another scale). Input goes back out through
// WindowInput, already translated to Android's MotionEvent and KeyEvent terms:
//
//   the mouse's left button   one finger on the touchscreen (pointer)
//   a touchscreen, a pen      every contact a pointer of its own (touch)
//   the wheel                 a scroll step, or a short drag for an app whose
//                             views take no scroll step (wheel)
//   Ctrl+wheel, and a pinch   two fingers moving apart or together around the
//   on a precision touchpad   cursor (pinch)
//   the other buttons, and    a mouse: the right button opens what a long
//   the pointer over the app  press opens, the pointer hovers (mouse)
//   the keyboard              key events with the character Windows says the
//                             key types (key); composed text (text); an input
//                             method's composition (composing)
//
// Positions are pixels in the box the app draws in (window_app_box), origin at
// its top left, with the box's size beside them.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pointer_input.h"
#include "text_input_model.h"

namespace winapk::host {

// Called on the window's UI thread.
class WindowInput {
public:
    virtual ~WindowInput() = default;
    // MotionEvent action (DOWN 0, UP 1, MOVE 2, CANCEL 3); x and y in client
    // pixels, origin at the top left, of a client area view_w by view_h.
    virtual void pointer(int action, float x, float y, float view_w, float view_h) = 0;
    // KeyEvent action (DOWN 0, UP 1), KEYCODE_*, the character or 0, META_*;
    // repeat: Windows is repeating a key that is held.
    virtual void key(int action, int key_code, int unicode, int meta, bool repeat) = 0;
    // The contacts on a touchscreen or a pen's tip (pointer_input.h, TouchTracker).
    virtual void touch(const TouchEvent& /*event*/, float /*view_w*/, float /*view_h*/) {}
    // A wheel step at (x, y): vscroll and hscroll in notches, positive up and
    // to the right (MotionEvent.AXIS_VSCROLL / AXIS_HSCROLL). The app gets it as
    // a scroll event; when none of its views takes that, as a finger that lands
    // at (x, y), moves by the slop and then by the drag, and lifts. False when
    // there is nothing to send it through: the caller drags with pointer().
    virtual bool wheel(float /*x*/, float /*y*/, float /*view_w*/, float /*view_h*/, float /*vscroll*/,
                       float /*hscroll*/, float /*slop_x*/, float /*slop_y*/, float /*drag_x*/, float /*drag_y*/) {
        return false;
    }
    // A step of a two-finger pinch (pointer_input.h, PinchSynth).
    virtual void pinch(const PinchStep& /*step*/, float /*view_w*/, float /*view_h*/) {}
    // The mouse with `buttons` (MotionEvent.BUTTON_SECONDARY, BUTTON_TERTIARY)
    // held at (x, y): a press, a drag or a release when they changed or one is
    // held, else the pointer hovering. mouse_exit: it left the app.
    virtual void mouse(int /*buttons*/, int /*meta*/, float /*x*/, float /*y*/, float /*view_w*/, float /*view_h*/) {}
    virtual void mouse_exit() {}
    // Text that is no single key's: composed with a dead key, an input
    // method's result, a pasted-in character (UTF-16).
    virtual void text(const std::wstring& /*utf16*/) {}
    // An input method is composing. composing_inline: whether the focused
    // field can show the text being composed itself (it has an
    // InputConnection); then composing() carries each state of it and
    // composing_end() its end, and Windows draws no composition window.
    virtual bool composing_inline() { return false; }
    virtual void composing(const std::wstring& /*utf16*/) {}
    virtual void composing_end() {}
    // Where the focused field's caret is, under its line, for the input
    // method's candidate window. False when the app has no such field.
    virtual bool caret(float* /*x*/, float* /*y*/, float /*view_w*/, float /*view_h*/) { return false; }
    // The user closed the window, which is hidden already, or the Windows
    // session is ending: the app is taken through its lifecycle and the
    // process ends, within a bounded time; a second call ends it at once.
    virtual void close_requested() = 0;
    // The user minimized the window: on a device, the app's task went to the
    // back. Also called when the app minimized it itself (moveTaskToBack).
    virtual void minimized() {}
    // The window came back from being minimized (taskbar, Alt+Tab): upstream's
    // unhide observer, which brings back a task that went to the back.
    virtual void restored() {}
    // The user gave the window another size (an edge dragged, maximized,
    // snapped) or it moved to a monitor of another scale: the app's display
    // is now w by h dp at density_dpi, as an Android desktop window's is, and
    // its frames are expected w_px by h_px pixels. The density changes only
    // with the scale the window shows the phone at (maximized: the phone
    // turned; restored; another monitor's scale), never in a drag. Called for
    // every step of a drag.
    virtual void resized(int /*w_dp*/, int /*h_dp*/, int /*w_px*/, int /*h_px*/, int /*density_dpi*/) {}
    // A size the user settled on (a drag ended, a snap), to keep for the
    // app's next start; not a maximized window's.
    virtual void remember_size(int /*w_dp*/, int /*h_dp*/) {}
    // The window was maximized (true) or taken out of it; w by h dp is the
    // size it has when it is not, to keep with the state for the next start.
    virtual void remember_maximized(bool /*maximized*/, int /*w_dp*/, int /*h_dp*/) {}
};

// The box, in client pixels, that the app draws in, for a client area of cw by
// ch pixels showing a phone of phone_w by phone_h (its aspect is all that
// counts): the whole client area, as the app follows the window; the largest
// box of the phone's aspect, centred, when the app fixed the other orientation
// (lock: 0 any, 1 portrait, 2 landscape). Pure; exposed for the window test.
struct AppBox {
    int x = 0, y = 0, w = 0, h = 0;
};
AppBox window_app_box(int cw, int ch, int phone_w, int phone_h, int lock);

// How a frame of fw by fh pixels is shown in a box of bw by bh when the app was
// asked to draw sw by sh: copied pixel for pixel (the frame fills the box, up
// to a pixel or two of rounding, centred), scaled to the box (the app draws at
// the size it was asked to and the box has another: a monitor with another
// scale, a pinned density), or copied unscaled at the box's top left (the
// frame is from before the last resize; the app has not drawn at the new size
// yet, and a stretched frame would only pretend it has). A frame of ow by oh,
// the size asked before the last density change, is rescaled: the same
// picture at the old density, made larger or smaller by the densities' ratio
// from the box's top left, shown until the app draws at the new one. Pure.
enum class FramePresent { kExact, kScaled, kStale, kRescaled };
FramePresent window_frame_present(int fw, int fh, int bw, int bh, int sw, int sh, int ow = 0, int oh = 0);

// The scale, in pixels per dp, the window will open at for a phone of phone_w
// by phone_h dp: the scale of the monitor the app is started on, or less, so
// that the whole window fits that monitor's work area (the screen without the
// taskbar) with a small margin. Remembered for window_start. The runtime
// makes the app's density from it (window_density_dpi), so the frames come at
// the window's own pixel size.
float window_plan(int phone_w, int phone_h);
// Before window_plan: the app's window opens maximized (it was when the app
// last ran), so the plan is for the phone turned to fill the screen.
void window_set_start_maximized(bool maximized);
// The density, in dpi, for a scale in px per dp: scale x 160, rounded down
// so the window still fits, within the 120..640 the Java side accepts.
int window_density_dpi(float scale);

// Creates the window (hidden) for a phone of phone_w by phone_h points, whose
// frames come at surface_scale pixels per point (the app's density / 160; the
// density follows the scale the window shows the phone at later on).
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

// The app window, for dialogs it owns (nullptr headless or before start).
void* window_handle();
// Where in the client area the phone is shown, in client pixels: all of it,
// or a centred rect with the phone's aspect when the window has another shape
// (maximized), with black bars around it. False when there is no window.
bool window_phone_rect(int* x, int* y, int* w, int* h);
// The inject driver's mouse (MACAPK_INJECT_FILE's hosttap, hostswipe, hostmouse, hostwheel and
// magnify verbs): the window's own mouse messages posted to it, at (fx, fy), a fraction of the
// phone's area. False with no window (headless) or a point outside [0,1].
enum PostedMouse {
    kPostLeftDown = 0, kPostLeftUp = 1, kPostMove = 2,  // NativeCocoa.postHostMouse's own three
    kPostRightDown = 3, kPostRightUp = 4, kPostMiddleDown = 5, kPostMiddleUp = 6, kPostLeave = 7,
};
bool window_post_mouse(int action, float fx, float fy);
// A wheel turned by `notches` (positive away from the user, or right), with Ctrl held: the pinch.
bool window_post_wheel(float fx, float fy, float notches, bool horizontal, bool ctrl);
// A trackpad's magnification (the scale grows by 1 + m) as the Ctrl+wheel notches that zoom as much.
bool window_post_magnify(float fx, float fy, float magnification);
// The app moved its task to the back or front (Activity.moveTaskToBack), or
// is quitting.
void window_minimize();
void window_hide();
void window_restore();
// The window and taskbar icon, from a PNG (the app's rendered launcher icon).
// Returns false if the file cannot be read or decoded.
bool window_set_icon_png(const std::wstring& path);
// The color shown until the first frame (the theme's windowBackground).
void window_set_background(uint32_t argb);
// Where Ctrl+Shift+F12 saves a snapshot of the display (window_snapshot), and
// the app's name for the files: the data directory's logs folder.
void window_set_snapshot_dir(const std::wstring& dir, const std::wstring& app);
// Copies the frame; it is painted on the UI thread. A frame at the size the
// app was asked to draw is shown pixel for pixel (window_frame_present).
void window_present(const uint32_t* argb, int w, int h);
// A frame that differs from the one window_present last copied only inside
// [l, r) x [t, b): `rows` holds that rectangle's pixels, row after row. Only
// that part is copied, and repainted. False, with nothing done, when no frame
// of this size is held (the caller then presents the whole frame).
bool window_present_rect(const uint32_t* rows, int w, int h, int l, int t, int r, int b);
// A layer drawn over the frame (dialogs over GL or surface content), scaled to
// the frame, alpha-blended; cleared with window_clear_overlay.
void window_set_overlay(const uint32_t* argb, int w, int h);
void window_clear_overlay();
// Keeps the window out of screenshots and screen sharing (FLAG_SECURE).
void window_set_secure(bool secure);

// The launcher's launch handshake. Win.apk.exe shows a splash while an app
// starts and passes WINAPK_READY_EVENT, an inherited event handle written as a
// decimal number. take_launcher_ready_event (at start) takes it out of the
// environment; signal_launcher_ready sets it once: when the window has painted
// the app's first frame, or when a second start brought the running copy's
// window forward. Without the variable both do nothing (headless runs, tests).
void take_launcher_ready_event();
void signal_launcher_ready();

// ---- text input: the focused editor for Windows' text services, and the touch keyboard ----
//
// The editor side (WindowsTextInput.java, on the editor's thread) tells the window which
// editor has the focus and sends a copy of it (text_input_model.h's Snapshot) and where it drew
// its characters (Layout) whenever they changed; the window's thread keeps a TSF document over
// it for Windows' input methods (tsf_session.h), whose edits go back to the editor through the
// TextInputClient, one per TSF lock. Nothing of TSF exists before the first editor takes the
// focus. Where TSF cannot be set up, input methods compose through IMM32 ("input methods" in
// win32_window.cpp). Any thread; nothing happens without a window.

struct WindowTextField {
    int epoch = 0;        // the editor: a new one, a new epoch
    int input_type = 0;   // EditorInfo.inputType
    int ime_options = 0;  // EditorInfo.imeOptions
    // The editor side hears of every change itself (an Editable it watches): no need to ask it
    // after each key and touch (TextInputClient::sync).
    bool watched = false;
};

// Called on the window's thread.
class TextInputClient {
public:
    virtual ~TextInputClient() = default;
    // What a text service did in one lock, for the editor.
    virtual void edit(const textinput::Edit& e) = 0;
    // A key, a character or a touch went to an editor whose changes the editor side does not
    // watch: it sends a fresh copy.
    virtual void sync(int epoch) = 0;
};
void window_text_client(TextInputClient* client);

void window_text_focus(const WindowTextField& field, const textinput::Snapshot& snapshot);
void window_text_blur(int epoch);
void window_text_snapshot(const textinput::Snapshot& snapshot);
void window_text_layout(const textinput::Layout& layout);
// InputMethodManager.showSoftInput / hideSoftInput: Windows' touch keyboard (input_pane.h),
// shown only for the focused editor, after a touch or a pen on the app, on a PC with a
// touchscreen, and then only if Windows' own policy lets it.
void window_soft_input(bool show);

// Translates a Windows virtual key to an Android KEYCODE_* (0 if none), as
// upstream's host_keymap.h does for macOS key codes.
int android_key_code(unsigned vk, wchar_t ch);

}  // namespace winapk::host
