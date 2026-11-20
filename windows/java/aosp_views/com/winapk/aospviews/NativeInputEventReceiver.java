// The natives of AOSP's android.view.InputEventReceiver (android_view_InputEventReceiver.cpp on
// Android: events read off the window's InputChannel, handed to dispatchInputEvent on the
// receiver's looper). Here the "channel" is the InputChannel object the window session handed out
// at addToDisplay: each receiver is recorded by its channel, and InputRouter delivers the host's
// events to the window's receiver on the main looper, as the native side does with an event it
// read (docs/research/aosp-views.md, step V2). Every event is delivered at once, so nothing is
// batched and nothing is left to consume; finishing an event has nothing to tell a dispatcher.
package com.winapk.aospviews;

import android.os.MessageQueue;
import android.view.InputChannel;
import android.view.InputEvent;
import android.view.InputEventReceiver;

import java.lang.ref.WeakReference;
import java.lang.reflect.Method;
import java.util.Map;
import java.util.WeakHashMap;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

public final class NativeInputEventReceiver {
    private NativeInputEventReceiver() {}

    private static final ConcurrentHashMap<Long, WeakReference<InputEventReceiver>> sReceivers =
            new ConcurrentHashMap<>();
    private static final Map<InputChannel, WeakReference<InputEventReceiver>> sByChannel =
            java.util.Collections.synchronizedMap(new WeakHashMap<>());
    private static final AtomicLong sNext = new AtomicLong(1);
    private static final AtomicInteger sSeq = new AtomicInteger(1);
    private static volatile Method sDispatch;

    public static long nativeInit(WeakReference<InputEventReceiver> receiver, InputChannel channel,
            MessageQueue queue) {
        long id = sNext.getAndIncrement();
        sReceivers.put(id, receiver);
        if (channel != null) sByChannel.put(channel, receiver);
        return id;
    }

    public static void nativeDispose(long receiverPtr) { sReceivers.remove(receiverPtr); }

    public static void nativeFinishInputEvent(long receiverPtr, int seq, boolean handled) {}

    public static void nativeReportTimeline(long receiverPtr, int inputEventId, long gpuCompletedTime,
            long presentTime) {}

    public static boolean nativeConsumeBatchedInputEvents(long receiverPtr, long frameTimeNanos) { return false; }

    public static String nativeDump(long receiverPtr, String prefix) { return prefix + "(Win.apk: in-process channel)"; }

    /** The receiver reading the window's channel, or null (none made yet, or disposed). */
    static InputEventReceiver receiverFor(InputChannel channel) {
        if (channel == null) return null;
        WeakReference<InputEventReceiver> r = sByChannel.get(channel);
        return r != null ? r.get() : null;
    }

    /** The native side's call: the receiver's dispatchInputEvent(seq, event), on its looper. */
    static void dispatch(InputEventReceiver receiver, InputEvent event) {
        try {
            Method m = sDispatch;
            if (m == null) {
                m = InputEventReceiver.class.getDeclaredMethod("dispatchInputEvent", int.class, InputEvent.class);
                m.setAccessible(true);
                sDispatch = m;
            }
            m.invoke(receiver, sSeq.getAndIncrement(), event);
        } catch (java.lang.reflect.InvocationTargetException e) {
            Throwable t = e.getCause();
            if (t instanceof RuntimeException re) throw re;
            if (t instanceof Error er) throw er;
            throw new RuntimeException(t);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("InputEventReceiver.dispatchInputEvent", e);
        }
    }
}
