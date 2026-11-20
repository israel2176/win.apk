package aospviews.sys;

/** Bodies for the java.* members only ART's libcore has, which AOSP's framework calls (RemapJdk
 *  redirects each call site here). Win.apk would put these in its java.base overlay instead
 *  (windows/java/java_base_patch), where the framework's own calls already land. */
public final class Art {
    private Art() {}

    // libcore's System.logX: ART's own log, tag "System".
    public static void logW(String m) { android.util.Log.w("System", m); }
    public static void logW(String m, Throwable t) { android.util.Log.w("System", m, t); }
    public static void logE(String m) { android.util.Log.e("System", m); }
    public static void logE(String m, Throwable t) { android.util.Log.e("System", m, t); }
    public static void logI(String m) { android.util.Log.i("System", m); }
    public static void logI(String m, Throwable t) { android.util.Log.i("System", m, t); }
}
