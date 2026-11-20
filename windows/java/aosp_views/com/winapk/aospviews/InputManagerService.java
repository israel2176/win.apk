// IInputManager for AOSP's InputManagerGlobal (docs/research/aosp-views.md, step V2): what
// InputManagerService answers an app about the input devices and the pointer. ViewRootImpl asks it
// on every mouse event (updatePointerIcon: the icon of the view under the pointer); with no
// service InputManagerGlobal.getInstance() is null and the mouse's events threw out of the input
// stage. The devices are Win.apk's own (InputDevice: the touchscreen, the keyboard, the mouse and
// the gamepads it knows). The pointer icon is the host's: Windows draws its own cursor and there is
// no bridge for an app's PointerIcon yet, so setting one changes nothing, as on a device whose
// pointer is hidden. Pointer capture is not granted (no IWindow.dispatchPointerCaptureChanged).
package com.winapk.aospviews;

import android.hardware.input.IInputDevicesChangedListener;
import android.os.IBinder;
import android.view.InputDevice;
import android.view.PointerIcon;

public final class InputManagerService {
    public int[] getInputDeviceIds() {
        int[] ids = InputDevice.getDeviceIds();
        return ids != null ? ids : new int[0];
    }

    public InputDevice getInputDevice(int id) { return InputDevice.getDevice(id); }

    public void registerInputDevicesChangedListener(IInputDevicesChangedListener listener) {}

    /** InputManagerService.isInTabletMode: SWITCH_STATE_OFF, a phone's (no tablet-mode switch). */
    public int isInTabletMode() { return 0; }

    public void setPointerIconType(int iconType) {}

    public void setCustomPointerIcon(PointerIcon icon) {}

    public void requestPointerCapture(IBinder inputChannelToken, boolean enabled) {}
}
