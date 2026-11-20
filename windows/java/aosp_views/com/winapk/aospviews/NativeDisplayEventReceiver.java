// The natives of AOSP's android.view.DisplayEventReceiver (android_view_DisplayEventReceiver.cpp on
// Android: SurfaceFlinger's vsync through a Looper fd), on a Java frame clock. Choreographer's
// FrameDisplayEventReceiver asks nativeScheduleVsync for the next frame; the clock's thread then
// posts dispatchVsync to the receiver's own looper as an asynchronous message, as the native
// receiver's handleEvent does, so it passes ViewRootImpl's traversal barrier. One tick per display
// frame (NativeCocoa.displayHz, else 60), with the frame's VsyncEventData filled as SurfaceFlinger's
// single timeline: id = frame number, deadline and expected presentation one frame later.
// AospViewsRewrite gives DisplayEventReceiver's natives these bodies.
package com.winapk.aospviews;

import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.os.MessageQueue;
import android.view.DisplayEventReceiver;

import java.lang.ref.WeakReference;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

public final class NativeDisplayEventReceiver {
    private NativeDisplayEventReceiver() {}

    private static final class Receiver {
        final WeakReference<DisplayEventReceiver> receiver;
        final WeakReference<DisplayEventReceiver.VsyncEventData> data;
        final Handler handler;
        volatile boolean scheduled;

        Receiver(WeakReference<DisplayEventReceiver> receiver, WeakReference<DisplayEventReceiver.VsyncEventData> data,
                 Looper looper) {
            this.receiver = receiver;
            this.data = data;
            this.handler = new Handler(looper);
        }
    }

    private static final ConcurrentHashMap<Long, Receiver> sReceivers = new ConcurrentHashMap<>();
    private static final AtomicLong sNext = new AtomicLong(1);
    private static final Object sClockLock = new Object();
    private static volatile long sFrame;
    private static volatile long sLastVsyncNanos;
    private static Thread sClock;
    private static Method sDispatchVsync;

    static long frameIntervalNanos() {
        int hz = 60;
        try {
            int h = com.macapk.loader.NativeCocoa.displayHz();
            if (h >= 24 && h <= 480) hz = h;
        } catch (Throwable ignored) {
            // No host (a plain JVM): 60 Hz.
        }
        return 1_000_000_000L / hz;
    }

    public static long nativeInit(WeakReference<DisplayEventReceiver> receiver,
            WeakReference<DisplayEventReceiver.VsyncEventData> vsyncEventData, MessageQueue messageQueue,
            int vsyncSource, int eventRegistration, long layerHandle) {
        // DisplayEventReceiver(Looper) took the queue from the looper it is built for, on that
        // looper's thread (Choreographer is per thread); Win.apk's MessageQueue does not name its
        // looper, so the receiver's looper is this thread's.
        Looper looper = Looper.myLooper();
        if (looper == null) looper = Looper.getMainLooper();
        long id = sNext.getAndIncrement();
        sReceivers.put(id, new Receiver(receiver, vsyncEventData, looper));
        startClock();
        return id;
    }

    /** NativeAllocationRegistry's free function: none (a 0 one is skipped); nativeDispose is not native. */
    public static long nativeGetDisplayEventReceiverFinalizer() { return 0L; }

    public static void nativeScheduleVsync(long receiverPtr) {
        Receiver r = sReceivers.get(receiverPtr);
        if (r == null) return;
        r.scheduled = true;
        synchronized (sClockLock) { sClockLock.notifyAll(); }
    }

    public static DisplayEventReceiver.VsyncEventData nativeGetLatestVsyncEventData(long receiverPtr) {
        Receiver r = sReceivers.get(receiverPtr);
        DisplayEventReceiver.VsyncEventData d = r != null ? r.data.get() : null;
        if (d != null) fill(d, sLastVsyncNanos != 0 ? sLastVsyncNanos : System.nanoTime(), sFrame);
        return d;
    }

    /** DisplayEventReceiver.dispose() drops the receiver (its pointer is cleared before). */
    static void forget(long receiverPtr) { sReceivers.remove(receiverPtr); }

    private static void fill(DisplayEventReceiver.VsyncEventData d, long vsyncNanos, long frame) {
        long interval = frameIntervalNanos();
        d.frameInterval = interval;
        d.preferredFrameTimelineIndex = 0;
        d.frameTimelinesLength = 1;
        DisplayEventReceiver.VsyncEventData.FrameTimeline t = d.frameTimelines[0];
        t.vsyncId = frame;
        t.expectedPresentationTime = vsyncNanos + interval;
        t.deadline = vsyncNanos + interval;
    }

    private static synchronized void startClock() {
        if (sClock != null) return;
        sClock = new Thread(NativeDisplayEventReceiver::run, "winapk-vsync");
        sClock.setDaemon(true);
        sClock.start();
    }

    private static void run() {
        final long interval = frameIntervalNanos();
        long next = System.nanoTime();
        for (;;) {
            // Sleep while nobody asked for a frame, then tick on the frame grid.
            synchronized (sClockLock) {
                while (!anyScheduled()) {
                    try { sClockLock.wait(); } catch (InterruptedException e) { return; }
                }
            }
            long now = System.nanoTime();
            if (next < now - interval) next = now - (now - next) % interval;  // idle: back onto the grid
            while (now < next) {
                long ms = (next - now) / 1_000_000L;
                try {
                    if (ms > 0) Thread.sleep(ms, (int) ((next - now) % 1_000_000L));
                    else Thread.sleep(0, (int) (next - now));
                } catch (InterruptedException e) {
                    return;
                }
                now = System.nanoTime();
            }
            final long vsync = next;
            next += interval;
            final long frame = ++sFrame;
            sLastVsyncNanos = vsync;
            List<Receiver> due = new ArrayList<>();
            for (Receiver r : sReceivers.values()) {
                if (r.scheduled) {
                    r.scheduled = false;
                    due.add(r);
                }
            }
            for (Receiver r : due) {
                Message m = Message.obtain(r.handler, () -> dispatch(r, vsync, frame));
                m.setAsynchronous(true);
                r.handler.sendMessage(m);
            }
        }
    }

    private static boolean anyScheduled() {
        for (Receiver r : sReceivers.values()) if (r.scheduled) return true;
        return false;
    }

    private static void dispatch(Receiver r, long vsync, long frame) {
        DisplayEventReceiver receiver = r.receiver.get();
        if (receiver == null) return;
        DisplayEventReceiver.VsyncEventData d = r.data.get();
        if (d != null) fill(d, vsync, frame);
        try {
            Method m = sDispatchVsync;
            if (m == null) {
                // "Called from native code": private, as the native receiver calls it through JNI.
                m = DisplayEventReceiver.class.getDeclaredMethod("dispatchVsync", long.class, long.class, int.class);
                m.setAccessible(true);
                sDispatchVsync = m;
            }
            m.invoke(receiver, vsync, 0L, (int) frame);
        } catch (java.lang.reflect.InvocationTargetException e) {
            Throwable t = e.getCause();
            if (t instanceof RuntimeException re) throw re;
            if (t instanceof Error er) throw er;
            throw new RuntimeException(t);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("DisplayEventReceiver.dispatchVsync", e);
        }
    }
}
