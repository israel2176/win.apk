package aospviews.sys;

/** com.android.internal.os.BinderInternal's natives (libbinder's process state): the context
 *  object is the in-process service manager of Services; the thread pool and GC hooks do nothing. */
public final class BridgeBinderInternal {
    private BridgeBinderInternal() {}

    private static android.os.IBinder sm;

    public static synchronized android.os.IBinder getContextObject() {
        if (sm == null) sm = Services.serviceManager();
        return sm;
    }
    public static void joinThreadPool() {}
    public static void disableBackgroundScheduling(boolean b) {}
    public static void setMaxThreads(int n) {}
    public static void handleGc() {}
    public static void nSetBinderProxyCountEnabled(boolean b) {}
    public static android.util.SparseIntArray nGetBinderProxyPerUidCounts() { return new android.util.SparseIntArray(); }
    public static int nGetBinderProxyCount(int uid) { return 0; }
    public static void nSetBinderProxyCountWatermarks(int hi, int lo) {}
}
