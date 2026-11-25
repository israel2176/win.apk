// DateTimeViewTest -- android.widget.DateTimeView, the time of a notification
// template (android:id/time, android:showRelative="true"), against AOSP 14's:
// the short time within today, the short date outside it; relative "now",
// "5m", "3h", "3d", "in 2d", "2y"; the accessibility text's plural form. No
// framework table on a plain JVM, so the strings are the English ones AOSP's
// default locale has.
//
//   javac -cp "<runtime>/lib/aosp-graphics.jar;<runtime>/lib/android-framework.jar;<runtime>/lib/macapk-loader.jar" -d out \
//         windows/test/java/DateTimeViewTest.java
//   MACAPK_LOG_LEVEL=7 java -Djava.awt.headless=true -Duser.language=en -Duser.country=US \
//         -cp "<runtime>/lib/android-framework.jar;<runtime>/lib/macapk-loader.jar;out" DateTimeViewTest

import android.content.Context;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.view.accessibility.AccessibilityNodeInfo;
import android.widget.DateTimeView;
import android.widget.TextView;
import java.io.File;
import java.text.DateFormat;
import java.util.Date;

public final class DateTimeViewTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "[PASS] " : "[FAIL] ") + what);
        if (!ok) failures++;
    }

    static final class PlainContext extends Context {
        @Override public String getPackageName() { return "test"; }
        @Override public Resources getResources() { return Resources.getSystem(); }
        @Override public AssetManager getAssets() { return null; }
        @Override public Context getApplicationContext() { return this; }
        @Override public ClassLoader getClassLoader() { return DateTimeViewTest.class.getClassLoader(); }
        @Override public File getFilesDir() { return null; }
        @Override public File getCacheDir() { return null; }
        @Override public File getDir(String name, int mode) { return null; }
    }

    private static String a11y(DateTimeView v) {
        AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain();
        v.onInitializeAccessibilityNodeInfoInternal(info);
        CharSequence t = info.getText();
        return t == null ? null : t.toString();
    }

    public static void main(String[] args) {
        DateTimeView v = new DateTimeView(new PlainContext());
        check(v instanceof TextView, "a TextView");
        check(!v.isShowRelativeTime(), "absolute until showRelative is set");
        check(v.getText().length() == 0, "no text before setTime");

        long now = System.currentTimeMillis();
        // A minute ago is today, or within 12 hours: the short time.
        long recent = now - 60_000L;
        String time = DateFormat.getTimeInstance(DateFormat.SHORT).format(new Date(recent - recent % 60_000L));
        v.setTime(recent);
        check(time.equals(v.getText().toString()), "today: the short time, " + v.getText());
        long old = now - 10L * 86_400_000L;
        v.setTime(old);
        String date = DateFormat.getDateInstance(DateFormat.SHORT).format(new Date(old - old % 60_000L));
        check(date.equals(v.getText().toString()), "ten days ago: the short date, " + v.getText());

        v.setShowRelativeTime(true);
        check(v.isShowRelativeTime(), "isShowRelativeTime after setShowRelativeTime(true)");
        v.setTime(now);
        check("now".equals(v.getText().toString()), "under a minute: now, " + v.getText());
        check("now".equals(a11y(v)), "accessibility: now");
        v.setTime(now - 5 * 60_000L - 1_000L);
        check("5m".equals(v.getText().toString()), "five minutes ago: 5m, " + v.getText());
        check("5 minutes ago".equals(a11y(v)), "accessibility: 5 minutes ago, " + a11y(v));
        v.setTime(now - 60_000L - 1_000L);
        check("1 minute ago".equals(a11y(v)), "accessibility: the =1 branch, " + a11y(v));
        v.setTime(now - 3 * 3_600_000L - 1_000L);
        check("3h".equals(v.getText().toString()), "three hours ago: 3h, " + v.getText());
        v.setTime(now - 3 * 86_400_000L);
        check("3d".equals(v.getText().toString()), "three days ago: 3d, " + v.getText());
        v.setTime(now + 2 * 86_400_000L + 5_000L);
        check("in 2d".equals(v.getText().toString()), "two days ahead: in 2d, " + v.getText());
        check("in 2 days".equals(a11y(v)), "accessibility: in 2 days, " + a11y(v));
        v.setTime(now - 2 * 31_449_600_000L - 5L);
        check("2y".equals(v.getText().toString()), "two years ago: 2y, " + v.getText());

        v.setShowRelativeTime(false);
        v.setTime(recent);
        check(time.equals(v.getText().toString()), "back to absolute: the short time");

        if (failures != 0) {
            System.out.println(failures + " FAILED");
            System.exit(1);
        }
        System.out.println("ALL PASSED");
    }
}
