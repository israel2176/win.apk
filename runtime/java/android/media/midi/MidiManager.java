package android.media.midi;
import android.os.Handler;
/** MIDI device access (API 23). Mac.apk exposes no MIDI devices, so the list is
 *  empty and no device callback fires. An open has an answer all the same:
 *  AOSP's OnDeviceOpenedListener.onDeviceOpened gets "a MidiDevice for the
 *  opened device, or null if opening failed", on the caller's Handler after
 *  the call has returned. It got nothing here, and an app that opens a device
 *  it remembers (a Bluetooth keyboard) waited for good.
 *  TODO(MIDI): the host's MIDI ports (WinRT Windows.Devices.Midi) behind
 *  getDevices and openDevice. */
public final class MidiManager {
    public static final int TRANSPORT_MIDI_BYTE_STREAM = 1;
    public static final int TRANSPORT_UNIVERSAL_MIDI_PACKETS = 2;
    public MidiManager() {}
    public MidiManager(android.content.Context context) {}
    public MidiDeviceInfo[] getDevices() { return new MidiDeviceInfo[0]; }
    public java.util.Set<MidiDeviceInfo> getDevicesForTransport(int transport) { return java.util.Collections.emptySet(); }
    public void openDevice(MidiDeviceInfo deviceInfo, OnDeviceOpenedListener listener, Handler handler) {
        failOpen(listener, handler);
    }
    public void openBluetoothDevice(android.bluetooth.BluetoothDevice bluetoothDevice,
            OnDeviceOpenedListener listener, Handler handler) {
        failOpen(listener, handler);
    }

    private static boolean sSaid;

    private static void failOpen(final OnDeviceOpenedListener listener, Handler handler) {
        synchronized (MidiManager.class) {
            if (!sSaid) {
                sSaid = true;
                android.util.Log.w("MidiManager", "there are no MIDI devices here yet: openDevice reports "
                        + "onDeviceOpened(null)");
            }
        }
        if (listener == null) return;
        // AOSP: the listener is called on `handler`, or on the main thread without one.
        Handler target = handler != null ? handler : new Handler(android.os.Looper.getMainLooper());
        target.post(new Runnable() { @Override public void run() { listener.onDeviceOpened(null); } });
    }
    public void registerDeviceCallback(DeviceCallback callback, Handler handler) { }
    public void registerDeviceCallback(int transport, java.util.concurrent.Executor executor, DeviceCallback callback) { }
    public void unregisterDeviceCallback(DeviceCallback callback) { }
    public interface OnDeviceOpenedListener { void onDeviceOpened(MidiDevice device); }
    public static class DeviceCallback {
        public void onDeviceAdded(MidiDeviceInfo device) { }
        public void onDeviceRemoved(MidiDeviceInfo device) { }
        public void onDeviceStatusChanged(MidiDeviceStatus status) { }
    }
}
