package aospviews.sys;

/** android.util.EventLog's natives (liblog's binary event buffer): dropped, as Win.apk's own
 *  EventLog does. ServiceManager writes one when a service lookup is slow. */
public final class BridgeEventLog {
    private BridgeEventLog() {}

    public static int writeEvent(int tag, int v) { return 0; }
    public static int writeEvent(int tag, long v) { return 0; }
    public static int writeEvent(int tag, float v) { return 0; }
    public static int writeEvent(int tag, String v) { return 0; }
    public static int writeEvent(int tag, Object[] v) { return 0; }
    public static void readEvents(int[] tags, java.util.Collection<android.util.EventLog.Event> out) {}
    public static void readEventsOnWrapping(int[] tags, long t, java.util.Collection<android.util.EventLog.Event> out) {}
}
