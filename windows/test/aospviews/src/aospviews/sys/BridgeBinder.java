package aospviews.sys;

import android.os.Binder;
import android.os.IBinder;

/** android.os.Binder's natives for local (in-process) binders only: the calling identity is this
 *  process, a binder's native holder is a token nothing frees. Win.apk's cross-process Binder
 *  (host/binder_win.cpp, MacApkBinderTransport) would answer these for real. */
public final class BridgeBinder {
    private BridgeBinder() {}

    private static final java.util.concurrent.atomic.AtomicLong HOLDERS = new java.util.concurrent.atomic.AtomicLong();
    private static final ThreadLocal<Integer> POLICY = ThreadLocal.withInitial(() -> 0);

    public static long getNativeBBinderHolder() { return HOLDERS.incrementAndGet(); }
    public static long getNativeFinalizer() { return 0; }  // NativeAllocationRegistry skips 0
    public static int getCallingPid() { return (int) ProcessHandle.current().pid(); }
    public static int getCallingUid() { return 10123; }  // an app uid, as Process.myUid would say
    public static boolean isDirectlyHandlingTransaction() { return false; }
    public static boolean hasExplicitIdentity() { return false; }
    public static long clearCallingIdentity() { return 0; }
    public static void restoreCallingIdentity(long t) {}
    public static void setThreadStrictModePolicy(int p) { POLICY.set(p); }
    public static int getThreadStrictModePolicy() { return POLICY.get(); }
    public static long setCallingWorkSourceUid(int uid) { return 0; }
    public static int getCallingWorkSourceUid() { return -1; }
    public static long clearCallingWorkSource() { return 0; }
    public static void restoreCallingWorkSource(long t) {}
    public static void markVintfStability(Binder b) {}
    public static void forceDowngradeToSystemStability(Binder b) {}
    public static void flushPendingCommands() {}
    public static void blockUntilThreadAvailable() {}
    public static IBinder getExtension(Binder b) { return null; }
    public static void setExtension(Binder b, IBinder e) {}
}
