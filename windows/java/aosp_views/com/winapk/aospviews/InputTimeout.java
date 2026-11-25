// Input the app was given and has not taken: Android's input-dispatching ANR. AOSP 14's InputDispatcher
// reports an app whose window has not finished an event within DEFAULT_DISPATCHING_TIMEOUT (5 s) to
// ActivityManager, which shows "<app> isn't responding" with Close app and Wait over it
// (AppNotRespondingDialog); Close app kills the process, Wait leaves it running.
//
// Here the host's input reaches the app as messages on its main looper (AospInput): an event is pending
// from its post until the main thread runs it, which it cannot while it is stuck. A watchdog thread looks
// twice a second; once the oldest event has waited five seconds it asks the user through the launcher
// (Win.apk.exe app-not-responding, the launcher's dialog over the app's window, as the permission questions
// are asked). Wait drops the events given until then, as InputDispatcher drops the event it reported: new
// input starts a new five seconds. Close app ends the process with CLOSED_EXIT, which the launcher takes as
// the user's word (include/winapk/app_exit.h). A pointer only passing over the window (hover) is no input
// an app is waited for. Nothing is watched in a run with no user (WINAPK_HEADLESS).
package com.winapk.aospviews;

import android.os.SystemClock;
import android.util.Log;

import java.util.ArrayDeque;

final class InputTimeout {
    private static final String TAG = "Win.apk.InputTimeout";

    /** InputDispatcher's DEFAULT_DISPATCHING_TIMEOUT. */
    static final long DISPATCHING_TIMEOUT_MS = 5000;

    /** The exit of a process the user closed from the dialog; app_exit.h's kExitClosedNotResponding. */
    static final int CLOSED_EXIT = 0x10000009;

    private static final boolean WATCHED = !"1".equals(System.getenv("WINAPK_HEADLESS"));

    private static final Object LOCK = new Object();
    /** The events posted and not yet run, oldest first: {sequence, uptime when posted}. */
    private static final ArrayDeque<long[]> PENDING = new ArrayDeque<>();
    private static long sSeq;
    /** Events up to this sequence were reported, and Wait dropped them. */
    private static long sDropped;
    private static Thread sWatchdog;

    private InputTimeout() {}

    /** `r`, which hands one input event to the app on its main thread, counted as pending until it runs. */
    static Runnable track(Runnable r) {
        if (!WATCHED) return r;
        final long[] e;
        synchronized (LOCK) {
            e = new long[] {++sSeq, SystemClock.uptimeMillis()};
            PENDING.addLast(e);
            if (sWatchdog == null) {
                sWatchdog = new Thread(InputTimeout::watch, "winapk-input-timeout");
                sWatchdog.setDaemon(true);
                sWatchdog.start();
            }
        }
        return () -> {
            synchronized (LOCK) {
                PENDING.remove(e);
            }
            r.run();
        };
    }

    private static void watch() {
        for (;;) {
            try {
                Thread.sleep(500);
            } catch (InterruptedException ie) {
                return;
            }
            long seq = 0, waited = 0;
            synchronized (LOCK) {
                for (long[] e : PENDING) {
                    if (e[0] <= sDropped) continue;
                    seq = e[0];
                    waited = SystemClock.uptimeMillis() - e[1];
                    break;
                }
            }
            if (seq == 0 || waited < DISPATCHING_TIMEOUT_MS) continue;
            Log.w(TAG, "ANR: input event " + seq + " not taken by the main thread in " + waited
                    + " ms (InputDispatcher's timeout is " + DISPATCHING_TIMEOUT_MS + " ms); asking the user");
            boolean close = ask();
            if (close) {
                Log.w(TAG, "ANR: Close app");
                try {
                    com.macapk.loader.NativeCocoa.exitProcess(CLOSED_EXIT, null);
                } catch (Throwable ignored) {
                    // A plain JVM.
                }
                Runtime.getRuntime().halt(CLOSED_EXIT);
            }
            synchronized (LOCK) {
                sDropped = sSeq;
            }
            Log.i(TAG, "ANR: Wait");
        }
    }

    /** The launcher's dialog; true for Close app. Without a launcher to ask, the app is left running. */
    private static boolean ask() {
        java.io.File helper = com.macapk.loader.PackageInstallerBackend.locateHelper();
        if (helper == null) {
            Log.e(TAG, "ANR: no Win.apk.exe to ask; the app keeps running");
            return false;
        }
        try {
            Process p = new ProcessBuilder(helper.getAbsolutePath(), "app-not-responding", "--source", "android",
                    "--json").redirectErrorStream(true).start();
            try (java.io.InputStream in = p.getInputStream()) {
                in.transferTo(java.io.OutputStream.nullOutputStream());
            }
            return p.waitFor() == 3;
        } catch (Exception e) {
            Log.e(TAG, "ANR: the question failed: " + e);
            return false;
        }
    }
}
