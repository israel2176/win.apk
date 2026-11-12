// android.hardware.usb.UsbPortStatus -- what a USB Type-C port is doing: is
// something attached, and which power and data roles the port has taken.
// A value type. UsbManager.getPorts() is empty here (a PC's ports are not the
// app's to switch roles on), so no instance reaches an app; the class and its
// constants are what settings-style code links against. Mirrors
// frameworks/base/core/java/android/hardware/usb/UsbPortStatus.java.
package android.hardware.usb;

import android.os.Parcel;
import android.os.Parcelable;

public final class UsbPortStatus implements Parcelable {

    public static final int POWER_ROLE_NONE = 0;
    public static final int POWER_ROLE_SOURCE = 1;
    public static final int POWER_ROLE_SINK = 2;

    public static final int DATA_ROLE_NONE = 0;
    public static final int DATA_ROLE_HOST = 1;
    public static final int DATA_ROLE_DEVICE = 2;

    public static final int MODE_NONE = 0;
    public static final int MODE_UFP = 1 << 0;
    public static final int MODE_DFP = 1 << 1;
    public static final int MODE_DUAL = MODE_UFP | MODE_DFP;
    public static final int MODE_AUDIO_ACCESSORY = 1 << 2;
    public static final int MODE_DEBUG_ACCESSORY = 1 << 3;

    private final int mCurrentMode;
    private final int mCurrentPowerRole;
    private final int mCurrentDataRole;
    private final int mSupportedRoleCombinations;

    /** @hide in AOSP: only the USB service makes one. */
    public UsbPortStatus(int currentMode, int currentPowerRole, int currentDataRole,
            int supportedRoleCombinations) {
        mCurrentMode = currentMode;
        mCurrentPowerRole = currentPowerRole;
        mCurrentDataRole = currentDataRole;
        mSupportedRoleCombinations = supportedRoleCombinations;
    }

    /** Whether something is attached to the port. */
    public boolean isConnected() { return mCurrentMode != 0; }

    public int getCurrentMode() { return mCurrentMode; }

    public int getCurrentPowerRole() { return mCurrentPowerRole; }

    public int getCurrentDataRole() { return mCurrentDataRole; }

    /** Whether the port can take this pair of roles. AOSP keeps the supported
     *  pairs as one bit each, numbered by combineRoles. */
    public boolean isRoleCombinationSupported(int powerRole, int dataRole) {
        return (mSupportedRoleCombinations & combineRoles(powerRole, dataRole)) != 0;
    }

    /** @hide in AOSP. */
    public int getSupportedRoleCombinations() { return mSupportedRoleCombinations; }

    /** AOSP UsbPort.combineRoles: the bit of one (power role, data role) pair. */
    private static int combineRoles(int powerRole, int dataRole) {
        final int numPowerRoles = 3;
        return 1 << ((powerRole - POWER_ROLE_NONE) * numPowerRoles + (dataRole - DATA_ROLE_NONE));
    }

    @Override public String toString() {
        return "UsbPortStatus{connected=" + isConnected() + ", currentMode=" + mCurrentMode
                + ", currentPowerRole=" + mCurrentPowerRole + ", currentDataRole=" + mCurrentDataRole
                + ", supportedRoleCombinations=" + mSupportedRoleCombinations + "}";
    }

    @Override public int describeContents() { return 0; }

    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeInt(mCurrentMode);
        dest.writeInt(mCurrentPowerRole);
        dest.writeInt(mCurrentDataRole);
        dest.writeInt(mSupportedRoleCombinations);
    }

    public static final Parcelable.Creator<UsbPortStatus> CREATOR = new Parcelable.Creator<UsbPortStatus>() {
        @Override public UsbPortStatus createFromParcel(Parcel in) {
            int currentMode = in.readInt();
            int currentPowerRole = in.readInt();
            int currentDataRole = in.readInt();
            int supportedRoleCombinations = in.readInt();
            return new UsbPortStatus(currentMode, currentPowerRole, currentDataRole, supportedRoleCombinations);
        }
        @Override public UsbPortStatus[] newArray(int size) { return new UsbPortStatus[size]; }
    };
}
