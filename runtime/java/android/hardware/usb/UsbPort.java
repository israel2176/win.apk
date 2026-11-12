// android.hardware.usb.UsbPort -- one USB Type-C port of the device, whose
// power and data roles a privileged app may switch. UsbManager.getPorts() is
// empty here, and the constructor is not public (as in AOSP), so an app never
// holds one; the class exists for the code that names it. Mirrors
// frameworks/base/core/java/android/hardware/usb/UsbPort.java.
package android.hardware.usb;

public final class UsbPort {

    private final String mId;
    private final int mSupportedModes;

    UsbPort(String id, int supportedModes) {
        mId = id;
        mSupportedModes = supportedModes;
    }

    /** @hide in AOSP: the port's unique id. */
    public String getId() { return mId; }

    /** The port's status, or null when it is unknown. With no USB service
     *  behind the port, it is unknown. */
    public UsbPortStatus getStatus() { return null; }

    /** AOSP asks the USB service to switch roles, which then broadcasts the new
     *  status; with no service nothing switches, as on a port that refuses. */
    public void setRoleCombination(int powerRole, int dataRole) {}

    @Override public String toString() {
        return "UsbPort{id=" + mId + ", supportedModes=" + mSupportedModes + "}";
    }
}
