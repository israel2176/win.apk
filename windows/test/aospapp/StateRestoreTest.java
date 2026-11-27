// An app's task across the end of its process, on the harness of AospAppLifecycleTest (whose setUp
// this reuses): on Android system_server keeps each activity's record, intent, result target and
// saved instance state while the app's process is gone, and the next process gets the task back, its
// top relaunched with its saved Bundle and the activity under it as it is revealed. Win.apk keeps
// them in the task's saved state (com.winapk.aospapp.SavedTask, apps\<name>\task-state.parcel).
//
// Each run is one process of the app; run.sh runs them in turn over one state file:
//
//   background        Root starts Detail for a result, Detail's EditText gets text, the task goes
//                     behind (the window minimized: both stopped, each saving its state), then the
//                     process is killed (Runtime.halt: nothing runs at its death, as SIGKILL)
//   foreground        the same with the task in front: Detail resumed (no saved state), Root stopped
//   shutdown          the Windows session ends with Detail in front (AppHost.shutdown: stopped, its
//                     state saved, nothing destroyed), then the process ends
//   crash             as background, then the framework's crash handler reports a crash
//                     (AppHost.processCrashed: the top is not kept)
//   expect-detail     the task comes back on Detail: its saved Bundle, its intent's extra, its
//                     EditText's text; Detail finishes with a result, Root comes back with its saved
//                     Bundle and the result before onResume; Back out of Root empties the task, and
//                     the saved state is gone
//   expect-root       the task comes back on Root alone (Detail had no state, ActivityRecord's
//                     handleAppDied), then the user closes the window (AppHost.terminate: the task
//                     dismissed) and the saved state is gone
//   expect-fresh      a fresh start: Root has no saved state
//
// A run given a third argument, `updated`, is the app after an update (another APK update time): what
// the older version left is dropped.
//
//   windows/test/aospapp/run.sh <stage> <deps>   (runs AospAppLifecycleTest, MagnifierTest, then these)
//
// Prints each callback, then "ALL PASSED" or "FAILED: ..." and exits 0 or 1 (the killing runs print
// "KILLED" and exit 0 by halt).

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.EditText;

import com.winapk.aospapp.AospAppHost;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

public class StateRestoreTest {
    static final String PKG = AospAppLifecycleTest.PKG;
    static String mode;
    static File state;
    static final List<String> log = new ArrayList<>();

    static synchronized void note(String s) {
        log.add(s);
        System.out.println("  " + s);
    }

    static void fail(String what) {
        System.out.println("FAILED: " + what + "; callbacks " + log);
        System.out.flush();
        Runtime.getRuntime().halt(1);
    }

    static void check(boolean ok, String what) {
        System.out.println((ok ? "  ok: " : "  FAILED: ") + what);
        if (!ok) fail(what);
    }

    static void pass() {
        System.out.println("ALL PASSED");
        System.out.flush();
        Runtime.getRuntime().halt(0);
    }

    /** The process killed (no Java runs at a SIGKILL), once the system thread has taken every report
     *  posted so far (pendingStarts runs on it), after `delayMs`. */
    static void killLater(long delayMs, String why) {
        new Thread(() -> {
            try { Thread.sleep(delayMs); } catch (InterruptedException e) { return; }
            AospAppHost.INSTANCE.pendingStarts();
            check(state.isFile(), "the task's saved state is on disk when the process dies (" + why + ")");
            System.out.println("KILLED (" + why + ")");
            System.out.flush();
            Runtime.getRuntime().halt(0);
        }).start();
    }

    static boolean expecting() { return mode.startsWith("expect-"); }

    public static class Root extends Activity {
        int n;

        @Override protected void onCreate(Bundle b) {
            super.onCreate(b);
            n = b != null ? b.getInt("n", -1) : 1;
            note("Root.onCreate " + (b == null ? "fresh" : "restored n=" + n));
            if (mode.equals("expect-fresh")) {
                check(b == null, "a fresh start: Root has no saved state");
                check(!state.exists(), "nothing of an earlier task is on disk");
                pass();
            }
            if (mode.equals("expect-detail") || mode.equals("expect-root")) {
                check(b != null && n == 1, "Root comes back with its saved Bundle (n=1): " + n);
            } else {
                check(b == null, "Root starts fresh in a " + mode + " run");
            }
        }

        @Override protected void onSaveInstanceState(Bundle out) {
            super.onSaveInstanceState(out);
            out.putInt("n", n);
            note("Root.onSaveInstanceState");
            if (mode.equals("foreground")) killLater(300, "in front, Root stopped behind Detail");
        }

        @Override protected void onActivityResult(int request, int result, Intent data) {
            note("Root.onActivityResult " + request + " " + result + " "
                    + (data != null ? data.getStringExtra("r") : null));
        }

        @Override protected void onResume() {
            super.onResume();
            note("Root.onResume");
            if (!expecting()) {
                startActivityForResult(new Intent(this, Detail.class).putExtra("item", 42), 5);
            } else if (mode.equals("expect-detail")) {
                check(log.contains("Root.onActivityResult 5 -1 back"),
                        "Detail's result reached the restored Root before its onResume");
                // Back at the task's root: the task is finished, and nothing of it is kept.
                finish();
            } else if (mode.equals("expect-root")) {
                check(!log.contains("Detail.onCreate"), "Detail, with no saved state when the process died, is not back");
                new Thread(() -> {
                    try {
                        // The user closes the window: the task is dismissed.
                        AospAppHost.INSTANCE.terminate(null).await();
                    } catch (InterruptedException e) {
                        return;
                    }
                    check(log.contains("Root.onDestroy"), "the window's close destroyed Root");
                    check(!state.exists(), "a task the user dismissed is not kept");
                    pass();
                }).start();
            }
        }

        @Override protected void onDestroy() {
            super.onDestroy();
            note("Root.onDestroy");
        }
    }

    public static class Detail extends Activity {
        EditText edit;
        String text;

        @Override protected void onCreate(Bundle b) {
            super.onCreate(b);
            text = b != null ? b.getString("text") : "new";
            int item = getIntent().getIntExtra("item", -1);
            note("Detail.onCreate " + (b == null ? "fresh" : "restored text=" + text) + " item=" + item);
            if (expecting()) {
                check(mode.equals("expect-detail"), "Detail comes back only when it had saved state");
                check(b != null && "draft".equals(text), "Detail comes back with its saved Bundle: " + text);
                check(item == 42, "Detail comes back with its intent's extra: " + item);
                check(!log.contains("Root.onCreate"), "Root, under Detail, is not made before it is revealed");
            }
            edit = new EditText(this);
            edit.setId(0x7f0a0001);
            setContentView(edit);
        }

        @Override protected void onRestoreInstanceState(Bundle b) {
            super.onRestoreInstanceState(b);
            note("Detail.onRestoreInstanceState");
        }

        @Override protected void onSaveInstanceState(Bundle out) {
            super.onSaveInstanceState(out);
            out.putString("text", "draft");
            note("Detail.onSaveInstanceState");
            if (mode.equals("background")) killLater(300, "behind, both activities stopped");
            if (mode.equals("crash")) {
                new Thread(() -> {
                    try { Thread.sleep(300); } catch (InterruptedException e) { return; }
                    AospAppHost.INSTANCE.processCrashed();
                    killLater(0, "a crash with the task behind");
                }).start();
            }
        }

        @Override protected void onResume() {
            super.onResume();
            note("Detail.onResume");
            if (expecting()) {
                check(log.contains("Detail.onRestoreInstanceState"), "onRestoreInstanceState ran before onResume");
                check("typed".equals(edit.getText().toString()),
                        "the EditText's text came back through its view state: " + edit.getText());
                setResult(RESULT_OK, new Intent().putExtra("r", "back"));
                finish();
                return;
            }
            edit.setText("typed");
            Handler h = new Handler(Looper.getMainLooper());
            switch (mode) {
                case "background":
                case "crash":
                    // The window minimized once Root is stopped: the task behind.
                    h.postDelayed(AospAppHost.INSTANCE::hostMinimized, 300);
                    break;
                case "shutdown":
                    h.postDelayed(() -> new Thread(() -> {
                        boolean stopped;
                        try {
                            stopped = AospAppHost.INSTANCE.shutdown().await(5, java.util.concurrent.TimeUnit.SECONDS);
                        } catch (InterruptedException e) {
                            return;
                        }
                        check(stopped, "the session's end stopped every activity");
                        check(log.contains("Detail.onSaveInstanceState"), "Detail saved its state at the session's end");
                        check(!log.contains("Detail.onDestroy"), "nothing was destroyed at the session's end");
                        killLater(0, "the session ended");
                    }).start(), 300);
                    break;
                default:
                    break;
            }
        }

        @Override protected void onDestroy() {
            super.onDestroy();
            note("Detail.onDestroy");
        }
    }

    static final String MANIFEST = "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\""
            + " package=\"" + PKG + "\">\n"
            + "  <application android:name=\"android.app.Application\"/>\n"
            + "</manifest>\n";

    public static void main(String[] args) throws Exception {
        mode = args[0];
        state = new File(args[1]);
        System.out.println("== " + mode);
        ApplicationInfo ai = AospAppLifecycleTest.setUp(MANIFEST, android.app.Application.class.getName());
        // The installed APK's update time, which the package manager reports as lastUpdateTime: the
        // same for every run unless this one is the app after an update.
        boolean updated = args.length > 2 && args[2].equals("updated");
        new File(ai.sourceDir).setLastModified(updated ? 1_700_000_100_000L : 1_700_000_000_000L);
        Map<String, ActivityInfo> activities = Map.of(
                Root.class.getName(), AospAppLifecycleTest.activity(ai, Root.class),
                Detail.class.getName(), AospAppLifecycleTest.activity(ai, Detail.class));
        Configuration config = new Configuration();
        config.setToDefaults();
        config.screenWidthDp = 360;
        config.screenHeightDp = 640;
        config.densityDpi = 320;
        config.orientation = Configuration.ORIENTATION_PORTRAIT;
        config.setLocale(java.util.Locale.US);
        Intent launch = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                .setClassName(PKG, Root.class.getName());
        com.winapk.aospapp.AospApp.start(ai, config,
                (intent, type) -> intent.getComponent() != null ? activities.get(intent.getComponent().getClassName()) : null,
                StateRestoreTest.class.getClassLoader(),
                AospAppLifecycleTest.packageManager(ai.sourceDir, ai), launch,
                () -> {
                    note("task empty");
                    check(mode.equals("expect-detail"), "the task empties only by Back out of Root");
                    check(!state.exists(), "a task finished by Back is not kept");
                    pass();
                },
                state);
        Thread watchdog = new Thread(() -> {
            try { Thread.sleep(60_000); } catch (InterruptedException e) { return; }
            fail("TIMEOUT");
        });
        watchdog.setDaemon(true);
        watchdog.start();
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
                fail("exception: " + t);
            }
        }
    }
}
