package aospviews.sys;

/** android.os.SystemClock's natives (libutils' clocks on Android). The aospviews.sys bridges stand
 *  for what stays Win.apk's own under AOSP's framework: the system layer (clocks, libcore's Os,
 *  the looper); Win.apk's framework has these today. */
public final class BridgeSystemClock {
    private BridgeSystemClock() {}

    private static final long T0 = System.nanoTime();

    public static long uptimeNanos() { return System.nanoTime() - T0; }
    public static long uptimeMillis() { return uptimeNanos() / 1_000_000; }
    public static long elapsedRealtimeNanos() { return uptimeNanos(); }
    public static long elapsedRealtime() { return uptimeMillis(); }
    public static long currentThreadTimeMillis() { return currentThreadTimeMicro() / 1000; }
    public static long currentThreadTimeMicro() {
        return java.lang.management.ManagementFactory.getThreadMXBean().getCurrentThreadCpuTime() / 1000;
    }
    public static long currentTimeMicro() { return System.currentTimeMillis() * 1000; }
}
