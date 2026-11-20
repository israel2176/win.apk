// The natives of AOSP's android.view.InputEventReceiver (android_view_InputEventReceiver.cpp on
// Android: events read off the window's InputChannel). ViewRootImpl.setView builds its
// WindowInputEventReceiver on the channel the window session handed out; here the receiver is only
// recorded, so that the host's input can later be delivered through its dispatchInputEvent (step V2
// of docs/research/aosp-views.md). Nothing is batched, so nothing is ever left to consume.
package com.winapk.aospviews;

import android.os.MessageQueue;
import android.view.InputChannel;
import android.view.InputEventReceiver;

import java.lang.ref.WeakReference;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

public final class NativeInputEventReceiver {
    private NativeInputEventReceiver() {}

    private static final ConcurrentHashMap<Long, WeakReference<InputEventReceiver>> sReceivers =
            new ConcurrentHashMap<>();
    private static final AtomicLong sNext = new AtomicLong(1);

    public static long nativeInit(WeakReference<InputEventReceiver> receiver, InputChannel channel,
            MessageQueue queue) {
        long id = sNext.getAndIncrement();
        sReceivers.put(id, receiver);
        return id;
    }

    public static void nativeDispose(long receiverPtr) { sReceivers.remove(receiverPtr); }

    public static void nativeFinishInputEvent(long receiverPtr, int seq, boolean handled) {}

    public static void nativeReportTimeline(long receiverPtr, int inputEventId, long gpuCompletedTime,
            long presentTime) {}

    public static boolean nativeConsumeBatchedInputEvents(long receiverPtr, long frameTimeNanos) { return false; }

    public static String nativeDump(long receiverPtr, String prefix) { return prefix + "(Win.apk: no channel)"; }
}
