// com.macapk.loader.HostPower -- the PC's power source and battery (Win.apk:
// windows/host/native_radios.cpp, GetSystemPowerStatus), for JobScheduler's
// charging and battery-not-low constraints (the system broker's held jobs,
// BrokerJobs) and android.os.BatteryManager.
//
// AOSP's BatteryController counts a device as charging while it is plugged in;
// a device with no battery at all (a TV box) is always plugged in. So is a
// desktop PC: no system battery reads as on mains. A laptop on battery is not
// charging, as a phone off its charger is not. A host without the native (the
// Mac, which asks pmset in BatteryManager) answers "on mains", the answer the
// task this serves needs from a computer that would not say.
package com.macapk.loader;

public final class HostPower {
    private HostPower() {}

    private static final long TTL_MS = 10_000L;
    private static int[] sCached;
    private static long sCachedAtMs;

    /** {AC line 0/1/255, battery percent or 255, battery flags (128: none)}, or null. */
    static synchronized int[] status() {
        long now = System.currentTimeMillis();
        if (sCached != null && now - sCachedAtMs < TTL_MS) return sCached;
        int[] s;
        try {
            s = nativeStatus();
        } catch (UnsatisfiedLinkError e) {
            s = null;
        }
        sCached = s != null && s.length >= 3 ? s : null;
        sCachedAtMs = now;
        return sCached;
    }

    /** Whether there is a battery at all (a desktop PC has none). Null when not known. */
    public static Boolean hasBattery() {
        int[] s = status();
        if (s == null) return null;
        return (s[2] & 128) == 0 && s[2] != 255;
    }

    /** On mains power (or with no battery to run on). */
    public static boolean charging() {
        int[] s = status();
        if (s == null) return true;
        if ((s[2] & 128) != 0) return true;   // no system battery
        if (s[0] == 0) return false;          // on battery
        return true;                          // on mains, or Windows would not say
    }

    /** The battery's charge in percent, or -1 when there is none or it is not known. */
    public static int batteryPercent() {
        int[] s = status();
        if (s == null || (s[2] & 128) != 0 || s[1] < 0 || s[1] > 100) return -1;
        return s[1];
    }

    private static native int[] nativeStatus();
}
