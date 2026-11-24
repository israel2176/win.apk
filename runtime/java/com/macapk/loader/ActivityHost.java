// The host's side of an app's starts and its window, on AOSP's app layer (com.macapk.loader.app.AppHost:
// the task, the activities and their lifecycle are AOSP's ActivityThread over the in-process task
// manager, lib/aosp-app.jar): the hand-offs of an intent no activity of the app answers to Windows (a
// URL, mail, a share, an install, a settings page, a picker), the cover a host window puts over the
// task, the window's close, minimize and restore, and starts from outside (another app's, a URL).
package com.macapk.loader;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;

import java.lang.reflect.Method;

public final class ActivityHost {
    private static final String TAG = "Mac.apk.ActivityHost";


    // ---- START RESOLUTION (v1.0.1652; docs/cross-app-launch-and-implicit-
    // start-design_2026-09-26.md stages 2a/2b/2d/2e). Each is read ONCE (the JDK
    // caches the environment at startup, so this is the value a per-call read
    // returns) and "0" restores the exact pre-change start path.
    /** Stage 2a: an IMPLICIT start resolves through the ONE resolver
     *  (MacApkPackageManager.queryIntentActivities, MATCH_DEFAULT_ONLY) instead
     *  of going straight to the host handoff, so an app's OWN implicit targets
     *  launch (they were silently dropped) and setPackage / the selector are
     *  honoured because the resolver already honours them. */
    static final boolean START_RESOLVE =
        !"0".equals(System.getenv("MACAPK_START_RESOLVE"));
    /** Stage 2b: a start nothing resolves throws ActivityNotFoundException with
     *  Instrumentation.checkStartActivityResult's two message shapes (android14
     *  :2232-2240), which is what every app's fallback path is written against
     *  (ACRA's email report, "no browser installed" toasts). Public: Activity
     *  reads it to drop, not settle, a startActivityForResult pairing. */
    public static final boolean START_ANF =
        !"0".equals(System.getenv("MACAPK_START_ANF"));
    /** An activity that never installs a content view gets its window's OWN
     *  decor, empty, at the end of its resume, as AOSP
     *  ActivityThread.handleResumeActivity adds `r.window.getDecorView()`
     *  (android14-release :5019-5041). It replaces the framework-built empty
     *  GLSurfaceView that ensureFallbackContentView installed after onCreate:
     *  no device ever shows one, it painted black over the starting window's
     *  windowBackground and hid any views the app had added to
     *  android.R.id.content. See addEmptyWindowIfNeeded and
     *  docs/fallback-glsurfaceview-content-retired_2026-09-27.md.
     *  MACAPK_EMPTY_WINDOW_DECOR=0 restores the fallback GLSurfaceView (and its
     *  renderer-bearing guard in GLSurfaceView) byte for byte. Public: Activity
     *  and GLSurfaceView read it. */
    public static final boolean EMPTY_WINDOW_DECOR =
        !"0".equals(System.getenv("MACAPK_EMPTY_WINDOW_DECOR"));
    /** Stage 2d: an explicit start of an <activity-alias> instantiates its
     *  targetActivity; getIntent().getComponent() stays the alias
     *  (ActivityThread.performLaunchActivity). Class.forName(alias) used to miss
     *  and the start was dropped. */
    static final boolean START_ALIAS =
        !"0".equals(System.getenv("MACAPK_START_ALIAS"));
    /** Stage 2e: a caller whose targetSdk is KNOWN and >= 34, starting an intent
     *  with no package and no component, matches only EXPORTED components, its
     *  own included (ResolveIntentHelper.java:114-148, android14). */
    static final boolean START_EXPORTED_ONLY =
        !"0".equals(System.getenv("MACAPK_START_EXPORTED_ONLY"));

    private static android.app.Application app;
    private static ClassLoader appCl;

    /** The Application and the app's class loader, right before Application.onCreate. */
    public static void init(android.app.Application application, ClassLoader cl) {
        app = application;
        appCl = cl;
    }

    /** v1.0.572: the APK's class loader, for the shim's JNI FindClass
     *  fallback. On Android an app process's "system class loader" IS the
     *  app's PathClassLoader, so native FindClass from ANY thread (even an
     *  attached game thread with no Java frames) resolves APK classes; on
     *  HotSpot the system loader only sees the framework jars, so the shim
     *  retries failed lookups through this loader to mirror Android. */
    public static ClassLoader macapkAppClassLoader() { return appCl; }

    private static com.macapk.loader.app.AppHost appHost() {
        com.macapk.loader.app.AppHost h = com.macapk.loader.app.AppHost.get();
        return h != null && h.started() ? h : null;
    }

    /** The task's top activity (AOSP's instance in ActivityThread), or null. */
    public static Activity peek() {
        com.macapk.loader.app.AppHost h = appHost();
        return h != null ? h.topActivity() : null;
    }

    /** An activity's view root: its window's decor (null before the window has one), the view
     *  ViewRootImpl dispatches the window's input to. */
    public static android.view.View contentRoot(Activity a) {
        android.view.Window w = a != null ? a.getWindow() : null;
        return w != null ? w.peekDecorView() : null;
    }

    /** The same: where MacApkProcessExit and the inject driver look. */
    public static Activity macapkTopActivity() { return peek(); }

    /** The window was closed (or the app's process leaves): the task is removed by the task
     *  manager, every activity destroyed from the top down; true when that finished in time. */
    public static boolean macapkTerminate(long timeoutMs) {
        return macapkTerminate(timeoutMs, true);
    }

    /** @param processExits true when the caller leaves the process as soon as
     *         this returns (macapk_terminate_now: window close, an app's
     *         System.exit). Only then may the wait take the windows off screen
     *         (QUIT_HIDE_AFTER_PAUSE) or arm the backstop (QUIT_BACKSTOP_GRACE_MS).
     *         android.os.Process.killProcess(myPid()) passes false: the process
     *         lives on after it, so its windows must stay where they are. */
    public static boolean macapkTerminate(long timeoutMs, boolean processExits) {
        // The input recorder's `inputrec-end`: a quit leaves through _exit, which runs no Java
        // shutdown hook, so this is the one place a normal quit can close the recording.
        if (processExits && InputRecorder.ON) InputRecorder.close();
        com.macapk.loader.app.AppHost h = appHost();
        if (h == null) return true;
        sTerminateTopPaused = false;
        java.util.concurrent.CountDownLatch done = h.terminate(() -> sTerminateTopPaused = true);
        try {
            if (QUIT_PUMPS_RENDER) return awaitTeardownPumping(done, timeoutMs, processExits);
            return done.await(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
    }

    // ---- The quit wait keeps the render thread running (2026-09-30) ----------
    //
    // AOSP android14-release opengl/java/android/opengl/GLSurfaceView.java: the
    // renderer runs on `static class GLThread extends Thread` (:1253), whose
    // guardedRun() loop (:1302) takes queued events and calls onDrawFrame on its
    // OWN thread for as long as `readyToDraw()` (:1628) holds, i.e. until the
    // pause is delivered; GLThread.onPause() (:1721-1740) is the UI thread
    // blocking in `while ((! mExited) && (! mPaused)) sGLThreadManager.wait()`
    // for THAT thread. So a UI-thread onPause may wait on the render thread, and
    // apps do: libGDX's AndroidApplication.onPause -> AndroidGraphics.pause()
    // sets `pause` and `synch.wait(4000)`s until the next onDrawFrame clears it
    // (else "waiting for pause synchronization took too long; assuming deadlock
    // and killing"), and when finishing, AndroidGraphics.destroy() `synch.wait()`s
    // UNBOUNDED for one more onDrawFrame (read from Klooni's own converted
    // classes.jar, 2026-09-30).
    //
    // Ours has no GLThread: the render thread is the host AppKit main thread
    // (drawInMTKView -> GLSurfaceView.macapkDrawFrame). And the close / Cmd-Q /
    // Dock Quit paths call macapkTerminate ON that thread and block it until
    // the teardown returns. Measured (debug/closelat.sh + jstack, Klooni): the
    // Android main thread sat in AndroidGraphics.pause -> Object.wait while
    // AppKit main sat in this await, each waiting on the other, until the 2 s cap
    // -- every close of every libGDX app cost 2.0 s, and its pause/dispose never
    // ran. So while it waits, the host render thread keeps doing the one job
    // AOSP's GLThread would still be doing: a render tick
    // (NativeCocoa.hostRenderTick -> -[MTKView draw] -> drawInMTKView), which
    // runs queued GL events and, until GLSurfaceView.onPause/onDetachedFromWindow
    // withholds it, onDrawFrame. This is not a new concurrency: while an activity
    // pauses for any OTHER reason the display link ticks exactly like this.
    // Nothing is ticked if the teardown finishes within the first slice (the
    // Java-only apps finish in 3-8 ms).
    //
    // MACAPK_QUIT_PUMPS_RENDER=0 restores the plain await (the old path, byte
    // for byte). Default ON.
    static final boolean QUIT_PUMPS_RENDER =
        !"0".equals(System.getenv("MACAPK_QUIT_PUMPS_RENDER"));
    /** One render-tick interval, well under a 60 Hz frame; a tick is itself
     *  paced by the swap it does. */
    private static final long QUIT_SLICE_MS = 8L;
    /** The render tick awaitTeardownPumping runs between slices. A field only so
     *  debug/quitpump_gate.sh can drive the real wait on a bare JVM, where
     *  NativeCocoa's natives do not exist; nothing in the runtime assigns it. */
    static java.util.function.BooleanSupplier sHostRenderTick = NativeCocoa::hostRenderTick;

    // ---- ...and takes the window off screen once the task has paused ----------
    //
    // lanes19 kept the window up until the WHOLE teardown returned, because
    // taking it off screen BEFORE the pause finished cut Unity's pause off (its
    // render thread cannot get a drawable once the layer is off screen; the
    // MACAPK_ASYNC_CLOSE arm, refuted and deleted). AOSP hides a finishing
    // RESUMED activity even earlier, when the finish is requested:
    // ActivityRecord.finishIfPossible (android14 :3572-3573) calls
    // `setVisibility(false)` -- "Tell window manager to prepare for this one to
    // be removed" -- and stop/destroy only follow the pause
    // (TaskFragment.completePause :1744-1751 -> completeFinishing). So on a
    // device an app whose onStop / onDestroy is slow is never on screen for it.
    // We cannot hide as early as AOSP (Unity, above), so we hide at the earliest
    // point that does not cut a pause off: when onPause has returned. MEASURED victim: OpenTTD's own MainActivity.onDestroy is
    // `mGLView.exitApp(); super.onDestroy(); Thread.sleep(2000L);
    // System.exit(0)` (its converted classes.jar), so its window stayed frozen
    // for the full 2 s cap. Once the top activity's onPause has returned, the
    // wait orders the app's windows out and stops ticking (the surface is gone:
    // AOSP's GLThread draws nothing without one -- though it would still run
    // queued GL events, guardedRun :1330; see docs/gl-close-latency_2026-09-30.md
    // section 5), then keeps waiting for onStop/onDestroy under the same cap.
    // Only when the caller exits right after (processExits): killProcess(myPid())
    // keeps the process, and an ordered-out window would never come back for the
    // activity a restart idiom starts next.
    //
    // MACAPK_QUIT_HIDE_AFTER_PAUSE=0: the window stays until the teardown
    // returns (the old path, byte for byte). Default ON.
    static final boolean QUIT_HIDE_AFTER_PAUSE =
        !"0".equals(System.getenv("MACAPK_QUIT_HIDE_AFTER_PAUSE"));
    /** Set on the Android main thread once the visible activity of a task being
     *  removed (sTerminating) has returned from onPause. */
    static volatile boolean sTerminateTopPaused;
    /** Takes the app's windows off screen (first call) and flushes the
     *  transaction that carries it (every call); a field for the same reason as
     *  sHostRenderTick. */
    static java.util.function.BooleanSupplier sHostHideWindows = NativeCocoa::hostHideWindowsForQuit;

    // ---- ...and a tick can no longer hold the quit past the cap -------------
    //
    // The plain await above was bounded by construction: whatever the guest did,
    // macapk_terminate_now _exit()ed 2 s later ("a guest whose main thread is
    // already wedged must not be able to hang the quit path"). A render tick
    // breaks that: it runs the guest's renderer SYNCHRONOUSLY on the waiting
    // thread, and no timeout can preempt a call in progress -- a renderer that
    // blocks for good (on the Android main thread it shares the teardown with,
    // say) would leave the app frozen on screen until Force Quit. AOSP never
    // gets there: the task's process is killed whatever its GLThread is doing.
    // So once the wait has pumped a tick, a daemon thread waits out the cap plus
    // this grace (one tick may overrun the cap by up to a frame, or by
    // CAMetalLayer.nextDrawable's 1 s timeout) and, if the waiting thread has
    // still not come back, leaves the process the way macapk_terminate_now
    // would have (NativeCocoa.quitBackstopExit: process-group kill, _exit with
    // the same code). Only for a caller that exits anyway (processExits); never
    // armed when no tick ran, so the Java-only closes start no thread.
    static final long QUIT_BACKSTOP_GRACE_MS = 2000L;
    /** What the backstop runs; a field for the same reason as sHostRenderTick
     *  (null = no backstop, which debug/quitpump_gate.sh's control uses to show
     *  the cap alone does not bound a wedged tick). */
    static Runnable sQuitBackstop = NativeCocoa::quitBackstopExit;

    private static Thread armQuitBackstop(final long deadline,
            final java.util.concurrent.atomic.AtomicBoolean returned) {
        final Runnable backstop = sQuitBackstop;
        if (backstop == null) return null;
        Thread t = new Thread(new Runnable() {
            @Override public void run() {
                final long end = deadline + QUIT_BACKSTOP_GRACE_MS * 1_000_000L;
                while (true) {
                    long leftMs = (end - System.nanoTime()) / 1_000_000L;
                    if (leftMs <= 0) break;
                    try { Thread.sleep(leftMs); } catch (InterruptedException ie) { return; }
                }
                if (returned.get()) return;
                NativeLog.w(TAG, "macapkTerminate: the host thread has not come back from a"
                    + " render tick " + QUIT_BACKSTOP_GRACE_MS + " ms past the teardown cap;"
                    + " leaving the process without it");
                backstop.run();
            }
        }, "macapk-quit-backstop");
        t.setDaemon(true);
        t.start();
        return t;
    }

    private static boolean awaitTeardownPumping(
            java.util.concurrent.CountDownLatch done, long timeoutMs, boolean processExits)
            throws InterruptedException {
        final long deadline = System.nanoTime() + timeoutMs * 1_000_000L;
        final java.util.concurrent.atomic.AtomicBoolean returned =
            new java.util.concurrent.atomic.AtomicBoolean();
        Thread backstop = null;
        int ticks = 0;
        boolean hidden = false;
        try {
            while (true) {
                long leftMs = (deadline - System.nanoTime()) / 1_000_000L;
                if (leftMs <= 0) {
                    // A tick that overran the cap may have let the teardown finish.
                    if (done.getCount() == 0) return true;
                    break;
                }
                if (hidden) {
                    // Keep flushing: the ordering reaches the window server only
                    // through a transaction commit this blocked thread must make.
                    sHostHideWindows.getAsBoolean();
                } else if (QUIT_HIDE_AFTER_PAUSE && processExits && sTerminateTopPaused
                        && sHostHideWindows.getAsBoolean()) {
                    hidden = true;
                    NativeLog.i(TAG, "macapkTerminate: top activity paused; window off screen"
                        + " (onStop/onDestroy continue, " + ticks + " render tick(s) so far)");
                }
                if (done.await(Math.min(leftMs, QUIT_SLICE_MS),
                        java.util.concurrent.TimeUnit.MILLISECONDS)) {
                    if (ticks > 0) {
                        NativeLog.i(TAG, "macapkTerminate: teardown needed " + ticks
                            + " render tick(s) on the host render thread");
                    }
                    return true;
                }
                // false off the host main thread, inside a tick already (a renderer
                // that quits from onDrawFrame), or with no window: then this is just
                // the await, in slices.
                if (!hidden) {
                    if (processExits && backstop == null)
                        backstop = armQuitBackstop(deadline, returned);
                    if (sHostRenderTick.getAsBoolean()) ticks++;
                }
            }
        } finally {
            returned.set(true);
            if (backstop != null) backstop.interrupt();
        }
        // Say WHAT the teardown is stuck on: the next wall of this shape should
        // not need a jstack to find.
        Thread owner = null;
        try {
            android.os.Looper m = android.os.Looper.getMainLooper();
            if (m != null) owner = m.getThread();
        } catch (Throwable ignored) {}
        StringBuilder sb = new StringBuilder();
        if (owner != null) {
            StackTraceElement[] st = owner.getStackTrace();
            for (int i = 0; i < st.length && i < 12; i++) sb.append("\n    at ").append(st[i]);
        }
        NativeLog.w(TAG, "macapkTerminate: teardown did not finish in " + timeoutMs
            + " ms (" + ticks + " render tick(s) pumped); Android main thread "
            + (owner != null ? owner.getName() : "?") + " is at:" + sb);
        return false;
    }

    // ---- ONE Android main thread (2026-09-25) --------------------------------
    //
    // AOSP has exactly one main thread per process: ActivityThread.main() calls
    // Looper.prepareMainLooper() once, and a second call throws "The main Looper
    // has already been prepared." (android14-release Looper.java:127-131).
    // Context.startActivity from any other thread is a binder call into
    // ActivityTaskManager; it prepares no Looper on the caller, and the launch
    // arrives on the main thread as a transaction (ActivityThread.H).
    //
    // start()/startAll() used to call prepareMainLooper() on WHATEVER thread
    // called startActivity (v1.0.99, from before Boot pinned the main thread and
    // before the M3 off-thread post existed), and our prepareMainLooper() only
    // ADDS the caller to Looper's set of "main" threads. So every worker that
    // ever started an activity became a main thread for the rest of the process:
    // myLooper() answered MAIN on it (AOSP: null, it never called prepare()),
    // and every set-membership main-thread test passed off the main thread.
    // MEASURED victim, OpenTTD (tablet and phone A/B, 2026-09-24/25, two runs
    // each): RestartMainActivity$1's worker calls startActivity(MainActivity)
    // and then System.exit. The exit read the worker as "the main thread", so
    // MacApkProcessExit skipped its v1.0.1645 calling-thread attribution and
    // quit the whole app (rule c); macapkTerminate then ran MainActivity's
    // teardown INLINE on that worker, whose onDestroy re-entered System.exit and
    // printed a "that process ends" line that made the v1.0.1645 fix look live.
    // docs/tablet-ab-triage_2026-09-25.md section 1.
    //
    // MACAPK_ONE_MAIN_THREAD=0 restores the old behaviour for bisecting: start()
    // registers its caller again, and the decisions below go back to set
    // membership. Default ON.
    static final boolean ONE_MAIN_THREAD =
        !"0".equals(System.getenv("MACAPK_ONE_MAIN_THREAD"));

    /** Is the calling thread THE Android main thread? With the real looper and a
     *  registered main thread that is the one Boot designated, by identity
     *  (Looper.macapkIsActivityThread): other members of Looper's main-thread set
     *  (the frame-callback thread ViewCanvasDriver marks for RecyclerView, a
     *  worker registered before this fix) can pass androidx's assertions but are
     *  not the thread that owns the activity stack, so "run it inline or post it
     *  to the main looper?" must not treat them as the owner. Under
     *  MACAPK_REAL_LOOPER=0, or before any main thread exists, the historical
     *  set-membership answer stands (the legacy model drains the main queue on
     *  more than one thread). So does a main looper that has QUIT (the v1.0.833
     *  pathology): start() then launches inline on its caller and registers it,
     *  exactly as the legacy model does, and nothing posted to the dead queue
     *  would ever run -- the decision here has to agree with start()'s, or
     *  macapkTerminate from that thread waits out its timeout for nothing.
     *  Package-private: MacApkProcessExit asks the same question. */
    static boolean macapkOnMainThread() {
        if (ONE_MAIN_THREAD && android.os.Looper.MACAPK_REAL_LOOPER
                && android.os.Looper.macapkHasMainThread()
                && !android.os.Looper.macapkMainLooperQuit()) {
            return android.os.Looper.macapkIsActivityThread();
        }
        return android.os.Looper.getMainLooper().isCurrentThread();
    }

    private static android.os.Handler ownerHandler() {
        android.os.Looper main = android.os.Looper.getMainLooper();
        return android.os.Looper.MACAPK_REAL_LOOPER
            ? android.os.Handler.macapkCreateOwnerOnly(main)
            : new android.os.Handler(main);
    }

    /** Win.apk: the host's window was minimized by the user (the title bar's button, the taskbar,
     *  Win+D), from the window's thread: the task goes behind, its resumed activity paused and
     *  stopped, as a device does for a task moved to the back. */
    public static void macapkHostMinimized() {
        com.macapk.loader.app.AppHost h = appHost();
        if (h != null) h.hostMinimized();
    }

    /** The window came back: the task comes to the front (restarted, started, resumed), or the
     *  launcher activity starts again on an empty one. */
    public static void macapkTaskToFront() {
        com.macapk.loader.app.AppHost h = appHost();
        if (h != null) h.taskToFront();
    }

    static void macapkPostOnOwner(Runnable r) {
        ownerHandler().post(r);
    }

    /** MacApkProcessExit: a secondary process's activities finish (AppHost.finishActivities). */
    static void macapkFinishActivities(java.util.Set<String> classNames) {
        com.macapk.loader.app.AppHost h = appHost();
        if (h != null) h.finishActivities(classNames);
    }

    /** MacApkProcessExit: the starts a leaving process still owes, for the next process. The
     *  package is the app's own (one package per process). */
    static java.util.List<Intent> macapkPendingSelfStarts(String pkg) {
        com.macapk.loader.app.AppHost h = appHost();
        return h != null ? h.pendingStarts() : java.util.List.of();
    }

    /** Another app's start of one of this app's activities, or a URL the host hands over (IntentInbox,
     *  HostUrlIntake): the task manager starts it (ActivityStarter's rules), and a start from outside
     *  brings the task, the host's window, to the front. */
    static void macapkDeliverCrossApp(Intent intent, String sender) {
        com.macapk.loader.app.AppHost h = appHost();
        if (h == null) return;
        try { NativeCocoa.moveTaskToFront(); } catch (Throwable ignored) {}
        h.deliver(intent, sender);
    }

    /** The host-launch POLICY shared by host URL opens and cross-app starts: the
     *  harness kill switch, and "a human just did something" (the v1.0.822 /
     *  v1.0.849 recency window). A refusal is the background-activity-launch
     *  START_ABORTED answer: silent to the caller, logged here. */
    static boolean macapkLaunchAllowed(String what) {
        if (externalUrlsOff()) {
            NativeLog.w(TAG, "REFUSED " + what + ": MACAPK_EXTERNAL_URLS=0 (harness run)");
            return false;
        }
        if (!allowHostUrlAtLaunch()) {
            long last = TouchDispatcher.lastUserInputMillis();
            long age = (last == 0L) ? Long.MAX_VALUE : (System.currentTimeMillis() - last);
            if (age > hostUrlGraceMs()) {
                NativeLog.w(TAG, "REFUSED " + what + ": last real input "
                        + (last == 0L ? "NEVER" : (age + "ms ago"))
                        + " (> " + hostUrlGraceMs() + "ms grace) -- a programmatic"
                        + " launch, not a user click. MACAPK_ALLOW_HOST_URL_AT_LAUNCH=1 to allow.");
                return false;
            }
        }
        return true;
    }

    /** The cover a host window (a prompt, a picker, a settings page standing in for an activity of
     *  the system's) puts over the task: the task manager pauses the top activity, and stops it too
     *  when the window hides it (opaque), and brings it back with the window's result. Without the
     *  app layer started (a JVM with no app), nothing to cover. */
    static SpecialAccessHandler.Cover hostCover(final boolean opaque) {
        com.macapk.loader.app.AppHost h = appHost();
        if (h != null) return h.hostCover(opaque);
        return new SpecialAccessHandler.Cover() {
            @Override public void cover() {}
            @Override public void uncover() {}
            @Override public void uncoverWithResult(Runnable result) {
                if (result != null) result.run();
            }
        };
    }

    /**
     * v1.0.690: resolve an implicit intent to a macOS-host action where one
     * exists. Returns true if handled. Covers the handoffs an APK expects the
     * system to route: ACTION_VIEW of a web/geo URL, ACTION_SENDTO/mailto,
     * ACTION_DIAL/tel, and (v1.0.940) ACTION_SEND of text -- through a chooser
     * wrapper or not. Anything else returns false (unhandled), which is the
     * same observable result as a device with no app registered for it.
     */
    /** A start no activity of the app answers, offered to the host (AppHost.Start.hostStart): the
     *  browser, mail, a share, an install, a settings page, a picker, the camera...; true when the
     *  host took it. A result it produces goes to the HostResult offered for `intent`
     *  (HostResults). */
    public static boolean macapkStartOnHost(Intent intent) {
        return resolveImplicitToHost(intent);
    }

    private static boolean resolveImplicitToHost(Intent intent) {
        // The intent under resolution, which for a chooser is the one INSIDE it.
        Intent real = unwrapChooser(intent);
        // The package UNinstaller's surface (ACTION_DELETE /
        // ACTION_UNINSTALL_PACKAGE with package: data -- Aurora Store's exact
        // uninstall shape). One line here, all knowledge in the backend; it
        // runs the SAME helper device-owner gate as PackageInstaller.uninstall,
        // and consent belongs to that prompt, not to any recency heuristic.
        // See PackageInstallerBackend.maybeHandleUninstallIntent.
        if (PackageInstallerBackend.maybeHandleUninstallIntent(real)) return true;
        HostAction act = hostActionFor(real);
        if (act == null) return false;
        // The install handoff is not a URL open: it claims the caller's
        // pending activity-result pairing (against the OUTER intent, the
        // identity startActivityForResult recorded) and answers it when the
        // helper finishes, so it must not fall through to openHost's
        // user-recency gate -- consent for an install belongs to the
        // helper's own confirm+auth step, not to a click-recency heuristic.
        if (act.kind == HostAction.Kind.INSTALL_PACKAGE) {
            // PackageInstaller's activities are dialog-themed
            // (Theme.AlertDialogActivity -> Theme.DeviceDefault.Light.Dialog.Alert,
            // floating; packages/PackageInstaller AndroidManifest.xml + themes.xml
            // android14), so the caller is paused, not stopped.
            return InstallIntentHandler.handle(intent, real, hostCover(false));
        }
        // Settings' special-access screens (stage S1). Same claim-the-result
        // shape as the installer, plus what a covering Settings activity does
        // to the caller: onPause/onStop while the out-of-process consent is up,
        // onRestart/onStart/onResume after. The stack and the lifecycle drivers
        // stay private to this class; SpecialAccessHandler gets posted
        // callbacks, the same pause+stop / restoreTop pair
        // macapkMoveTaskToBack / macapkTaskToFront already use.
        if (act.kind == HostAction.Kind.SPECIAL_ACCESS) {
            return SpecialAccessHandler.handle(intent, real, act.op, hostCover(true));
        }
        // Settings' App info (stage S5): an opaque Settings screen too.
        if (act.kind == HostAction.Kind.APP_DETAILS) {
            return SpecialAccessHandler.handleAppDetails(intent, real, hostCover(true));
        }
        if (act.kind == HostAction.Kind.DEVICE_CREDENTIAL) {
            return DeviceCredentialHandler.handle(intent, real, hostCover(false));
        }
        // DocumentsUI's picker: a full-screen activity on a device, so the
        // caller is paused AND stopped while it is up; the pick is the user's
        // own act inside the panel, so no recency gate applies.
        if (act.kind == HostAction.Kind.DOCUMENT_PICKER) {
            return DocumentPickerHandler.handle(intent, real, hostCover(true));
        }
        // The camera app's capture activity: full-screen, so the caller is
        // paused and stopped while the host's camera screen is up; the picture
        // is the user's own act there.
        if (act.kind == HostAction.Kind.IMAGE_CAPTURE) {
            return CameraCaptureHandler.handle(intent, real, hostCover(true));
        }
        // SHARE_TEXT's payload lives in the EXTRAS, which the predicate is not
        // allowed to read (see hostActionFor). Compose it HERE, where the
        // intent is being performed and is fully populated.
        String target = (act.kind == HostAction.Kind.SHARE_TEXT)
                      ? shareTextUrl(real) : act.url;
        if (target == null) return false;
        return openHost(target, real.getAction(), real);
    }

    /**
     * The intent a chooser is choosing FOR, or the intent itself when it is not
     * a chooser. Null when a chooser carries nothing to choose for.
     *
     * ⚠ v1.0.940. AOSP's chooser is an ACTIVITY (ResolverActivity /
     * ChooserActivity) that resolves the intent in EXTRA_INTENT and offers the
     * targets matching IT -- so `Intent.createChooser(share, "...")` hands
     * startActivity an ACTION_CHOOSER WRAPPER, never the share itself
     * (android/content/Intent.java:1358-1363 builds exactly that). Without this
     * step the ACTION_SEND work below would be nearly invisible in practice:
     * measured 2026-08-10 with `debug/appclass.py &lt;app&gt; --callers
     * createChooser`, 8 of 9 fleet apps sampled route their share through the
     * chooser (fdroid 5 classes, anki 7, aegis 4, newpipe / wikipedia /
     * antennapod 2 each, organicmaps / deskclock 1; opensudoku 0).
     *
     * Reading EXTRA_INTENT is NOT the extras test hostActionFor's header rules
     * out. That rule is about IntentFilter matching, where extras are payload;
     * EXTRA_INTENT is not payload, it IS the intent being resolved, and reading
     * it is precisely what ChooserActivity does.
     *
     * ⚠ KNOWN DIVERGENCE, deliberate: a device resolves ACTION_CHOOSER
     * unconditionally, because ResolverActivity declares a filter on the action
     * alone. We answer for the WRAPPED intent instead, so a chooser around an
     * image share still reports that nothing can handle it. Claiming a chooser
     * we cannot present, then doing nothing when it is started, would be the
     * dead-button bug this path exists to fix with a lie added on top.
     *
     * Bounded rather than recursive: an Intent can legally be put in its own
     * extras, and a self-referential wrapper must not run the stack out.
     */
    private static Intent unwrapChooser(Intent intent) {
        Intent cur = intent;
        for (int depth = 0; depth < 4; depth++) {
            if (cur == null || !Intent.ACTION_CHOOSER.equals(cur.getAction())) return cur;
            android.os.Parcelable inner = null;
            try { inner = cur.getParcelableExtra(Intent.EXTRA_INTENT); }
            catch (Throwable ignored) {}
            Intent wrapped = inner instanceof Intent ? (Intent) inner : null;
            // The chooser's OTHER targets (v1.0.1658, MACAPK_CHOOSER_INITIAL):
            // taken only when the wrapped intent itself is not one the host
            // can serve, so every chooser that resolved before resolves the
            // same way now. And only when there IS a wrapped intent: AOSP
            // ChooserActivity.onCreate (android12-release :619-623) finishes the
            // whole chooser with "Target is not an intent" when EXTRA_INTENT is
            // missing or not an Intent, before it looks at the initial intents.
            if (CHOOSER_INITIAL && wrapped != null
                    && !Intent.ACTION_CHOOSER.equals(wrapped.getAction())
                    && hostActionForDirect(wrapped) == null) {
                Intent initial = firstServableInitialIntent(cur);
                if (initial != null) return initial;
            }
            if (wrapped == null) return null;
            cur = wrapped;
        }
        return null;
    }

    /** MACAPK_CHOOSER_INITIAL (labelled bisect gate, default ON): a chooser's
     *  EXTRA_INITIAL_INTENTS are candidate targets too. =0 resolves a chooser
     *  for its EXTRA_INTENT alone, byte-for-byte the pre-v1.0.1658 path. */
    static final boolean CHOOSER_INITIAL = !"0".equals(System.getenv("MACAPK_CHOOSER_INITIAL"));

    /** ChooserActivity.MAX_EXTRA_INITIAL_INTENTS (android12-release :286). */
    private static final int MAX_EXTRA_INITIAL_INTENTS = 2;

    /**
     * The first of a chooser's EXTRA_INITIAL_INTENTS the host can serve, or null.
     *
     * AOSP Intent.EXTRA_INITIAL_INTENTS: "A Parcelable[] of Intent or
     * LabeledIntent objects ... of additional activities to place at the front
     * of the list of choices, when shown to the user with an ACTION_CHOOSER."
     * ChooserActivity (frameworks/base core/java/com/android/internal/app/
     * ChooserActivity.java, onCreate) reads them into the SAME target list as
     * the resolved EXTRA_INTENT, so on a device the chooser offers them even
     * when nothing at all resolves the wrapped intent. That is the common
     * "pick a picture" idiom: Intent.createChooser(ACTION_PICK of
     * MediaStore.Images, ...) plus an initial ACTION_GET_CONTENT of image/*.
     * AOSP ships no ACTION_PICK handler for images (MediaProvider's
     * PhotoPickerActivity filters PICK_IMAGES and, via the
     * PhotoPickerGetContentActivity alias, GET_CONTENT of image/ and video/;
     * packages/providers/MediaProvider AndroidManifest.xml), so on a stock
     * device the GET_CONTENT entry is the one that works. Before this we
     * resolved the chooser for ACTION_PICK alone, answered "nothing handles
     * it" and the import button was dead.
     *
     * A LabeledIntent is an Intent subclass, so instanceof covers both. A
     * nested chooser inside the initial list is not followed (ChooserActivity
     * would list it as one opaque target).
     *
     * Bounds, as ChooserActivity.onCreate (android12-release :674-689) has them:
     * at most MAX_EXTRA_INITIAL_INTENTS = 2 (:286) entries are read
     * (`Math.min(pa.length, MAX_EXTRA_INITIAL_INTENTS)`, :677), and a non-Intent
     * entry among those finishes the whole chooser (:680-684, "Initial intent #i
     * not an Intent") -- so here it makes the chooser unservable (null), not a
     * skipped entry.
     */
    private static Intent firstServableInitialIntent(Intent chooser) {
        android.os.Parcelable[] initial = null;
        try { initial = chooser.getParcelableArrayExtra(Intent.EXTRA_INITIAL_INTENTS); }
        catch (Throwable ignored) {}
        if (initial == null) return null;
        int count = Math.min(initial.length, MAX_EXTRA_INITIAL_INTENTS);
        for (int n = 0; n < count; n++) {
            android.os.Parcelable p = initial[n];
            if (!(p instanceof Intent)) return null;
            Intent i = (Intent) p;
            if (Intent.ACTION_CHOOSER.equals(i.getAction())) continue;
            if (hostActionForDirect(i) != null) return i;
        }
        return null;
    }

    /** A macOS-native realization of an implicit intent: WHAT the host would
     *  do, kept separate from the URL that does it, because not every host
     *  action is "open this link". */
    static final class HostAction {
        enum Kind {
            /** Hand {@link HostAction#url} to NSWorkspace: a web / geo / mailto
             *  / tel link. */
            OPEN_URL,
            /** Share text. The payload is in the intent's EXTRAS, so
             *  {@link HostAction#url} is null here and the real URL is composed
             *  at START time by {@code shareTextUrl} -- resolution must never
             *  depend on extras. */
            SHARE_TEXT,
            /** The system package installer's surface (ACTION_INSTALL_PACKAGE,
             *  or ACTION_VIEW of an application/vnd.android.package-archive).
             *  {@link HostAction#url} is null: the payload is the data URI,
             *  read at PERFORM time by InstallIntentHandler, which stages the
             *  bytes and hands them to the macOS install helper. v1.0.953. */
            INSTALL_PACKAGE,
            /** Settings' special-access screens (All files access, media
             *  management): {@link HostAction#op} is the app-op the screen
             *  toggles; SpecialAccessHandler runs the out-of-process consent.
             *  Stage S1. */
            SPECIAL_ACCESS,
            /** Settings' App info (ACTION_APPLICATION_DETAILS_SETTINGS with
             *  package: data): SpecialAccessHandler runs the out-of-process
             *  App info alert. Stage S5, MACAPK_APP_DETAILS_SETTINGS. */
            APP_DETAILS,
            /** DocumentsUI's picker (ACTION_GET_CONTENT / OPEN_DOCUMENT /
             *  CREATE_DOCUMENT): DocumentPickerHandler shows the macOS open/save
             *  panel and answers with HostDocumentsProvider URIs. v1.0.1653,
             *  MACAPK_DOC_PICKER. */
            DOCUMENT_PICKER,
            /** The camera app's capture activity (MediaStore.ACTION_IMAGE_CAPTURE):
             *  CameraCaptureHandler shows the host's camera screen and writes the
             *  picture to EXTRA_OUTPUT. MACAPK_IMAGE_CAPTURE. */
            IMAGE_CAPTURE,
            /** Real macOS password verification for Android Keyguard's
             * confirm-device-credential activity. */
            DEVICE_CREDENTIAL
        }
        final Kind kind;
        final String url;
        /** SPECIAL_ACCESS only: the app-op string; null otherwise. */
        final String op;
        HostAction(Kind kind, String url) { this(kind, url, null); }
        HostAction(Kind kind, String url, String op) {
            this.kind = kind; this.url = url; this.op = op;
        }
    }

    /**
     * What macOS would do with this implicit intent, or null when the host can
     * do nothing with it. The PURE predicate half of resolveImplicitToHost.
     *
     * ⚠ v1.0.938: extracted so `MacApkPackageManager.queryIntentActivities` can
     * ask the SAME question the start path asks. Before this, Mac.apk could
     * PERFORM these intents but reported that nothing could handle them --
     * queryIntentActivities only ever scanned the app's own manifest. The
     * near-universal guard
     *
     *     if (intent.resolveActivity(getPackageManager()) != null) startActivity(intent);
     *
     * therefore took the "no app can handle this" branch, so share buttons,
     * "open in browser", "email us" and "call" were silently dead across the
     * fleet even though the handoff behind them worked. That is the same shape
     * as the GMS-era bug the changelog calls "answering one question four
     * contradictory ways".
     *
     * Kept as ONE method on purpose: two copies of "can the host handle this?"
     * is exactly how the inconsistency arose.
     *
     * ⚠ v1.0.940 -- WHY THIS RETURNS A DESCRIPTOR RATHER THAN A URL STRING.
     * It used to return the URL NSWorkspace would be handed, and that shape can
     * only express "open this link". ACTION_SEND is not "open this link", so
     * the SEND branch tested whether EXTRA_TEXT happened to BE an http(s) URL
     * and opened that -- a category error, not merely a narrow test. Android
     * resolves an intent on action / type / scheme / data / categories and
     * NEVER on extras, which is provable from the signature alone:
     * frameworks/base/core/java/android/content/IntentFilter.java:1805
     * (android11-release) is
     *     match(String action, String type, String scheme, Uri data,
     *           Set&lt;String&gt; categories, String logTag)
     * and its dispatcher at :1770 decomposes the Intent into exactly those five
     * fields, dropping the extras on the floor. So a device answers "yes,
     * something can share text/plain" BEFORE the app has put any text in the
     * intent -- which is the normal order an app does it in (resolve first,
     * fill extras second), and therefore the order the old test got wrong.
     *
     * The descriptor is what lets the ANSWER stay extras-independent while the
     * start path still does something different per kind.
     */
    static HostAction hostActionFor(Intent intent) {
        // A chooser resolves for the intent it wraps -- see unwrapChooser.
        return hostActionForDirect(unwrapChooser(intent));
    }

    /** hostActionFor for an intent that is NOT a chooser wrapper (unwrapChooser
     *  asks this about each candidate, so it must not unwrap again). */
    private static HostAction hostActionForDirect(Intent intent) {
        if (intent == null) return null;
        String action = intent.getAction();
        if (action == null) return null;
        android.net.Uri data = intent.getData();
        String uri = data != null ? data.toString() : null;
        String scheme = data != null ? data.getScheme() : null;

        boolean isView = Intent.ACTION_VIEW.equals(action);
        boolean isSendto = Intent.ACTION_SENDTO.equals(action);
        boolean isDial = "android.intent.action.DIAL".equals(action)
                || "android.intent.action.CALL".equals(action);

        // Web / geo / generic content links open in the host browser/handler.
        if (isView && scheme != null
                && (scheme.equals("http") || scheme.equals("https")
                    || scheme.equals("geo") || scheme.equals("mailto")
                    || scheme.equals("tel"))) {
            return new HostAction(HostAction.Kind.OPEN_URL, uri);
        }
        // mailto: via SENDTO, or a mailto: data URI.
        if (isSendto && uri != null && uri.startsWith("mailto:")) {
            return new HostAction(HostAction.Kind.OPEN_URL, uri);
        }
        // tel: via DIAL/CALL.
        if (isDial && uri != null && uri.startsWith("tel:")) {
            return new HostAction(HostAction.Kind.OPEN_URL, uri);
        }
        // Settings' location screen (an app's "turn on location"): the host's
        // own location settings, where the host names one (Windows).
        if (android.provider.Settings.ACTION_LOCATION_SOURCE_SETTINGS.equals(action)) {
            String settings = LocationBridge.hostSettingsUrl();
            if (settings != null) return new HostAction(HostAction.Kind.OPEN_URL, settings);
        }
        // The package installer's surface. On a device these resolve to the
        // platform packageinstaller app (android-9.0.0_r1 packages/apps/
        // PackageInstaller/AndroidManifest.xml: .InstallStart takes
        // {VIEW, INSTALL_PACKAGE} x schemes {file, content} x mimeType
        // application/vnd.android.package-archive, plus INSTALL_PACKAGE alone
        // on {file, package, content}); Mac.apk's equivalent is the install
        // helper inside the .app bundle. Predicate lives with the handler so
        // the install knowledge stays in one class; it tests action / scheme /
        // explicit type only, per this method's extras rule.
        if (InstallIntentHandler.isInstallIntent(intent)) {
            return new HostAction(HostAction.Kind.INSTALL_PACKAGE, null);
        }
        // Settings' special-access screens (stage S1): the four filters AOSP
        // Settings declares for MANAGE_APP_ALL_FILES_ACCESS_PERMISSION (package:
        // data required), MANAGE_ALL_FILES_ACCESS_PERMISSION (no data) and
        // REQUEST_MANAGE_MEDIA (either). The predicate, with the filter-matching
        // details, lives with the handler; null with MACAPK_ALLFILES_HONEST=0.
        String specialOp = SpecialAccessHandler.opFor(intent);
        if (specialOp != null) {
            return new HostAction(HostAction.Kind.SPECIAL_ACCESS, null, specialOp);
        }
        // Settings' App info (stage S5): InstalledAppDetails' one filter --
        // APPLICATION_DETAILS_SETTINGS or AUTO_REVOKE_PERMISSIONS, category
        // DEFAULT, package: data required. null with
        // MACAPK_APP_DETAILS_SETTINGS=0.
        if (SpecialAccessHandler.isAppDetailsIntent(intent)) {
            return new HostAction(HostAction.Kind.APP_DETAILS, null);
        }
        if (DeviceCredentialHandler.isCredentialIntent(intent)) {
            return new HostAction(HostAction.Kind.DEVICE_CREDENTIAL, null);
        }
        // ACTION_SEND of TEXT -- matched on action + MIME type ONLY. See the
        // header for why reading EXTRA_TEXT here was the wrong question.
        //
        // The type scope is deliberately text/*, and the reason is the
        // no-shortcuts rule rather than convenience: text is the share macOS
        // can actually PERFORM through the one host bridge that exists. An
        // ACTION_SEND of image/* (or any EXTRA_STREAM content:// share) is a
        // FILE share, no URL expresses it, and there is no NSSharingService
        // bridge yet -- so we keep answering "nothing can handle this" for
        // those rather than resolving something we would then have to fake.
        // ACTION_SEND_MULTIPLE is the same story and is likewise not claimed.
        if (Intent.ACTION_SEND.equals(action)) {
            String type = intent.getType();
            if (type != null && type.startsWith("text/")) {
                return new HostAction(HostAction.Kind.SHARE_TEXT, null);
            }
        }
        if (DocumentPickerHandler.isPickerIntent(intent)) {
            return new HostAction(HostAction.Kind.DOCUMENT_PICKER, null);
        }
        if (CameraCaptureHandler.isCaptureIntent(intent)) {
            return new HostAction(HostAction.Kind.IMAGE_CAPTURE, null);
        }
        return null;
    }

    /**
     * The macOS URL that actually PERFORMS an ACTION_SEND text share, or null.
     *
     * ⚠ THE HONEST LIMIT, stated here because claiming to resolve a share and
     * then doing nothing would be a worse answer than not resolving it at all.
     * macOS's real equivalent of the Android chooser is NSSharingServicePicker,
     * and reaching it needs a native bridge alongside Cocoa_openHostUrl
     * (runtime/src/cocoa_bridge.mm) that does not exist yet. What we have is
     * `NSWorkspace openURL:`, and the one share target expressible as a URL is
     * the system mail composer: RFC 6068 2 gives
     *     mailtoURI = "mailto:" [ to ] [ hfields ]
     * so the address is OPTIONAL, and "body" is the pseudo-header that section
     * defines for the message text. A mail app is also exactly what Android's
     * own chooser offers for text/plain, so this is a NARROWING of the chooser
     * to one target, not a different action.
     *
     * Nothing about the outcome is reported back to the app: ACTION_SEND is
     * fire-and-forget with no result, so there is no way for this to lie to the
     * caller the way a for-result intent could.
     *
     * Extras are read HERE and never in the predicate, so what we RESOLVE never
     * depends on what the app has filled in yet. That also makes the empty case
     * coherent: a share carrying no text opens an empty compose window, which
     * is what a mail app does with the same intent.
     */
    private static String shareTextUrl(Intent intent) {
        if (intent == null) return null;
        // AOSP declares both extras as CharSequence (Intent.EXTRA_TEXT is
        // "CharSequence", EXTRA_SUBJECT "String"), and apps do put Spanned text
        // in EXTRA_TEXT -- getCharSequenceExtra covers String and Spanned both.
        CharSequence text = null, subject = null;
        try { text    = intent.getCharSequenceExtra(Intent.EXTRA_TEXT); }    catch (Throwable ignored) {}
        try { subject = intent.getCharSequenceExtra(Intent.EXTRA_SUBJECT); } catch (Throwable ignored) {}
        StringBuilder sb = new StringBuilder("mailto:");
        char sep = '?';
        if (subject != null && subject.length() > 0) {
            sb.append(sep).append("subject=")
              .append(android.net.Uri.encode(subject.toString()));
            sep = '&';
        }
        if (text != null && text.length() > 0) {
            sb.append(sep).append("body=")
              .append(android.net.Uri.encode(text.toString()));
        }
        return sb.toString();
    }

    private static Boolean s_allowHostUrlAtLaunch = null;
    private static boolean allowHostUrlAtLaunch() {
        if (s_allowHostUrlAtLaunch == null) {
            String v = System.getenv("MACAPK_ALLOW_HOST_URL_AT_LAUNCH");
            s_allowHostUrlAtLaunch = (v != null && !v.isEmpty() && !v.equals("0"));
        }
        return s_allowHostUrlAtLaunch;
    }

    // v1.0.849: how recent a real input must be for a host-URL open to count
    // as user-initiated. The v1.0.822 one-shot guard ("has the user EVER
    // touched") stopped only the pre-first-touch hijack; one tap then armed
    // host opens for the app's whole lifetime, so ad SDKs firing ACTION_VIEW
    // minutes later still threw the user into Safari (owner report 2026-08-01:
    // "we still have multiple apps opening browser links"; measured: subway
    // opened an ad tracker mid-campaign after one harness tap). Android's own
    // rule for background activity starts is a RECENT-user-interaction grace
    // window, not ever-interacted -- mirror that. A human tapping a real link
    // reaches this code within milliseconds, so 10s is generous for the
    // legitimate case and hopeless for a delayed ad fire.
    private static Long s_hostUrlGraceMs = null;
    private static long hostUrlGraceMs() {
        if (s_hostUrlGraceMs == null) {
            long v = 10_000L;
            String e = System.getenv("MACAPK_HOST_URL_GRACE_MS");
            if (e != null && !e.isEmpty()) {
                try { v = Long.parseLong(e); } catch (NumberFormatException ignored) {}
            }
            s_hostUrlGraceMs = v;
        }
        return s_hostUrlGraceMs;
    }
    // v1.0.849: hard kill-switch for unattended harness runs. A CGEvent-
    // synthesized click (debug/click.swift) is a REAL NSEvent by the time it
    // reaches TouchDispatcher -- indistinguishable from a human -- so the
    // recency gate alone cannot protect an automated sweep. The campaign
    // harness sets MACAPK_EXTERNAL_URLS=0 and no host URL ever opens.
    private static Boolean s_externalUrlsOff = null;
    private static boolean externalUrlsOff() {
        if (s_externalUrlsOff == null) {
            s_externalUrlsOff = "0".equals(System.getenv("MACAPK_EXTERNAL_URLS"));
        }
        return s_externalUrlsOff;
    }

    /** MACAPK_HOST_URL_DRY_RUN=1 (test hook, default off): openHost logs the
     *  URL it would hand the host (NSWorkspace / ShellExecute) and reports it
     *  opened, instead of opening it, so an unattended run never pops the
     *  user's browser. Checked on the Java side, before the native call. */
    private static final boolean HOST_URL_DRY_RUN =
            "1".equals(System.getenv("MACAPK_HOST_URL_DRY_RUN"));
    static boolean hostUrlDryRun() { return HOST_URL_DRY_RUN; }

    /** v1.0.849: the ONE policy choke point for handing a URL to the host.
     *  Everything that can open the macOS browser must come through here --
     *  ActivityHost's implicit-intent resolution and URLSpan.onClick both do.
     *  Package-visible so framework shims reach it via this loader class. */
    public static boolean openHostUrlGated(String uri, String why) {
        return openHost(uri, why, null);
    }

    /** `intent`: the intent being performed, when there is one (a URLSpan tap
     *  has none). A Custom Tab or TWA launch of a web URL opens in the
     *  runtime's own browser tab where the host has one (HostBrowserTab),
     *  after the same two policy gates; everything else goes to the host. */
    private static boolean openHost(String uri, String action, Intent intent) {
        if (uri == null) return false;
        // v1.0.822 (revised v1.0.849, see hostUrlGraceMs above): a host-URL
        // open must be USER-INITIATED. Original motivation stands: Simon
        // Tatham's Puzzles fired ACTION_VIEW at its Play Store page from
        // onCreate and every launch threw the user into Safari. Refusing is
        // the Android-faithful answer: on a device this intent is reached
        // because a human just tapped something.
        //
        // MACAPK_ALLOW_HOST_URL_AT_LAUNCH=1 restores the old always-open
        // behaviour; MACAPK_EXTERNAL_URLS=0 refuses every open.
        // A user-initiated browser open takes the shared sign-in redirect schemes
        // this app claims (HostUrlIntake.claimRedirectSchemes: the redirect must
        // return to the tab's opener). The decision is HostUrlIntake.shouldClaim,
        // which is exactly "this open happens and a person made it": a programmatic
        // open (a launch-time ad / rate-us) must not steal another app's in-flight
        // sign-in redirect, and a REFUSED open (MACAPK_EXTERNAL_URLS=0) must not
        // rewrite the machine-global LaunchServices default (the harness-only
        // MACAPK_URL_REDIRECT_CLAIM_ON_REFUSAL=1 is the one exception, honoured only
        // in that branch). The claim is made AFTER both refusal checks, just before
        // the open. debug/urlscheme_gate.sh P5-P8 guard this ordering.
        final boolean claim = HostUrlIntake.shouldClaim(allowHostUrlAtLaunch(),
                TouchDispatcher.lastUserInputMillis(), System.currentTimeMillis(),
                hostUrlGraceMs(), externalUrlsOff(), HostUrlIntake.CLAIM_ON_REFUSAL);
        if (externalUrlsOff()) {
            if (claim) HostUrlIntake.claimRedirectSchemes(uri);   // harness opt-in only
            NativeLog.w(TAG, "REFUSED " + action + " -> host open(" + uri
                    + "): MACAPK_EXTERNAL_URLS=0 (harness run)");
            return false;
        }
        if (!allowHostUrlAtLaunch()) {
            long last = TouchDispatcher.lastUserInputMillis();
            long age = (last == 0L) ? Long.MAX_VALUE
                                    : (System.currentTimeMillis() - last);
            if (age > hostUrlGraceMs()) {
                NativeLog.w(TAG, "REFUSED " + action + " -> host open(" + uri
                        + "): last real input "
                        + (last == 0L ? "NEVER" : (age + "ms ago"))
                        + " (> " + hostUrlGraceMs() + "ms grace) -- treating as a"
                        + " programmatic browser hijack, not a user click."
                        + " MACAPK_ALLOW_HOST_URL_AT_LAUNCH=1 to allow.");
                return false;
            }
        }
        // The runtime's own browser tab: not a host open, so neither the dry
        // run below nor a redirect-scheme claim applies (the tab returns the
        // redirect to this app itself).
        if (intent != null && HostBrowserTab.handles(uri, intent) && HostBrowserTab.open(uri, intent)) {
            NativeLog.i(TAG, "implicit intent " + action + " (Custom Tab) -> browser tab("
                    + HostUrlIntake.redact(uri) + ")");
            return true;
        }
        return openHostNow(uri, action, claim);
    }

    /** The host open itself, past the policy gates. */
    static boolean openHostNow(String uri, String action, boolean claim) {
        if (hostUrlDryRun()) {
            // Test hook: everything up to the host call ran (resolution, both
            // policy gates), and the open is reported as done so the app goes
            // on as it would after a browser opened -- but no browser opens and
            // no redirect scheme is claimed.
            NativeLog.i(TAG, "MACAPK_HOST_URL_DRY_RUN: implicit intent " + action
                    + " -> host open(" + uri + ") NOT performed (dry run)");
            return true;
        }
        if (claim) HostUrlIntake.claimRedirectSchemes(uri);
        // A sign-in whose redirect_uri is one of this app's VERIFIED App Links
        // opens in the Mac.apk sign-in window, which hands that redirect back
        // the way a device browser does; Safari would load it as a page.
        if (AppLinks.maybeOpenSignInWindow(uri)) return true;
        try {
            boolean ok = NativeCocoa.openHostUrl(uri);
            NativeLog.i(TAG, "implicit intent " + action + " -> host open("
                    + uri + ") = " + ok);
            return ok;
        } catch (Throwable t) {
            NativeLog.w(TAG, "host open failed for " + uri + ": " + t);
            return false;
        }
    }

    /**
     * Stages 2a/2b/2e (START_RESOLVE): route one start through the ONE
     * resolver. Returns the intent to launch in THIS app -- the caller's own
     * object for an explicit start, or a COPY carrying the resolved component
     * for an implicit one (the caller's intent is never mutated) -- or null
     * when the start was performed on the host or dropped.
     *
     * Order, as ActivityStarter: resolution errors first. ActivityNotFoundException
     * (START_ANF) leaves on the caller's thread with AOSP's message shapes.
     *
     * Two deliberate, stated divergences until the cross-app stages land
     * (docs/cross-app-launch-and-implicit-start-design_2026-09-26.md):
     *  - an EXPLICIT start of a component in ANOTHER package our PM reports as
     *    installed (a Mac.apk registry app, a GMS stand-in) keeps the pre-change
     *    path (performLaunch misses and logs). Throwing "not found" for an app
     *    that IS installed would be a lie; stage 3a hands it to that app.
     *  - an EXPLICIT start of an undeclared class in OUR package still launches
     *    (Class.forName finds it). A device throws; our manifest view of a split
     *    bundle's feature activities is not yet trusted enough to throw on it.
     */
    private static Intent macapkRouteStart(Intent intent) {
        // Every start (ActivityStarter.java:688-691). Inert today:
        // Bundle.hasFileDescriptors is a constant false.
        if (intent.hasFileDescriptors()) {
            throw new IllegalArgumentException("File descriptors passed in Intent");
        }
        final String own = app != null ? app.getPackageName() : null;
        final ComponentName cn = intent.getComponent();
        if (cn != null) {
            final String pkg = cn.getPackageName();
            // A caller may pin the exact Settings component returned by our
            // resolver. It performs the same real host check as the implicit
            // Keyguard intent, before cross-app resolution is considered.
            if (DeviceCredentialHandler.isCredentialIntent(intent) && resolveImplicitToHost(intent)) return null;
            if (own == null || own.equals(pkg)) return intent;
            // The platform package installer, pinned by the caller (a store
            // picks InstallStart out of queryIntentActivities and setClassName()s
            // it): performed by the installer surface, as the implicit start is.
            if (MacApkPackageManager.macapkInstallerComponent()
                    && MacApkPackageManager.macapkIsInstallerPackage(pkg)
                    && InstallIntentHandler.isInstallIntent(intent)
                    && resolveImplicitToHost(intent)) {
                return null;
            }
            // The camera app's capture activity, pinned from the row the PM
            // lists (CameraCaptureHandler.resolveInfo).
            if (CameraCaptureHandler.isCameraComponent(cn) && CameraCaptureHandler.isCaptureIntent(intent)
                    && resolveImplicitToHost(intent)) {
                return null;
            }
            // Stage 3a: a component of ANOTHER installed Mac.apk app is handed to
            // that app (resolveForStart: not narrowed by package visibility).
            if (CrossAppLauncher.XAPP_START && macapkCrossAppStart(own, intent, null)) return null;
            if (macapkPackageInstalled(pkg)) return intent;
            if (START_ANF) throw macapkAnf(intent);
            return intent;
        }
        java.util.List<android.content.pm.ResolveInfo> rows = java.util.Collections.emptyList();
        try {
            android.content.pm.PackageManager pm = app != null ? app.getPackageManager() : null;
            if (pm != null) {
                // resolveForStart: registry rows are not narrowed by package
                // visibility (MacApkPackageManager stage 4).
                rows = MacApkPackageManager.macapkQueryForStart(pm, intent,
                        android.content.pm.PackageManager.MATCH_DEFAULT_ONLY);
            }
        } catch (Throwable t) {
            NativeLog.w(TAG, "start: resolver threw (" + t + ") -- host path only");
        }
        // A WEB intent not pinned to a package never resolves to the app's OWN
        // deep-link row: Android 12+ opens an unverified web link in the browser
        // (DomainVerificationManager; ComputerEngine
        // filterCandidatesWithDomainPreferredActivitiesLPr), and we never
        // perform Digital Asset Links verification. Without this, an app's own
        // "open in browser" for its own site would route straight back into it,
        // because the PM lists the app's row before the host browser row.
        final boolean web = macapkIsWebIntent(intent) && intent.getPackage() == null;
        final boolean exportedOnly = START_EXPORTED_ONLY
                && intent.getPackage() == null && macapkCallerTargetsU();
        android.content.pm.ResolveInfo pick = null;
        for (android.content.pm.ResolveInfo ri : rows) {
            if (ri == null || ri.activityInfo == null) continue;
            final boolean mine = own != null && own.equals(ri.activityInfo.packageName);
            if (mine && web) continue;
            if (exportedOnly && !ri.activityInfo.exported) continue;
            pick = ri;
            break;
        }
        if (pick == null) {
            // The host still performs what the PM does not list (the package
            // uninstaller's surface); only after that is the start unresolved.
            if (resolveImplicitToHost(intent)) return null;
            if (START_ANF) throw macapkAnf(intent);
            NativeLog.w(TAG, "startActivity implicit intent unresolved (action="
                    + intent.getAction() + " data=" + intent.getDataString() + ") -- ignored");
            return null;
        }
        if (own != null && own.equals(pick.activityInfo.packageName)) {
            Intent copy = new Intent(intent);
            copy.setComponent(new ComponentName(own, pick.activityInfo.name));
            NativeLog.i(TAG, "startActivity implicit (action=" + intent.getAction()
                    + ") -> own " + pick.activityInfo.name);
            return copy;
        }
        // Mac.apk's GMS stand-in has no process of its own: its sign-in activity
        // (a framework class) runs as an activity of the CALLING app's task, so
        // the for-result round trip is the ordinary in-app one. On a device the
        // component's package is com.google.android.gms; here it is the caller's
        // (the activity reads its request from the intent, never from that).
        if (own != null && com.macapk.gms.GoogleSignInBroker.isInProcessActivity(pick.activityInfo)) {
            Intent copy = new Intent(intent);
            copy.setComponent(new ComponentName(own, pick.activityInfo.name));
            NativeLog.i(TAG, "startActivity implicit (action=" + intent.getAction()
                    + ") -> in-process GMS " + pick.activityInfo.name);
            return copy;
        }
        // Stage 3a: a row owned by ANOTHER installed Mac.apk app (the query was
        // scoped to it with setPackage): hand the start to that app.
        if (CrossAppLauncher.XAPP_START && macapkCrossAppStart(own, intent, pick.activityInfo)) {
            return null;
        }
        // A host or Settings row: perform it. A POLICY refusal (the recency
        // grace window, MACAPK_EXTERNAL_URLS=0) is silent -- that is the
        // background-activity-launch START_ABORTED answer, not "not found".
        if (!resolveImplicitToHost(intent)) {
            NativeLog.i(TAG, "startActivity implicit (action=" + intent.getAction()
                    + ") resolved to " + pick.activityInfo.packageName
                    + " but the host refused it (policy) -- aborted");
        }
        return null;
    }

    /** Stage 3a: if `intent` targets a component of another INSTALLED Mac.apk
     *  app, hand it to that app and return true. `resolved` is the resolver's
     *  row for an implicit start, or null for an explicit component (looked up
     *  resolveForStart-style). May throw ActivityNotFoundException (the bundle
     *  is not really there) or SecurityException (not exported / permission). */
    private static boolean macapkCrossAppStart(String own, Intent intent,
                                               android.content.pm.ActivityInfo resolved) {
        android.content.pm.PackageManager pm = app != null ? app.getPackageManager() : null;
        if (!(pm instanceof MacApkPackageManager)) return false;
        MacApkPackageManager mpm = (MacApkPackageManager) pm;
        android.content.pm.ActivityInfo ai = resolved;
        if (ai == null) ai = mpm.macapkRegistryActivityForStart(intent.getComponent());
        if (ai == null || ai.packageName == null || ai.packageName.equals(own)) return false;
        String[] bundle = mpm.macapkRegistryBundle(ai.packageName);
        if (bundle == null) return false;
        CrossAppLauncher.start(own, pm, intent, ai, bundle);
        return true;
    }

    /** Instrumentation.checkStartActivityResult's two shapes (android14
     *  :2232-2240). */
    private static android.content.ActivityNotFoundException macapkAnf(Intent intent) {
        if (intent.getComponent() != null) {
            return new android.content.ActivityNotFoundException(
                    "Unable to find explicit activity class "
                    + intent.getComponent().toShortString()
                    + "; have you declared this activity in your AndroidManifest.xml"
                    + ", or does your intent not match its declared <intent-filter>?");
        }
        return new android.content.ActivityNotFoundException(
                "No Activity found to handle " + intent);
    }

    /** Is `pkg` installed per OUR PackageManager (a registry app, a host /
     *  Settings stand-in, a GMS answer)? */
    private static boolean macapkPackageInstalled(String pkg) {
        try {
            android.content.pm.PackageManager pm = app != null ? app.getPackageManager() : null;
            return pm != null && pm.getPackageInfo(pkg, 0) != null;
        } catch (Throwable t) {
            return false;
        }
    }

    /** AOSP Intent.isWebIntent: ACTION_VIEW with an http/https data URI. */
    /** AOSP Intent.isWebIntent (ACTION_VIEW of an http/https URI), for the PM's
     *  stage-4 registry scan. */
    static boolean macapkWebIntent(Intent intent) {
        return intent != null && macapkIsWebIntent(intent);
    }

    private static boolean macapkIsWebIntent(Intent intent) {
        if (!Intent.ACTION_VIEW.equals(intent.getAction())) return false;
        android.net.Uri d = intent.getData();
        String s = d != null ? d.getScheme() : null;
        return "http".equalsIgnoreCase(s) || "https".equalsIgnoreCase(s);
    }

    /** Stage 2e's caller test: targetSdk KNOWN (a real 0 is known and below)
     *  and at least UPSIDE_DOWN_CAKE (34). */
    private static boolean macapkCallerTargetsU() {
        try {
            int t = app != null && app.getApplicationInfo() != null
                    ? app.getApplicationInfo().targetSdkVersion : -1;
            return com.macapk.loader.app.Apps.targetSdkKnown(t) && t >= 34;
        } catch (Throwable t) {
            return false;
        }
    }

    /** Stage 2d: the targetActivity of an <activity-alias>, or null. */
    private static String macapkAliasTarget(ComponentName cn) {
        try {
            android.content.pm.PackageManager pm = app != null ? app.getPackageManager() : null;
            android.content.pm.ActivityInfo ai = pm != null ? pm.getActivityInfo(cn, 0) : null;
            String t = ai != null ? ai.targetActivity : null;
            return (t != null && !t.isEmpty() && !t.equals(cn.getClassName())) ? t : null;
        } catch (Throwable t) {
            return null;
        }
    }
}
