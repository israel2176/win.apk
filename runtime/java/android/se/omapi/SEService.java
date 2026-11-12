// android.se.omapi.SEService -- the entry to the device's secure elements (the
// SIM, an embedded SE), Open Mobile API. Mirrors
// frameworks/base/omapi/java/android/se/omapi/SEService.java.
//
// AOSP's constructor binds the secure element service and calls the listener
// once it connects. A PC has no secure element and no such service, which is a
// device whose bind finds nothing: the listener is never called, isConnected()
// stays false, and asking for readers is an IllegalStateException, all AOSP's
// own answers for that state.
package android.se.omapi;

import android.content.Context;

import java.util.concurrent.Executor;

public final class SEService {

    /** Told when the service has connected. */
    public interface OnConnectedListener {
        void onConnected();
    }

    /** @hide in AOSP: an IO error on the secure element. */
    public static final int IO_ERROR = 1;
    /** @hide in AOSP: the applet was not found. */
    public static final int NO_SUCH_ELEMENT_ERROR = 2;

    private final Context mContext;

    public SEService(Context context, Executor executor, OnConnectedListener listener) {
        if (context == null || listener == null || executor == null) {
            throw new NullPointerException("Arguments must not be null");
        }
        mContext = context;
    }

    public boolean isConnected() { return false; }

    /** @throws IllegalStateException always: the service never connects here */
    public Reader[] getReaders() {
        throw new IllegalStateException("service not connected to system");
    }

    /** @throws IllegalStateException always: the service never connects here */
    public Reader getUiccReader(int slotNumber) {
        if (slotNumber < 1) {
            throw new IllegalArgumentException("slotNumber should be larger than 0");
        }
        throw new IllegalStateException("service not connected to system");
    }

    /** Releases every secure element resource; there are none to release. */
    public void shutdown() {}

    /** The Open Mobile API version this implements. */
    public String getVersion() { return "3.3"; }
}
