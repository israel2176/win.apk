// AOSP 14's own activity lifecycle on a plain JVM (docs/research/aosp-app.md, step A1): AOSP's
// ActivityThread, TransactionExecutor, Instrumentation, ContextImpl, LoadedApk and Activity
// (lib/aosp-app.jar, windows/scripts/build_aosp_app.sh) over the in-process activity manager, task
// manager and client controller (windows/java/aosp_app), with AOSP's views, resources and graphics
// beneath, as a Win.apk run would have them with the app layer on.
//
// The app is this file's: an Application and two activities with no content. Each one's window is
// added on resume to AOSP's WindowManagerGlobal and the views' in-process window manager
// (com.winapk.aospviews.AospWindows), as on a device; nothing here presents a frame to a host window.
// First starts Second for a result; Second sets it and, once First is stopped, finishes; First
// finishes once Second is destroyed; the task empties. Every callback is recorded and the order
// compared with the one Android gives: the new activity resumes before the old one stops (it stops
// when the new one is idle); onSaveInstanceState after onStop (API 28+); the result after onStart
// and before onResume (API 30's CALL_ACTIVITY_RESULT_BEFORE_RESUME); the finishing activity stopped
// and destroyed after the one under it resumes.
//
//   windows/test/aospapp/run.sh <stage> <deps>   (builds aosp-app.jar, the host library and this)
//
// Prints each callback, then "ALL PASSED" or the first difference, and exits 0 or 1.

import android.app.Activity;
import android.app.Application;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Looper;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

public class AospAppLifecycleTest {
    static final String PKG = "com.winapk.lifecycletest";
    static final List<String> log = new ArrayList<>();

    static synchronized void note(String s) {
        log.add(s);
        System.out.println("  " + s);
    }

    public static class App extends Application {
        @Override public void onCreate() {
            super.onCreate();
            note("App.onCreate");
        }
    }

    public static class Base extends Activity {
        String n() { return getClass().getSimpleName(); }
        @Override protected void onCreate(Bundle b) { super.onCreate(b); note(n() + ".onCreate"); }
        @Override protected void onStart() { super.onStart(); note(n() + ".onStart"); }
        @Override protected void onRestart() { super.onRestart(); note(n() + ".onRestart"); }
        @Override protected void onResume() { super.onResume(); note(n() + ".onResume"); }
        @Override protected void onPause() { super.onPause(); note(n() + ".onPause"); }
        @Override protected void onStop() { super.onStop(); note(n() + ".onStop"); }
        @Override protected void onDestroy() { super.onDestroy(); note(n() + ".onDestroy"); }
        @Override protected void onSaveInstanceState(Bundle out) { super.onSaveInstanceState(out); note(n() + ".onSaveInstanceState"); }
    }

    public static class First extends Base {
        boolean started;

        @Override protected void onResume() {
            super.onResume();
            first = this;
            if (!started) {
                started = true;
                note("First: getApplication() is App: " + (getApplication() instanceof App));
                startActivityForResult(new Intent(this, Second.class).putExtra("x", 7), 42);
            }
        }

        @Override protected void onSaveInstanceState(Bundle out) {
            super.onSaveInstanceState(out);
            new android.os.Handler(Looper.getMainLooper()).post(() -> second.finish());
        }

        @Override protected void onActivityResult(int request, int result, Intent data) {
            super.onActivityResult(request, result, data);
            note("First.onActivityResult " + request + " " + result + " " + (data != null ? data.getStringExtra("r") : null));
        }
    }

    /** Second finishes once First is stopped (First's onSaveInstanceState, after its onStop), so the
     *  way back passes through onRestart; First finishes once Second is destroyed. Each is posted,
     *  as an app's click would be. */
    static Second second;
    static First first;

    public static class Second extends Base {
        @Override protected void onResume() {
            super.onResume();
            second = this;
            note("Second: extra x=" + getIntent().getIntExtra("x", -1) + ", calling " + getCallingActivity().getShortClassName());
            setResult(RESULT_OK, new Intent().putExtra("r", "done"));
        }

        @Override protected void onDestroy() {
            super.onDestroy();
            new android.os.Handler(Looper.getMainLooper()).post(() -> first.finish());
        }
    }

    static final List<String> EXPECTED = List.of(
            "App.onCreate",
            "First.onCreate", "First.onStart", "First.onResume",
            "First: getApplication() is App: true",
            "First.onPause",
            "Second.onCreate", "Second.onStart", "Second.onResume",
            "Second: extra x=7, calling AospAppLifecycleTest$First",
            "First.onStop", "First.onSaveInstanceState",
            "Second.onPause",
            "First.onRestart", "First.onStart",
            "First.onActivityResult 42 -1 done",
            "First.onResume",
            "Second.onStop", "Second.onDestroy",
            "First.onPause", "First.onStop", "First.onDestroy",
            "task empty");

    static native boolean redirectBegin();   // plainjvm_host.cpp
    static native int redirectEnd();

    public static void main(String[] args) throws Exception {
        String fw = System.getProperty("winapk.aosp.res.framework");
        if (fw == null || !new File(fw).isFile()) throw new IllegalStateException("-Dwinapk.aosp.res.framework=<framework-res-34.apk>");
        String host = System.getProperty("winapk.test.host");   // plainjvm_host.c, built
        if (host == null || !new File(host).isFile()) throw new IllegalStateException("-Dwinapk.test.host=<libplainjvm_host>");
        System.load(new File(host).getAbsolutePath());
        // AOSP's graphics, as winapk-runtime binds them (host/aosp_gfx.cpp): Robolectric's native
        // runtime onto AOSP's classes through the host's own redirect (plainjvm_host.cpp), ICU's data, then
        // the system fonts (AospGfx.materializeSystemFonts, what Boot does): an activity's decor
        // reaches ColorSpace and Typeface.
        String runtime = System.getProperty("winapk.test.gfx.runtime");
        if (runtime == null) throw new IllegalStateException("-Dwinapk.test.gfx.runtime=<robolectric-nativeruntime>");
        if (!redirectBegin()) throw new IllegalStateException("the redirect did not install");
        System.load(new File(runtime).getAbsolutePath());
        int bound = redirectEnd();
        if (bound < 600) throw new IllegalStateException("graphics natives bound: " + bound);
        if (com.macapk.loader.gfx.GfxBackend.get().materializeSystemFonts(name -> null) == null) {
            throw new IllegalStateException("no system fonts (-Dwinapk.aosp.fonts=<fonts dir>)");
        }
        File data = java.nio.file.Files.createTempDirectory("aospapp").toFile();
        Looper.prepareMainLooper();
        // The framework's resources as the system AssetManager, as Boot has them before the app binds
        // (AospRes: AssetManager.createSystemAssetsInZygoteLocked without overlays).
        com.winapk.aospres.AospRes.INSTANCE.openAssets(fw);

        ApplicationInfo ai = new ApplicationInfo();
        ai.packageName = PKG;
        ai.processName = PKG;
        ai.className = App.class.getName();
        // The resources the app's LoadedApk opens: any APK will do, the framework's by default.
        String apk = System.getProperty("winapk.test.apk", fw);
        if (!new File(apk).isFile()) throw new IllegalStateException("no " + apk);
        ai.sourceDir = ai.publicSourceDir = apk;
        // As the package manager fills them: /data/user/0/<pkg> (credential protected, the default
        // storage) and /data/user_de/0/<pkg>.
        ai.dataDir = ai.credentialProtectedDataDir = new File(data, "user").getPath();
        ai.deviceProtectedDataDir = new File(data, "user_de").getPath();
        new File(ai.dataDir).mkdirs();
        new File(ai.deviceProtectedDataDir).mkdirs();
        ai.nativeLibraryDir = new File(data, "lib").getPath();
        ai.targetSdkVersion = 34;
        ai.uid = android.os.Process.myUid();
        ai.enabled = true;
        ai.flags = ApplicationInfo.FLAG_HAS_CODE;

        Map<String, ActivityInfo> activities = Map.of(
                First.class.getName(), activity(ai, First.class),
                Second.class.getName(), activity(ai, Second.class));
        Configuration config = new Configuration();
        config.setToDefaults();
        config.screenWidthDp = 360;
        config.screenHeightDp = 640;
        config.densityDpi = 320;
        config.orientation = Configuration.ORIENTATION_PORTRAIT;
        config.setLocale(java.util.Locale.US);

        Intent launch = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                .setClassName(PKG, First.class.getName());
        com.winapk.aospapp.AospApp.start(ai, config,
                (intent, type) -> intent.getComponent() != null ? activities.get(intent.getComponent().getClassName()) : null,
                AospAppLifecycleTest.class.getClassLoader(),
                new com.macapk.loader.MacApkPackageManager(PKG, apk, ai.dataDir, ai.nativeLibraryDir), launch,
                () -> {
                    note("task empty");
                    finish();
                });
        Thread watchdog = new Thread(() -> {
            try { Thread.sleep(60_000); } catch (InterruptedException e) { return; }
            System.out.println("TIMEOUT");
            finish();
        });
        watchdog.setDaemon(true);
        watchdog.start();
        loop();
    }

    /** Looper.loop()'s body, but an exception out of a message fails the test: Win.apk's main loop
     *  logs it through the host's log, which a plain JVM does not have. */
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
                note("exception: " + t);
                finish();
            }
        }
    }

    static ActivityInfo activity(ApplicationInfo ai, Class<?> c) {
        ActivityInfo a = new ActivityInfo();
        a.name = c.getName();
        a.packageName = ai.packageName;
        a.processName = ai.processName;
        a.applicationInfo = ai;
        a.theme = android.R.style.Theme_DeviceDefault;
        a.exported = true;
        a.enabled = true;
        a.screenOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
        return a;
    }

    static synchronized void finish() {
        int n = Math.max(log.size(), EXPECTED.size());
        for (int i = 0; i < n; i++) {
            String got = i < log.size() ? log.get(i) : "(nothing)";
            String want = i < EXPECTED.size() ? EXPECTED.get(i) : "(nothing)";
            if (!got.equals(want)) {
                System.out.println("FAILED at " + i + ": got \"" + got + "\", Android gives \"" + want + "\"");
                System.exit(1);
            }
        }
        System.out.println("ALL PASSED");
        System.exit(0);
    }
}
