// Adapted from the other runtime (https://example.invalid/other-runtime), compat/input/touch_keyboard.h,
// Copyright (C) 2026 its authors, SPDX-License-Identifier: Apache-2.0. Modified for Win.apk.
//
// touch_keyboard.h -- Windows' touch keyboard, as an Android device's soft keyboard: shown when
// the app shows the soft input for an editor the user touched (InputMethodManager.showSoftInput),
// hidden when it hides it or the editor goes (win32_window.cpp, "the touch keyboard").

#pragma once

#include <cstdint>

namespace winapk::host {

// Whether this PC has a touchscreen: without one Windows' touch keyboard is never offered.
bool touch_keyboard_device();

// Shows or hides the touch keyboard for window `hwnd` (InputPane.TryShow / TryHide): 1 when it
// did, 0 when Windows declined (a hardware keyboard is attached and the user's setting says to
// show it only without one, no touch keyboard on this edition), a failed HRESULT when InputPane
// is not there (Windows before 10 1809, Wine). On the window's thread.
int32_t show_touch_keyboard(void* hwnd, bool show);

}  // namespace winapk::host
