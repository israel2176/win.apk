// The text magnifier on AOSP 14's views, on a plain JVM (the harness of AospAppLifecycleTest, whose
// setUp this reuses): an activity with an EditText shows android.widget.Magnifier over it, as Editor does
// while a cursor or selection handle is dragged (Editor.MagnifierMotionAnimator.show -> Magnifier.show).
//
// Magnifier.show builds its popup as SurfaceFlinger layers: a container layer and a BLAST layer built
// with SurfaceControl.Builder(SurfaceSession) as children of the window's SurfaceControl, a
// BLASTBufferQueue's Surface, a ThreadedRenderer.SimpleRenderer drawing into it, and Transactions that
// place, raise and show the container. Win.apk's SurfaceControl lacked Builder(SurfaceSession), so this
// threw NoSuchMethodError out of the touch event and killed the app. Checked here:
//
//   * Magnifier.show does not throw, and its BLAST layer gets a layer of the popup's size (the content
//     and twice the shadow's room); dismiss releases it;
//   * the layer tree: a buffer layer built as a child of the window's control gets a layer of its size,
//     composed above the window at the window's place plus its position once a transaction shows it,
//     gone from the scene when hidden, and its layer released when the transaction removes it.
//
//   windows/test/aospapp/run.sh <stage> <deps>   (runs AospAppLifecycleTest, then this)
//
// Prints each check, then "ALL PASSED" or "FAILED: ..." and exits 0 or 1.

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.SurfaceControl;
import android.view.ViewTreeObserver;
import android.widget.EditText;
import android.widget.Magnifier;

import java.util.Arrays;
import java.util.Map;

public class MagnifierTest {
    static final String PKG = AospAppLifecycleTest.PKG;

    static native long[] scene();               // plainjvm_host.cpp: the last scene, {layer, x, y}...
    static native int[] layerSize(long layer);  // a layer's size, {} once released
    static native boolean bindSurfaceNatives();  // HardwareRenderer's natives that take a Surface

    static void check(boolean ok, String what) {
        System.out.println((ok ? "  ok: " : "  FAILED: ") + what);
        if (!ok) {
            System.out.println("FAILED: " + what + "; scene " + Arrays.toString(scene()));
            System.exit(1);
        }
    }

    /** The layers of the last scene, bottom to top. */
    static long[] layers() {
        long[] s = scene();
        long[] out = new long[s.length / 3];
        for (int i = 0; i < out.length; i++) out[i] = s[3 * i];
        return out;
    }

    static int indexOf(long layer) {
        long[] l = layers();
        for (int i = 0; i < l.length; i++) if (l[i] == layer) return i;
        return -1;
    }

    static long[] placeOf(long layer) {
        long[] s = scene();
        for (int i = 0; i < s.length; i += 3) if (s[i] == layer) return new long[] {s[i + 1], s[i + 2]};
        return null;
    }

    /** The layers the harness has made so far are numbered 1, 2, ...: the newest one. */
    static long newestLayer() {
        long n = 0;
        for (long l = 1; l < 1000; l++) if (layerSize(l).length == 2) n = l;
        return n;
    }

    /** After the main queue has run what is in it now (the compositor's scene is posted there). */
    static void later(Runnable r) {
        new Handler(Looper.getMainLooper()).postDelayed(r, 50);
    }

    public static class Typing extends Activity {
        EditText edit;

        @Override protected void onCreate(Bundle b) {
            super.onCreate(b);
            edit = new EditText(this);
            edit.setText("magnify me, please");
            setContentView(edit);
        }

        @Override protected void onResume() {
            super.onResume();
            edit.getViewTreeObserver().addOnGlobalLayoutListener(new ViewTreeObserver.OnGlobalLayoutListener() {
                @Override public void onGlobalLayout() {
                    edit.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                    later(() -> magnifier(Typing.this));
                }
            });
        }
    }

    /** The layer tree under the activity's window, with the public SurfaceControl API. */
    static void tree(Typing a) {
        SurfaceControl window = a.edit.getViewRootImpl().getSurfaceControl();
        check(window != null && window.isValid(), "the window has a SurfaceControl");
        long[] before = layers();
        check(before.length >= 1, "the window is in the scene: " + Arrays.toString(scene()));
        long windowLayer = before[before.length - 1];
        long[] windowAt = placeOf(windowLayer);

        SurfaceControl child = new SurfaceControl.Builder().setName("test child").setBufferSize(40, 30)
                .setParent(window).build();
        long layer = newestLayer();
        check(layer != windowLayer && Arrays.equals(layerSize(layer), new int[] {40, 30}),
                "a buffer layer built as the window's child has a layer of its size: " + layer);
        new SurfaceControl.Transaction().setPosition(child, 12, 34).setLayer(child, 5).apply();
        later(() -> {
            check(indexOf(layer) < 0, "hidden (the builder's default) until a transaction shows it");
            new SurfaceControl.Transaction().show(child).apply();
            later(() -> {
                int at = indexOf(layer);
                check(at == indexOf(windowLayer) + 1, "shown, it is composed right above its window");
                long[] p = placeOf(layer);
                check(p[0] == windowAt[0] + 12 && p[1] == windowAt[1] + 34,
                        "at the window's place plus its position: " + Arrays.toString(p));
                new SurfaceControl.Transaction().hide(child).apply();
                later(() -> {
                    check(indexOf(layer) < 0, "hidden by a transaction, it leaves the scene");
                    // Transaction.remove(child): reparent(child, null) and the handle released.
                    new SurfaceControl.Transaction().reparent(child, null).apply();
                    child.release();
                    check(layerSize(layer).length == 0, "removed, its layer is released");
                    System.out.println("ALL PASSED");
                    System.exit(0);
                });
            });
        });
    }

    /** What Editor does while a handle is dragged. */
    static void magnifier(Typing a) {
        long windowLayer = newestLayer();
        Magnifier m;
        try {
            m = new Magnifier.Builder(a.edit).build();
            m.show(a.edit.getWidth() / 2f, a.edit.getHeight() / 2f);
        } catch (Throwable t) {
            t.printStackTrace(System.out);
            check(false, "Magnifier.show threw " + t);
            return;
        }
        check(true, "Magnifier.show did not throw");
        long layer = newestLayer();
        int w = m.getWidth(), h = m.getHeight();
        int[] size = layerSize(layer);
        check(layer != windowLayer && size.length == 2 && size[0] >= w && size[1] >= h,
                "its popup has a layer of at least its size " + w + "x" + h + ": " + Arrays.toString(size));
        later(() -> {
            m.dismiss();
            later(() -> {
                check(layerSize(layer).length == 0 && indexOf(layer) < 0, "dismissed, its layer is released");
                tree(a);
            });
        });
    }

    static final String MANIFEST = "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\""
            + " package=\"" + PKG + "\">\n"
            + "  <application android:name=\"android.app.Application\"/>\n"
            + "</manifest>\n";

    public static void main(String[] args) throws Exception {
        ApplicationInfo ai = AospAppLifecycleTest.setUp(MANIFEST, android.app.Application.class.getName());
        if (!bindSurfaceNatives()) throw new IllegalStateException("HardwareRenderer's Surface natives not bound");
        Map<String, ActivityInfo> activities = Map.of(Typing.class.getName(),
                AospAppLifecycleTest.activity(ai, Typing.class));
        Configuration config = new Configuration();
        config.setToDefaults();
        config.screenWidthDp = 360;
        config.screenHeightDp = 640;
        config.densityDpi = 320;
        config.orientation = Configuration.ORIENTATION_PORTRAIT;
        config.setLocale(java.util.Locale.US);
        Intent launch = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                .setClassName(PKG, Typing.class.getName());
        com.winapk.aospapp.AospApp.start(ai, config,
                (intent, type) -> intent.getComponent() != null ? activities.get(intent.getComponent().getClassName()) : null,
                MagnifierTest.class.getClassLoader(),
                AospAppLifecycleTest.packageManager(ai.sourceDir, ai), launch, () -> {});
        Thread watchdog = new Thread(() -> {
            try { Thread.sleep(60_000); } catch (InterruptedException e) { return; }
            System.out.println("FAILED: TIMEOUT");
            System.exit(1);
        });
        watchdog.setDaemon(true);
        watchdog.start();
        // Looper.loop()'s body; an exception out of a message fails the test (Win.apk's main loop
        // logs it through the host's log, which a plain JVM does not have).
        java.lang.reflect.Method next = android.os.MessageQueue.class.getDeclaredMethod("next");
        next.setAccessible(true);
        android.os.MessageQueue q = Looper.myQueue();
        for (;;) {
            android.os.Message msg = (android.os.Message) next.invoke(q);
            if (msg == null) return;
            try {
                if (msg.getTarget() != null) msg.getTarget().dispatchMessage(msg);
                else if (msg.getCallback() != null) msg.getCallback().run();
            } catch (Throwable t) {
                t.printStackTrace(System.out);
                check(false, "exception: " + t);
            }
        }
    }
}
