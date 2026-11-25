// What a screen reader is given of an app's views, and what it can do with them, on AOSP 14's own
// views on a plain JVM (com.macapk.loader.HostAccessibility, com.winapk.aospviews.AccessibilityTree),
// in an activity AOSP's ActivityThread runs (lib/aosp-app.jar, as windows/test/aospapp's test).
//
// The screen: a heading, a clickable row (two texts and a switch), an editor labelled by a text,
// a checkbox, a seek bar, a polite live region, a custom view with two virtual keys behind an
// AccessibilityNodeProvider, and a Hebrew selectable text. The test plays the host: it turns the
// screen reader on (hostActivate) and checks that AOSP's accessibility turned on, that a snapshot
// came with no one asking, and what it says; then it acts as a client would (hostPerform: a tap,
// a text, a selection, a seek, a virtual key's tap), changes a live region and announces, and
// checks each reaches the views and comes back as a snapshot or an announcement, by the views' own
// accessibility events. The last snapshot is written for run.sh's C++ check (snapshot_check.cpp).
//
//   windows/test/accessibility/run.sh <stage> <deps>
//
// Prints each check, then "ALL PASSED" or the failures, and exits 0 or 1.

import android.app.Activity;
import android.app.Application;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

import com.macapk.loader.HostAccessibility;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.function.BooleanSupplier;

public class AccessibilityTreeTest {
    static final String PKG = "com.winapk.a11ytest";
    static int failures;
    static final List<String> snapshots = new ArrayList<>();
    static final List<String> announcements = new ArrayList<>();
    static Screen screen;
    static final List<String> virtualClicks = new ArrayList<>();

    static void check(boolean ok, String what) {
        System.out.println((ok ? "  ok   " : "  FAIL ") + what);
        if (!ok) failures++;
    }

    // ---- The app ----------------------------------------------------------------------------------

    public static class App extends Application {}

    /** A view drawing two keys, each a virtual node a screen reader can tap. */
    static class Keys extends View {
        final AccessibilityNodeProvider provider = new AccessibilityNodeProvider() {
            @Override public AccessibilityNodeInfo createAccessibilityNodeInfo(int id) {
                if (id == HOST_VIEW_ID) {
                    AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain(Keys.this);
                    onInitializeAccessibilityNodeInfo(info);
                    info.addChild(Keys.this, 1);
                    info.addChild(Keys.this, 2);
                    info.setClassName("android.widget.FrameLayout");
                    return info;
                }
                if (id != 1 && id != 2) return null;
                AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain(Keys.this, id);
                info.setPackageName(getContext().getPackageName());
                info.setClassName("android.widget.Button");
                info.setParent(Keys.this);
                info.setText(id == 1 ? "Key A" : "Key B");
                info.setEnabled(true);
                info.setClickable(true);
                info.setVisibleToUser(true);
                int[] at = new int[2];
                getLocationOnScreen(at);
                int half = getWidth() / 2;
                info.setBoundsInScreen(new android.graphics.Rect(at[0] + (id - 1) * half, at[1], at[0] + id * half,
                        at[1] + getHeight()));
                info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_CLICK);
                return info;
            }

            @Override public boolean performAction(int id, int action, Bundle args) {
                if (action != AccessibilityNodeInfo.ACTION_CLICK) return false;
                virtualClicks.add(id == 1 ? "A" : id == 2 ? "B" : "host");
                return true;
            }
        };

        Keys(android.content.Context c) {
            super(c);
            setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
        }

        @Override public AccessibilityNodeProvider getAccessibilityNodeProvider() { return provider; }
    }

    public static class Screen extends Activity {
        TextView heading, title, summary, label, live, hebrew;
        LinearLayout row;
        Switch toggle;
        EditText edit;
        CheckBox box;
        SeekBar seek;
        Keys keys;

        @Override protected void onCreate(Bundle b) {
            super.onCreate(b);
            setTitle("Settings");
            LinearLayout root = new LinearLayout(this);
            root.setOrientation(LinearLayout.VERTICAL);
            heading = text("Network");
            heading.setAccessibilityHeading(true);
            root.addView(heading);
            row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setClickable(true);
            row.setFocusable(true);
            row.setOnClickListener(v -> {});
            title = text("Wi-Fi");
            summary = text("Connected");
            toggle = new Switch(this);
            toggle.setChecked(true);
            row.addView(title);
            row.addView(summary);
            row.addView(toggle);
            root.addView(row);
            label = text("Name");
            root.addView(label);
            edit = new EditText(this);
            edit.setId(View.generateViewId());
            edit.setText("Dana Levi");
            edit.setHint("First and last name");
            label.setLabelFor(edit.getId());
            root.addView(edit, new ViewGroup.LayoutParams(600, ViewGroup.LayoutParams.WRAP_CONTENT));
            box = new CheckBox(this);
            box.setText("Remember me");
            root.addView(box);
            seek = new SeekBar(this);
            seek.setMax(10);
            seek.setProgress(3);
            root.addView(seek, new ViewGroup.LayoutParams(600, ViewGroup.LayoutParams.WRAP_CONTENT));
            live = text("0 items");
            live.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
            root.addView(live);
            keys = new Keys(this);
            root.addView(keys, new ViewGroup.LayoutParams(400, 100));
            hebrew = text("שלום עולם");
            hebrew.setTextIsSelectable(true);
            root.addView(hebrew);
            setContentView(root);
        }

        TextView text(String s) {
            TextView t = new TextView(this);
            t.setText(s);
            return t;
        }

        @Override protected void onResume() {
            super.onResume();
            screen = this;
            new Handler(Looper.getMainLooper()).postDelayed(AccessibilityTreeTest::run, 200);
        }
    }

    // ---- The checks -------------------------------------------------------------------------------

    /** One node of a snapshot: its fields by position (uia_tree.h's N record). */
    static final class N {
        final String[] f;

        N(String[] f) { this.f = f; }

        long id() { return Long.parseLong(f[1]); }
        long parent() { return Long.parseLong(f[2]); }
        int flags() { return Integer.parseInt(f[3]); }
        String cls() { return f[5]; }
        String desc() { return f[7]; }
        String text() { return f[8]; }
        String hint() { return f[9]; }
        int live() { return Integer.parseInt(f[15]); }
        String range() { return f[16]; }
        String selection() { return f[19]; }
        String lines() { return f[20]; }
        String charX() { return f[22]; }
        String words() { return f[23]; }
        String children() { return f[24]; }
        String actions() { return f[25]; }
        long labeledBy() { return Long.parseLong(f[26]); }
        boolean has(int flag) { return (flags() & flag) != 0; }
        boolean action(int id) {
            for (String a : actions().split(",")) {
                if (a.equals(Integer.toString(id))) return true;
            }
            return false;
        }
    }

    static final class Snap {
        String[] header;
        final List<String[]> windows = new ArrayList<>();
        final Map<Long, N> nodes = new HashMap<>();

        static Snap of(String text) {
            Snap s = new Snap();
            for (String r : text.split("\u001e")) {
                String[] f = r.split("\u001f", -1);
                if (f[0].equals("S")) s.header = f;
                else if (f[0].equals("W")) s.windows.add(f);
                else if (f[0].equals("N")) {
                    N n = new N(f);
                    s.nodes.put(n.id(), n);
                }
            }
            return s;
        }

        N byText(String t) {
            for (N n : nodes.values()) {
                if (n.text().equals(t)) return n;
            }
            return null;
        }

        N byClass(String c) {
            for (N n : nodes.values()) {
                if (n.cls().equals(c)) return n;
            }
            return null;
        }

        long focused() { return Long.parseLong(header[2]); }
    }

    static final int ENABLED = 1, FOCUSABLE = 2, FOCUSED = 4, CLICKABLE = 8, CHECKABLE = 32, CHECKED = 64,
            EDITABLE = 128, VISIBLE = 512, HEADING = 1 << 11, RTL = 1 << 16, TEXT_SELECTABLE = 1 << 18;

    static long viewId(View v) {
        return AccessibilityNodeInfo.makeNodeId(v.getAccessibilityViewId(), AccessibilityNodeInfo.UNDEFINED_ITEM_ID);
    }

    static Snap last() { return Snap.of(snapshots.get(snapshots.size() - 1)); }

    static final Handler main = new Handler(Looper.getMainLooper());

    /** Runs `then` once `cond` holds, checking every 20 ms for up to 3 s. */
    static void await(String what, BooleanSupplier cond, Runnable then) {
        long until = android.os.SystemClock.uptimeMillis() + 3000;
        Runnable poll = new Runnable() {
            @Override public void run() {
                if (cond.getAsBoolean()) {
                    then.run();
                } else if (android.os.SystemClock.uptimeMillis() > until) {
                    check(false, what + " (timed out)");
                    finish();
                } else {
                    main.postDelayed(this, 20);
                }
            }
        };
        main.post(poll);
    }

    static void run() {
        AccessibilityManager am = screen.getSystemService(AccessibilityManager.class);
        check(!am.isEnabled(), "no screen reader yet: accessibility off");
        HostAccessibility.install(new HostAccessibility.Sink() {
            @Override public boolean publish(String s) {
                snapshots.add(s);
                return true;
            }

            @Override public void announce(String text, boolean assertive) { announcements.add(text); }
        });
        screen.edit.requestFocus();
        HostAccessibility.hostActivate(true);
        await("a snapshot once a screen reader reads", () -> !snapshots.isEmpty(), () -> {
            check(am.isEnabled(), "a screen reader reads: AccessibilityManager.isEnabled()");
            check(HostAccessibility.active(), "HostAccessibility.active()");
            firstSnapshot();
            actions();
        });
    }

    static void firstSnapshot() {
        Snap s = last();
        check(s.header != null && s.header[1].equals("1"), "the header, version 1");
        check(s.windows.size() == 1, "one window");
        if (!s.windows.isEmpty()) {
            check(s.windows.get(0)[3].equals("Settings"), "the window's title is the activity's (accessibilityTitle)");
            check(s.nodes.containsKey(Long.parseLong(s.windows.get(0)[1])), "the window's root is a node");
        }
        Screen a = screen;
        N heading = s.nodes.get(viewId(a.heading));
        check(heading != null && heading.has(HEADING) && heading.text().equals("Network"), "the heading");
        N row = s.nodes.get(viewId(a.row));
        check(row != null && row.has(CLICKABLE) && row.action(AccessibilityNodeInfo.ACTION_CLICK), "the clickable row");
        if (row != null) {
            check(row.children().equals(viewId(a.title) + "," + viewId(a.summary) + "," + viewId(a.toggle)),
                    "the row's children in reading order");
        }
        N sw = s.nodes.get(viewId(a.toggle));
        check(sw != null && sw.has(CHECKABLE) && sw.has(CHECKED) && sw.cls().equals("android.widget.Switch"), "the switch, on");
        if (sw != null) check(sw.parent() == viewId(a.row), "the switch's parent is the row");
        N edit = s.nodes.get(viewId(a.edit));
        check(edit != null && edit.has(EDITABLE) && edit.text().equals("Dana Levi"), "the editor and its text");
        if (edit != null) {
            check(edit.hint().equals("First and last name"), "the editor's hint");
            check(edit.labeledBy() == viewId(a.label),
                    "the editor is labelled by its label (setLabelFor): " + edit.labeledBy() + " " + viewId(a.label));
            check(edit.action(AccessibilityNodeInfo.ACTION_SET_TEXT), "the editor takes a text");
            check(edit.lines().equals("0"), "one line, from the TextView's layout");
            check(edit.charX().split(",").length == "Dana Levi".length() + 1, "the x of every offset");
            check(("," + edit.words() + ",").contains(",4,") && ("," + edit.words() + ",").contains(",5,"),
                    "BreakIterator's word breaks around the space");
            check(edit.selection().equals(a.edit.getSelectionStart() + "," + a.edit.getSelectionEnd()),
                    "the editor's caret: " + edit.selection());
            check(s.focused() == viewId(a.edit), "the focused node is the editor");
        }
        N box = s.nodes.get(viewId(a.box));
        check(box != null && box.has(CHECKABLE) && !box.has(CHECKED) && box.text().equals("Remember me"), "the checkbox");
        N seek = s.nodes.get(viewId(a.seek));
        check(seek != null && seek.range().startsWith("0,0.0,10.0,3.0"), "the seek bar's range (int, 0..10, at 3)");
        if (seek != null) check(seek.action(android.R.id.accessibilityActionSetProgress), "the seek bar takes a progress");
        N live = s.nodes.get(viewId(a.live));
        check(live != null && live.live() == View.ACCESSIBILITY_LIVE_REGION_POLITE, "the polite live region");
        N keys = s.nodes.get(viewId(a.keys));
        N keyA = s.byText("Key A"), keyB = s.byText("Key B");
        check(keys != null && keyA != null && keyB != null, "the provider's host node and its two virtual keys");
        if (keys != null && keyA != null && keyB != null) {
            check(keyA.parent() == keys.id() && keyB.parent() == keys.id(), "the keys' parent is their view");
            check(keys.children().equals(keyA.id() + "," + keyB.id()), "the view lists its keys");
            check(AccessibilityNodeInfo.getVirtualDescendantId(keyB.id()) == 2, "a key's id holds its virtual id");
        }
        N hebrew = s.nodes.get(viewId(a.hebrew));
        check(hebrew != null && hebrew.has(TEXT_SELECTABLE) && hebrew.has(RTL), "the Hebrew text runs right to left");
        if (hebrew != null) check(!hebrew.words().isEmpty(), "and has word breaks");
        for (N n : s.nodes.values()) {
            if (n.parent() != 0 && !s.nodes.containsKey(n.parent())) {
                check(false, "every parent is in the snapshot (" + n.id() + ")");
                break;
            }
        }
    }

    static void actions() {
        Screen a = screen;
        Snap s = last();
        long keyB = s.byText("Key B").id();
        int before = snapshots.size();
        HostAccessibility.hostPerform(viewId(a.toggle), AccessibilityNodeInfo.ACTION_CLICK, "", 0, 0, 0);
        HostAccessibility.hostPerform(viewId(a.edit), AccessibilityNodeInfo.ACTION_SET_TEXT, "Noa Cohen", 0, 0, 0);
        HostAccessibility.hostPerform(viewId(a.seek), android.R.id.accessibilityActionSetProgress, "", 0, 0, 7);
        HostAccessibility.hostPerform(keyB, AccessibilityNodeInfo.ACTION_CLICK, "", 0, 0, 0);
        await("the actions reach the views", () -> !a.toggle.isChecked() && a.edit.getText().toString().equals("Noa Cohen")
                && a.seek.getProgress() == 7 && virtualClicks.contains("B"), () -> {
            check(true, "a tap toggled the switch; a text, a progress and a virtual key's tap arrived");
            await("a snapshot shows them", () -> {
                if (snapshots.size() <= before) return false;
                N e = last().nodes.get(viewId(a.edit));
                return e != null && e.text().equals("Noa Cohen");
            }, () -> {
                Snap t = last();
                check(!t.nodes.get(viewId(a.toggle)).has(CHECKED), "the new snapshot: the switch is off");
                check(t.nodes.get(viewId(a.seek)).range().endsWith(",7.0"), "the new snapshot: the seek bar at 7");
                selection();
            });
        });
    }

    static void selection() {
        Screen a = screen;
        HostAccessibility.hostPerform(viewId(a.edit), AccessibilityNodeInfo.ACTION_SET_SELECTION, "", 0, 3, 0);
        await("a selection reaches the editor", () -> a.edit.getSelectionStart() == 0 && a.edit.getSelectionEnd() == 3, () -> {
            check(true, "ACTION_SET_SELECTION selected 0..3");
            await("the selection's snapshot", () -> {
                N e = last().nodes.get(viewId(a.edit));
                return e != null && e.selection().equals("0,3");
            }, () -> {
                check(true, "the snapshot says 0,3");
                liveRegion();
            });
        });
    }

    static void liveRegion() {
        Screen a = screen;
        int before = snapshots.size();
        // No host call: the view's own content-change event brings the snapshot.
        a.live.setText("1 item");
        await("a live region's change sends a snapshot by itself", () -> {
            if (snapshots.size() <= before) return false;
            N l = last().nodes.get(viewId(a.live));
            return l != null && l.text().equals("1 item");
        }, () -> {
            check(true, "the live region's new text, from the view's own event");
            a.live.announceForAccessibility("Saved");
            await("an announcement", () -> announcements.contains("Saved"), () -> {
                check(true, "announceForAccessibility is spoken");
                throttle();
            });
        });
    }

    /** Many events in a burst: snapshots no closer than 100 ms. */
    static void throttle() {
        Screen a = screen;
        int before = snapshots.size();
        long start = android.os.SystemClock.uptimeMillis();
        for (int i = 0; i < 30; i++) a.live.setText(i + " items");
        main.postDelayed(() -> {
            long took = android.os.SystemClock.uptimeMillis() - start;
            int sent = snapshots.size() - before;
            check(sent >= 1 && sent <= took / 100 + 2, "30 changes in a burst: " + sent + " snapshots in " + took + " ms");
            off();
        }, 350);
    }

    static void off() {
        Screen a = screen;
        HostAccessibility.hostActivate(false);
        AccessibilityManager am = a.getSystemService(AccessibilityManager.class);
        await("the screen reader stops", () -> !HostAccessibility.active() && !am.isEnabled(), () -> {
            check(true, "accessibility is off again");
            int before = snapshots.size();
            a.live.setText("no one reads this");
            main.postDelayed(() -> {
                check(snapshots.size() == before, "no snapshot once off");
                finish();
            }, 300);
        });
    }

    // ---- The harness (as windows/test/aospapp/AospAppLifecycleTest.java starts an app) -------------

    static native boolean redirectBegin();   // a11y_natives.cpp
    static native int redirectEnd();

    static void finish() {
        if (!snapshots.isEmpty()) {
            String out = System.getProperty("winapk.test.snapshot");
            if (out != null) {
                try {
                    java.nio.file.Files.write(new File(out).toPath(), snapshots.get(snapshots.size() - 1)
                            .getBytes(StandardCharsets.UTF_8));
                } catch (java.io.IOException e) {
                    check(false, "write " + out + ": " + e);
                }
            }
        }
        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }

    public static void main(String[] args) throws Exception {
        String fw = System.getProperty("winapk.aosp.res.framework");
        if (fw == null || !new File(fw).isFile()) throw new IllegalStateException("-Dwinapk.aosp.res.framework=<framework-res-34.apk>");
        String host = System.getProperty("winapk.test.host");
        if (host == null || !new File(host).isFile()) throw new IllegalStateException("-Dwinapk.test.host=<host library>");
        System.load(new File(host).getAbsolutePath());
        // This plain JVM has Robolectric's native graphics, not the GPU DLL (no render thread): views
        // draw in software here, as Android's own tests disable hardware rendering.
        // (reflection: the class is hidden API, not in the jar this test compiles against)
        java.lang.reflect.Field rendererEnabled = Class.forName("android.view.ThreadedRenderer").getField("sRendererEnabled");
        rendererEnabled.setBoolean(null, false);  // what ViewRootImpl.enableHardwareAcceleration reads
        String runtime = System.getProperty("winapk.test.gfx.runtime");
        if (runtime == null) throw new IllegalStateException("-Dwinapk.test.gfx.runtime=<robolectric-nativeruntime>");
        if (!redirectBegin()) throw new IllegalStateException("the redirect did not install");
        System.load(new File(runtime).getAbsolutePath());
        if (redirectEnd() < 600) throw new IllegalStateException("graphics natives not bound");
        if (com.macapk.loader.gfx.GfxBackend.get().materializeSystemFonts(name -> null) == null) {
            throw new IllegalStateException("no system fonts (-Dwinapk.aosp.fonts=<fonts dir>)");
        }
        File data = java.nio.file.Files.createTempDirectory("a11ytest").toFile();
        Looper.prepareMainLooper();
        com.winapk.aospres.AospRes.INSTANCE.openAssets(fw);

        ApplicationInfo ai = new ApplicationInfo();
        ai.packageName = PKG;
        ai.processName = PKG;
        ai.className = App.class.getName();
        ai.sourceDir = ai.publicSourceDir = fw;
        ai.dataDir = ai.credentialProtectedDataDir = new File(data, "user").getPath();
        ai.deviceProtectedDataDir = new File(data, "user_de").getPath();
        new File(ai.dataDir).mkdirs();
        new File(ai.deviceProtectedDataDir).mkdirs();
        ai.nativeLibraryDir = new File(data, "lib").getPath();
        ai.targetSdkVersion = 34;
        ai.uid = android.os.Process.myUid();
        ai.enabled = true;
        ai.flags = ApplicationInfo.FLAG_HAS_CODE;

        ActivityInfo screenInfo = new ActivityInfo();
        screenInfo.name = Screen.class.getName();
        screenInfo.packageName = PKG;
        screenInfo.processName = PKG;
        screenInfo.applicationInfo = ai;
        screenInfo.theme = android.R.style.Theme_DeviceDefault;
        screenInfo.exported = true;
        screenInfo.enabled = true;
        screenInfo.screenOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;

        Configuration config = new Configuration();
        config.setToDefaults();
        config.screenWidthDp = 360;
        config.screenHeightDp = 640;
        config.densityDpi = 320;
        config.orientation = Configuration.ORIENTATION_PORTRAIT;
        config.setLocale(java.util.Locale.US);

        Intent launch = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                .setClassName(PKG, Screen.class.getName());
        com.winapk.aospapp.AospApp.start(ai, config,
                (intent, type) -> intent.getComponent() != null
                        && intent.getComponent().getClassName().equals(Screen.class.getName()) ? screenInfo : null,
                AccessibilityTreeTest.class.getClassLoader(),
                new com.macapk.loader.MacApkPackageManager(PKG, fw, ai.dataDir, ai.nativeLibraryDir), launch,
                () -> {});
        Thread watchdog = new Thread(() -> {
            try { Thread.sleep(60_000); } catch (InterruptedException e) { return; }
            System.out.println("TIMEOUT");
            System.exit(1);
        });
        watchdog.setDaemon(true);
        watchdog.start();
        loop();
    }

    /** Looper.loop()'s body, but an exception out of a message fails the test. */
    static void loop() throws Exception {
        java.lang.reflect.Method next = android.os.MessageQueue.class.getDeclaredMethod("next");
        next.setAccessible(true);
        android.os.MessageQueue q = Looper.myQueue();
        for (;;) {
            android.os.Message m = (android.os.Message) next.invoke(q);
            if (m == null) return;
            try {
                if (m.getTarget() != null) m.getTarget().dispatchMessage(m);
                else if (m.getCallback() != null) m.getCallback().run();
            } catch (Throwable t) {
                t.printStackTrace(System.out);
                check(false, "exception: " + t);
                finish();
            }
        }
    }
}
