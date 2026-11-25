// input_pane.h -- Windows' touch keyboard (the input pane) standing for an Android device's
// soft keyboard: InputMethodManager.showSoftInput shows it and hideSoftInput hides it, for an
// editor the user touched (as Windows shows it for its own fields: on a tap with a finger or a
// pen, not on a mouse click), and it goes when the editor loses the focus.
//
// Through WinRT's InputPane for the app's window (IInputPaneInterop::GetForWindow, then
// IInputPane2::TryShow / TryHide, Windows 10 1809 on); TryShow itself keeps to the user's
// setting (Settings > Time & language > Typing > Touch keyboard) and to whether a hardware
// keyboard is attached, and answers false when it declines. combase.dll is looked up at run
// time: where it or InputPane is missing (Wine) the answer is "not available".

#pragma once

#include <windows.h>

namespace winapk::host::input_pane {

// A touchscreen (built in or attached) is ready: without one the touch keyboard never shows
// for a field, as on Windows' own.
bool touch_screen();

enum class Answer { kShown, kHidden, kDeclined, kUnavailable };

// On the window's thread, with COM initialized on it. why: the HRESULT when kUnavailable.
Answer show(HWND window, bool show, HRESULT* why);

}  // namespace winapk::host::input_pane
