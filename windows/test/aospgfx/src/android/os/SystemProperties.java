package android.os;
// The property service is ART's; on HotSpot answer from java system properties.
public class SystemProperties {
  public static String get(String k) { return System.getProperty(k, ""); }
  public static String get(String k, String d) { return System.getProperty(k, d); }
  public static int getInt(String k, int d) { try { return Integer.parseInt(System.getProperty(k)); } catch (Exception e) { return d; } }
  public static long getLong(String k, long d) { try { return Long.parseLong(System.getProperty(k)); } catch (Exception e) { return d; } }
  public static boolean getBoolean(String k, boolean d) { String v = System.getProperty(k); return v == null ? d : v.equals("1") || v.equals("true"); }
  public static void set(String k, String v) { System.setProperty(k, v); }
  public static void addChangeCallback(Runnable r) {}
}
