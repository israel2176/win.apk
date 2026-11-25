package android.util;
// android.util.Log's natives are liblog's; print to stderr instead.
public final class Log {
  public static final int VERBOSE = 2, DEBUG = 3, INFO = 4, WARN = 5, ERROR = 6, ASSERT = 7;
  private static int p(int l, String t, String m, Throwable tr) { System.err.println("[" + l + "/" + t + "] " + m + (tr == null ? "" : " " + tr)); return 0; }
  public static int v(String t, String m) { return 0; } public static int v(String t, String m, Throwable e) { return 0; }
  public static int d(String t, String m) { return 0; } public static int d(String t, String m, Throwable e) { return 0; }
  public static int i(String t, String m) { return p(INFO, t, m, null); } public static int i(String t, String m, Throwable e) { return p(INFO, t, m, e); }
  public static int w(String t, String m) { return p(WARN, t, m, null); } public static int w(String t, String m, Throwable e) { return p(WARN, t, m, e); }
  public static int w(String t, Throwable e) { return p(WARN, t, "", e); }
  public static int e(String t, String m) { return p(ERROR, t, m, null); } public static int e(String t, String m, Throwable e) { return p(ERROR, t, m, e); }
  public static int wtf(String t, String m) { return p(ASSERT, t, m, null); } public static int wtf(String t, String m, Throwable e) { return p(ASSERT, t, m, e); }
  public static int wtf(String t, Throwable e) { return p(ASSERT, t, "", e); }
  public static boolean isLoggable(String t, int l) { return l >= INFO; }
  public static int println(int l, String t, String m) { return p(l, t, m, null); }
  public static String getStackTraceString(Throwable tr) { if (tr == null) return ""; java.io.StringWriter sw = new java.io.StringWriter(); tr.printStackTrace(new java.io.PrintWriter(sw)); return sw.toString(); }
}
