package android.os;
// atrace is ART's; tracing is off.
public final class Trace {
  public static final long TRACE_TAG_VIEW = 1L << 3, TRACE_TAG_RESOURCES = 1L << 13, TRACE_TAG_APP = 1L << 12;
  public static boolean isTagEnabled(long t) { return false; }
  public static boolean isEnabled() { return false; }
  public static void traceBegin(long t, String n) {} public static void traceEnd(long t) {}
  public static void beginSection(String n) {} public static void endSection() {}
  public static void asyncTraceBegin(long t, String n, int c) {} public static void asyncTraceEnd(long t, String n, int c) {}
  public static void traceCounter(long t, String n, int v) {}
}
