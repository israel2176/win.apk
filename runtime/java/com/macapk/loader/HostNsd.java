// HostNsd -- Windows' DNS-SD (windows/host/native_nsd.cpp), for android.net.nsd.NsdManager: browse a
// service type, resolve an instance, register one at this computer's mDNS name. Each call blocks its
// caller (NsdManager's own threads) for at most the time it is given.
package com.macapk.loader;

public final class HostNsd {
    private HostNsd() {}

    /** Whether this Windows has the DNS-SD functions (Windows 10 1809 on); false without a host. */
    public static boolean available() {
        try {
            return nativeAvailable();
        } catch (UnsatisfiedLinkError e) {
            return false;
        }
    }

    /** The instance names ("Name._http._tcp.local") answering `query` ("_http._tcp.local") within `ms`,
     *  or null when the browse could not be made. */
    public static String[] browse(String query, int ms) {
        try {
            return nativeBrowse(query, ms);
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
    }

    /** {host, port, ipv4, ipv6, key, value...} of one instance, or null when it did not answer. */
    public static String[] resolve(String instance, int ms) {
        try {
            return nativeResolve(instance, ms);
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
    }

    /** A registration's handle, 0 when Windows refused it; registered[0] is the name it took. */
    public static long register(String instance, int port, String[] keys, String[] values, int ms,
            String[] registered) {
        try {
            return nativeRegister(instance, port, keys, values, ms, registered);
        } catch (UnsatisfiedLinkError e) {
            return 0;
        }
    }

    public static void unregister(long handle) {
        try {
            nativeUnregister(handle);
        } catch (UnsatisfiedLinkError e) {
            // Nothing was registered.
        }
    }

    private static native boolean nativeAvailable();
    private static native String[] nativeBrowse(String query, int ms);
    private static native String[] nativeResolve(String instance, int ms);
    private static native long nativeRegister(String instance, int port, String[] keys, String[] values, int ms,
            String[] registered);
    private static native void nativeUnregister(long handle);
}
