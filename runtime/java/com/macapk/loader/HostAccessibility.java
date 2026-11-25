// com.macapk.loader.HostAccessibility -- the app's side of Windows' screen readers.
//
// Win.apk: the host's window answers UI Automation (windows/host/uia_provider.h) from snapshots of
// the app's windows that this class sends. Nothing here runs until a screen reader asks the window
// for its views: then the host calls hostActivate(true), and the app's accessibility is on, as
// when TalkBack is turned on (AccessibilityManager.isEnabled(), the services' state, the events
// the views send). From then on every accessibility event the views send (a focus moved, a text,
// a window or a list changed) asks for a snapshot, sent at most every 100 ms (AOSP's
// ViewConfiguration.getSendRecurringAccessibilityEventsInterval, the rate content changes reach a
// screen reader on a phone). An announcement (TYPE_ANNOUNCEMENT) is spoken by the host directly.
// What a client asks the app to do (a click, a text, a scroll, a selection) comes back through
// hostPerform and runs as AccessibilityNodeInfo.performAction on the main thread.
//
// The tree walk itself is the view host's (ViewHost.accessibilitySnapshot; AOSP's views:
// com.winapk.aospviews.AccessibilityTree), so this class needs no member of android.view beyond
// the public API.
package com.macapk.loader;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.accessibility.AccessibilityEvent;

public final class HostAccessibility {
    private HostAccessibility() {}

    private static final String TAG = "HostAccessibility";
    /** AOSP's ViewConfiguration.SEND_RECURRING_ACCESSIBILITY_EVENTS_INTERVAL_MILLIS. */
    private static final long SNAPSHOT_INTERVAL_MS = 100;

    /** Where snapshots and announcements go: the host's window (the natives), or a test's. */
    public interface Sink {
        /** False: no screen reader reads the app any more. */
        boolean publish(String snapshot);

        void announce(String text, boolean assertive);
    }

    private static final Sink NATIVE = new Sink() {
        @Override public boolean publish(String snapshot) { return nativePublish(snapshot); }

        @Override public void announce(String text, boolean assertive) { nativeAnnounce(text, assertive); }
    };

    private static volatile Sink sSink;
    private static volatile boolean sActive;
    private static Handler sMain;
    // Main thread only.
    private static boolean sScheduled;
    private static long sLastSnapshot = -SNAPSHOT_INTERVAL_MS;

    /** Boot, on the main thread: the host may call in from now on. */
    public static void install() {
        install(NATIVE);
    }

    /** As install(), with a test's sink instead of the host's window. */
    public static synchronized void install(Sink sink) {
        if (sSink != null) return;
        sMain = new Handler(Looper.getMainLooper());
        if (sink == NATIVE) {
            try {
                nativeInstall();
            } catch (UnsatisfiedLinkError e) {
                NativeLog.w(TAG, "no host screen reader support: " + e);
                return;
            }
        }
        sSink = sink;
    }

    /** Whether a screen reader reads the app now. */
    public static boolean active() {
        return sActive;
    }

    // ---- The views' side ----------------------------------------------------------------------

    /** An event the views sent a screen reader (AOSP's views: their AccessibilityManager's
     *  service, com.winapk.aospviews.AccessibilityService). Any thread. */
    public static void onEvent(AccessibilityEvent event) {
        if (event == null || !sActive) return;
        if (event.getEventType() == AccessibilityEvent.TYPE_ANNOUNCEMENT) {
            speak(joined(event.getText()), false);
            return;
        }
        requestSnapshot();
    }

    /** As onEvent(AccessibilityEvent), from Win.apk's own AccessibilityManager. */
    public static void onEvent(int type, CharSequence text, boolean assertive) {
        if (!sActive) return;
        if (type == AccessibilityEvent.TYPE_ANNOUNCEMENT) {
            speak(text != null ? text.toString() : "", assertive);
            return;
        }
        requestSnapshot();
    }

    // ---- The host's side: called on the host's worker thread, each posted to the main thread ----

    /** A screen reader started (true) reading the app, or none has for a while. */
    public static void hostActivate(boolean on) {
        post(() -> setActive(on));
    }

    /** Send a snapshot soon, though no view said it changed (a frame came with no event). */
    public static void hostRefresh() {
        post(HostAccessibility::requestSnapshot);
    }

    /** AccessibilityNodeInfo.performAction on a node of the last snapshot. */
    public static void hostPerform(long nodeId, int action, String text, int arg1, int arg2, float value) {
        post(() -> {
            boolean done = false;
            try {
                done = com.macapk.loader.view.ViewHost.get().accessibilityPerform(nodeId, action, text, arg1, arg2, value);
            } catch (RuntimeException e) {
                NativeLog.w(TAG, "action " + action + " on " + nodeId + ": " + e);
            }
            if (!done) NativeLog.d(TAG, "action " + action + " on " + nodeId + " not done");
            requestSnapshot();  // what it changed, at once rather than at the next event
        });
    }

    // ---- The main thread -------------------------------------------------------------------------

    private static void post(Runnable r) {
        Handler h = sMain;
        if (h != null) h.post(r);
    }

    private static void setActive(boolean on) {
        if (sActive == on) return;
        sActive = on;
        NativeLog.i(TAG, on ? "a screen reader reads the app" : "no screen reader reads the app any more");
        com.macapk.loader.view.ViewHost.get().accessibilityEnabled(on);
        if (on) requestSnapshot();
    }

    /** At most one snapshot every SNAPSHOT_INTERVAL_MS, whatever the number of events. */
    private static void requestSnapshot() {
        Handler h = sMain;
        if (h == null) return;
        if (Looper.myLooper() != h.getLooper()) {
            h.post(HostAccessibility::requestSnapshot);
            return;
        }
        if (!sActive || sScheduled) return;
        sScheduled = true;
        long wait = Math.max(0, sLastSnapshot + SNAPSHOT_INTERVAL_MS - SystemClock.uptimeMillis());
        h.postDelayed(HostAccessibility::sendSnapshot, wait);
    }

    private static void sendSnapshot() {
        sScheduled = false;
        Sink sink = sSink;
        if (!sActive || sink == null) return;
        sLastSnapshot = SystemClock.uptimeMillis();
        String snapshot;
        try {
            snapshot = com.macapk.loader.view.ViewHost.get().accessibilitySnapshot();
        } catch (RuntimeException e) {
            NativeLog.w(TAG, "snapshot: " + e);
            return;
        }
        if (snapshot != null && !sink.publish(snapshot)) setActive(false);
    }

    private static void speak(String text, boolean assertive) {
        Sink sink = sSink;
        if (sink != null && text != null && !text.isEmpty()) sink.announce(text, assertive);
    }

    private static String joined(java.util.List<CharSequence> parts) {
        StringBuilder out = new StringBuilder();
        for (CharSequence t : parts) {
            if (t == null || t.length() == 0) continue;
            if (out.length() > 0) out.append(' ');
            out.append(t);
        }
        return out.toString();
    }

    private static native void nativeInstall();
    private static native boolean nativePublish(String snapshot);
    private static native void nativeAnnounce(String text, boolean assertive);
}
