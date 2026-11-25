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
    static final String PING = PKG + ".PING";
    static volatile String heard;
    /** The services' callbacks, in their own order (each service's is Android's; between the two
     *  services and the activities it is the main queue's), checked once at the end. */
    static final List<String> svc = java.util.Collections.synchronizedList(new ArrayList<>());

    /** A started service that stops itself (ActivityThread.handleCreateService, handleServiceArgs,
     *  handleStopService through the in-process activity manager). */
    public static class Started extends android.app.Service {
        @Override public void onCreate() { svc.add("Started.onCreate"); }
        @Override public int onStartCommand(Intent i, int flags, int startId) {
            svc.add("Started.onStartCommand " + startId + " " + i.getStringExtra("s"));
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        @Override public void onDestroy() { svc.add("Started.onDestroy"); }
        @Override public android.os.IBinder onBind(Intent i) { return null; }
    }

    /** A bound service (handleBindService, publishService, the connection, handleUnbindService). */
    public static class Bound extends android.app.Service {
        final android.os.Binder binder = new android.os.Binder();
        @Override public void onCreate() { svc.add("Bound.onCreate"); }
        @Override public android.os.IBinder onBind(Intent i) { svc.add("Bound.onBind"); return binder; }
        @Override public boolean onUnbind(Intent i) { svc.add("Bound.onUnbind"); return false; }
        @Override public void onDestroy() { svc.add("Bound.onDestroy"); }
    }

    /** A started service bound twice in turn: onBind runs once, the second client gets the binder
     *  the first got, and as onUnbind returned false there is no onRebind and no second onUnbind
     *  (ActiveServices' IntentBindRecord keeps the binder while the service lives). */
    public static class Kept extends android.app.Service {
        final android.os.Binder binder = new android.os.Binder();
        @Override public void onCreate() { kept.add("Kept.onCreate"); }
        @Override public int onStartCommand(Intent i, int flags, int startId) {
            kept.add("Kept.onStartCommand");
            return START_NOT_STICKY;
        }
        @Override public android.os.IBinder onBind(Intent i) { kept.add("Kept.onBind"); return binder; }
        @Override public boolean onUnbind(Intent i) { kept.add("Kept.onUnbind"); return false; }
        @Override public void onRebind(Intent i) { kept.add("Kept.onRebind"); }
        @Override public void onDestroy() { kept.add("Kept.onDestroy"); }
    }

    static final List<String> kept = java.util.Collections.synchronizedList(new ArrayList<>());
    static final List<String> KEPT_EXPECTED = List.of("Kept.onCreate", "Kept.onStartCommand", "Kept.onBind",
            "connected 1", "Kept.onUnbind", "connected 2 same binder", "Kept.onDestroy");

    /** A receiver the manifest declares (ActivityThread.handleReceiver through scheduleReceiver), sent
     *  an ordered broadcast whose result it sets for the result receiver. */
    static final String STATIC = PKG + ".STATIC";
    static final List<String> rcv = java.util.Collections.synchronizedList(new ArrayList<>());
    static final String STICKY = PKG + ".STICKY";
    static final List<String> sticky = java.util.Collections.synchronizedList(new ArrayList<>());

    public static class Static extends android.content.BroadcastReceiver {
        @Override public void onReceive(android.content.Context c, Intent i) {
            rcv.add("Static " + i.getStringExtra("v") + " ordered=" + isOrderedBroadcast());
            setResultCode(7);
        }
    }

    /** A provider the manifest declares: created by ActivityThread.installContentProviders before
     *  Application.onCreate, then queried through the app's ContentResolver. */
    static final String AUTHORITY = PKG + ".provider";
    static final List<String> prov = java.util.Collections.synchronizedList(new ArrayList<>());

    public static class Provider extends android.content.ContentProvider {
        @Override public boolean onCreate() { prov.add("Provider.onCreate"); return true; }
        @Override public android.database.Cursor query(android.net.Uri uri, String[] projection, String selection,
                String[] selectionArgs, String sortOrder) {
            android.database.MatrixCursor c = new android.database.MatrixCursor(new String[] {"v"});
            c.addRow(new Object[] {"hello " + uri.getLastPathSegment()});
            return c;
        }
        @Override public String getType(android.net.Uri uri) { return null; }
        @Override public android.net.Uri insert(android.net.Uri uri, android.content.ContentValues v) { return null; }
        @Override public int delete(android.net.Uri uri, String s, String[] a) { return 0; }
        @Override public int update(android.net.Uri uri, android.content.ContentValues v, String s, String[] a) { return 0; }
    }

    static final List<String> SVC_EXPECTED = List.of(
            "Started.onCreate", "Started.onStartCommand 1 go", "Started.onDestroy",
            "Bound.onCreate", "Bound.onBind", "connected to Bound", "Bound.onUnbind", "Bound.onDestroy");

    static synchronized void note(String s) {
        log.add(s);
        System.out.println("  " + s);
    }

    public static class App extends Application {
        @Override public void onCreate() {
            super.onCreate();
            note("App.onCreate");
            prov.add("App.onCreate");
            // A receiver registered on the application's context hears a broadcast an activity sends
            // (step A3: ContextImpl's receivers and broadcasts). Not in the callback order: when it
            // is heard is the broadcast queue's, checked once at the end.
            registerReceiver(new android.content.BroadcastReceiver() {
                @Override public void onReceive(android.content.Context c, Intent i) {
                    heard = i.getStringExtra("v");
                }
            }, new android.content.IntentFilter(PING), RECEIVER_NOT_EXPORTED);
            // A registered receiver of the ordered broadcast below the manifest's in priority: it
            // hears it after the manifest receiver, with that one's result, and sets its own
            // (broadcastIntentLocked merges the two lists by priority).
            android.content.IntentFilter low = new android.content.IntentFilter(STATIC);
            low.setPriority(-10);
            registerReceiver(new android.content.BroadcastReceiver() {
                @Override public void onReceive(android.content.Context c, Intent i) {
                    rcv.add("Dyn " + getResultCode());
                    setResultCode(8);
                }
            }, low, RECEIVER_NOT_EXPORTED);
            // A sticky broadcast sent while registered is not an initial sticky one.
            registerReceiver(new android.content.BroadcastReceiver() {
                @Override public void onReceive(android.content.Context c, Intent i) {
                    sticky.add("live initial=" + isInitialStickyBroadcast());
                }
            }, new android.content.IntentFilter(STICKY), RECEIVER_NOT_EXPORTED);
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
        static boolean started;   // the process's first resume only (First is relaunched below)
        static boolean resized;

        @Override protected void onResume() {
            super.onResume();
            first = this;
            if (!started) {
                started = true;
                note("First: getApplication() is App: " + (getApplication() instanceof App));
                sendBroadcast(new Intent(PING).setPackage(PKG).putExtra("v", "pong"));
                sendStickyBroadcast(new Intent(STICKY).setPackage(PKG));
                // Registered after it was sent: the sticky is replayed, as an initial one.
                registerReceiver(new android.content.BroadcastReceiver() {
                    @Override public void onReceive(android.content.Context c, Intent i) {
                        sticky.add("replay initial=" + isInitialStickyBroadcast());
                    }
                }, new android.content.IntentFilter(STICKY), RECEIVER_NOT_EXPORTED);
                try (android.database.Cursor c = getContentResolver().query(
                        android.net.Uri.parse("content://" + AUTHORITY + "/x"), null, null, null, null)) {
                    prov.add(c != null && c.moveToFirst() ? "query " + c.getString(0) : "query nothing");
                }
                startService(new Intent(this, Started.class).putExtra("s", "go"));
                sendOrderedBroadcast(new Intent(STATIC).setPackage(PKG).putExtra("v", "m"), null,
                        new android.content.BroadcastReceiver() {
                            @Override public void onReceive(android.content.Context c, Intent i) {
                                rcv.add("result " + getResultCode());
                            }
                        }, null, 0, null, null);
                bindService(new Intent(this, Bound.class), new android.content.ServiceConnection() {
                    @Override public void onServiceConnected(android.content.ComponentName n, android.os.IBinder b) {
                        svc.add("connected to " + n.getClassName().substring(n.getClassName().lastIndexOf('$') + 1));
                        unbindService(this);
                    }
                    @Override public void onServiceDisconnected(android.content.ComponentName n) {}
                }, BIND_AUTO_CREATE);
                startService(new Intent(this, Kept.class));
                bindService(new Intent(this, Kept.class), new android.content.ServiceConnection() {
                    @Override public void onServiceConnected(android.content.ComponentName n, android.os.IBinder b) {
                        kept.add("connected 1");
                        android.os.IBinder firstBinder = b;
                        unbindService(this);
                        bindService(new Intent(First.this, Kept.class), new android.content.ServiceConnection() {
                            @Override public void onServiceConnected(android.content.ComponentName n2, android.os.IBinder b2) {
                                kept.add("connected 2" + (b2 == firstBinder ? " same binder" : " another binder"));
                                unbindService(this);
                                stopService(new Intent(First.this, Kept.class));
                            }
                            @Override public void onServiceDisconnected(android.content.ComponentName n2) {}
                        }, BIND_AUTO_CREATE);
                    }
                    @Override public void onServiceDisconnected(android.content.ComponentName n) {}
                }, BIND_AUTO_CREATE);
                // Through a trampoline that forwards First's request to Second
                // (FLAG_ACTIVITY_FORWARD_RESULT) and finishes in its onCreate.
                startActivityForResult(new Intent(this, Trampoline.class), 42);
            }
        }

        @Override protected void onSaveInstanceState(Bundle out) {
            super.onSaveInstanceState(out);
            // Once First is stopped, the display changes size (a configuration change neither
            // activity declares): Second, resumed, is relaunched now; First, stopped, only when it
            // comes back (ensureActivityConfiguration skips an invisible activity). The relaunched
            // Second finishes.
            if (!resized) {
                resized = true;
                Configuration c = new Configuration(launchConfig);
                c.screenWidthDp = 400;
                com.winapk.aospapp.AospAppHost.INSTANCE.configurationChanged(c);
            }
        }

        @Override protected void onActivityResult(int request, int result, Intent data) {
            super.onActivityResult(request, result, data);
            note("First.onActivityResult " + request + " " + result + " " + (data != null ? data.getStringExtra("r") : null));
        }
    }

    /** Started for a result, it starts Second with FLAG_ACTIVITY_FORWARD_RESULT and finishes: Second
     *  gets First as its caller and its result goes to First; the trampoline itself no longer has a
     *  caller. Not in the callback order (a finish in onCreate skips its onStart and onResume). */
    static final List<String> tramp = java.util.Collections.synchronizedList(new ArrayList<>());

    public static class Trampoline extends Activity {
        @Override protected void onCreate(Bundle b) {
            super.onCreate(b);
            tramp.add("calling " + (getCallingActivity() != null ? getCallingActivity().getShortClassName() : null));
            startActivity(new Intent(this, Second.class).putExtra("x", 7).addFlags(Intent.FLAG_ACTIVITY_FORWARD_RESULT));
            tramp.add("after forwarding, calling " + getCallingActivity());
            finish();
        }
    }

    /** Second finishes once First is stopped (First's onSaveInstanceState, after its onStop), so the
     *  way back passes through onRestart; First finishes once Second is destroyed. Each is posted,
     *  as an app's click would be. */
    static Second second;
    static First first;

    public static class Second extends Base {
        static int resumes;

        @Override protected void onResume() {
            super.onResume();
            second = this;
            note("Second: extra x=" + getIntent().getIntExtra("x", -1) + ", calling " + getCallingActivity().getShortClassName());
            setResult(RESULT_OK, new Intent().putExtra("r", "done"));
            if (++resumes == 2) new android.os.Handler(Looper.getMainLooper()).post(() -> second.finish());
        }

        @Override protected void onDestroy() {
            super.onDestroy();
            if (isFinishing()) new android.os.Handler(Looper.getMainLooper()).post(() -> first.finish());
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
            // The size change: Second relaunched, First (stopped) left alone.
            "Second.onPause", "Second.onStop", "Second.onSaveInstanceState", "Second.onDestroy",
            "Second.onCreate", "Second.onStart", "Second.onResume",
            "Second: extra x=7, calling AospAppLifecycleTest$First",
            "Second.onPause",
            // First comes back relaunched at the new size, its result delivered before onResume.
            "First.onDestroy", "First.onCreate", "First.onStart",
            "First.onActivityResult 42 -1 done",
            "First.onResume",
            "Second.onStop", "Second.onDestroy",
            "First.onPause", "First.onStop", "First.onDestroy",
            "task empty");

    static final Configuration launchConfig = new Configuration();

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
        apk = withoutManifest(apk, new File(data, "app.apk"));
        ai.sourceDir = ai.publicSourceDir = apk;
        // As the package manager fills them: /data/user/0/<pkg> (credential protected, the default
        // storage) and /data/user_de/0/<pkg>.
        ai.dataDir = ai.credentialProtectedDataDir = new File(data, "user").getPath();
        ai.deviceProtectedDataDir = new File(data, "user_de").getPath();
        new File(ai.dataDir).mkdirs();
        new File(ai.deviceProtectedDataDir).mkdirs();
        java.nio.file.Files.writeString(new File(ai.dataDir, "AndroidManifest.xml").toPath(), MANIFEST);
        ai.nativeLibraryDir = new File(data, "lib").getPath();
        ai.targetSdkVersion = 34;
        ai.uid = android.os.Process.myUid();
        ai.enabled = true;
        ai.flags = ApplicationInfo.FLAG_HAS_CODE;

        Map<String, ActivityInfo> activities = Map.of(
                First.class.getName(), activity(ai, First.class),
                Second.class.getName(), activity(ai, Second.class),
                Trampoline.class.getName(), activity(ai, Trampoline.class));
        Configuration config = launchConfig;
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
                packageManager(apk, ai), launch,
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

    static final String MANIFEST = "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\""
            + " package=\"" + PKG + "\">\n"
            + "  <application android:name=\"AospAppLifecycleTest$App\">\n"
            + "    <receiver android:name=\"AospAppLifecycleTest$Static\" android:exported=\"false\">\n"
            + "      <intent-filter><action android:name=\"" + STATIC + "\"/></intent-filter>\n"
            + "    </receiver>\n"
            + "    <provider android:name=\"AospAppLifecycleTest$Provider\" android:authorities=\"" + AUTHORITY
            + "\" android:exported=\"false\"/>\n"
            + "  </application>\n"
            + "</manifest>\n";

    /** A copy of `apk` with no AndroidManifest.xml: its resources, and the app's manifest from text. */
    static String withoutManifest(String apk, File out) throws java.io.IOException {
        try (java.util.zip.ZipFile in = new java.util.zip.ZipFile(apk);
             java.util.zip.ZipOutputStream zo = new java.util.zip.ZipOutputStream(new java.io.FileOutputStream(out))) {
            for (java.util.Enumeration<? extends java.util.zip.ZipEntry> en = in.entries(); en.hasMoreElements();) {
                java.util.zip.ZipEntry e = en.nextElement();
                if (e.getName().equals("AndroidManifest.xml")) continue;
                java.util.zip.ZipEntry c = new java.util.zip.ZipEntry(e.getName());
                c.setMethod(e.getMethod());
                if (e.getMethod() == java.util.zip.ZipEntry.STORED) {
                    c.setSize(e.getSize());
                    c.setCompressedSize(e.getCompressedSize());
                    c.setCrc(e.getCrc());
                }
                zo.putNextEntry(c);
                try (java.io.InputStream is = in.getInputStream(e)) { is.transferTo(zo); }
                zo.closeEntry();
            }
        }
        return out.getPath();
    }

    /** The package manager the runtime builds (Boot): the manifest's providers given to it. */
    static com.macapk.loader.MacApkPackageManager packageManager(String apk, ApplicationInfo ai) throws Exception {
        com.macapk.loader.Manifest m = com.macapk.loader.Manifest.parse(new File(ai.dataDir, "AndroidManifest.xml"));
        Map<String, com.macapk.loader.Manifest.ProviderEntry> providers = new java.util.HashMap<>();
        for (com.macapk.loader.Manifest.ProviderEntry p : m.providers) providers.put(p.name, p);
        return new com.macapk.loader.MacApkPackageManager(PKG, apk, ai.dataDir, ai.nativeLibraryDir, null, null,
                providers);
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
        if (!"pong".equals(heard)) {
            System.out.println("FAILED: the application's receiver heard " + heard + ", not the activity's broadcast");
            System.exit(1);
        }
        // Each service's own order: Started's callbacks and Bound's, interleaved as the queue has them.
        List<String> started = new ArrayList<>(), bound = new ArrayList<>();
        synchronized (svc) {
            for (String e : svc) (e.startsWith("Started") ? started : bound).add(e);
        }
        List<String> want = SVC_EXPECTED;
        if (!started.equals(want.subList(0, 3)) || !bound.equals(want.subList(3, want.size()))) {
            System.out.println("FAILED: the services gave " + svc + ", Android gives " + want);
            System.exit(1);
        }
        if (!tramp.equals(List.of("calling AospAppLifecycleTest$First", "after forwarding, calling null"))) {
            System.out.println("FAILED: the trampoline gave " + tramp + ", Android gives [calling AospAppLifecycleTest$First,"
                    + " after forwarding, calling null]");
            System.exit(1);
        }
        if (!kept.equals(KEPT_EXPECTED)) {
            System.out.println("FAILED: the service bound twice gave " + kept + ", Android gives " + KEPT_EXPECTED);
            System.exit(1);
        }
        if (!prov.equals(List.of("Provider.onCreate", "App.onCreate", "query hello x"))) {
            System.out.println("FAILED: the provider gave " + prov + ", Android gives [Provider.onCreate, App.onCreate,"
                    + " query hello x]");
            System.exit(1);
        }
        if (!rcv.equals(List.of("Static m ordered=true", "Dyn 7", "result 8"))) {
            System.out.println("FAILED: the ordered broadcast's receivers and result gave " + rcv
                    + ", Android gives [Static m ordered=true, Dyn 7, result 8]");
            System.exit(1);
        }
        if (!new java.util.HashSet<>(sticky).equals(java.util.Set.of("live initial=false", "replay initial=true"))) {
            System.out.println("FAILED: the sticky broadcast's receivers gave " + sticky
                    + ", Android gives [live initial=false, replay initial=true]");
            System.exit(1);
        }
        System.out.println("ALL PASSED");
        System.exit(0);
    }
}
