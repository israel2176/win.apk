// FrameTimes -- how long each frame of the view tree took (Win.apk: windows/host/frame_times.h,
// windows/README.md "Frame time").
//
// ViewCanvasDriver hands every frame that reached the present here: the time its view tree took
// to lay out and to draw, reading the frame's pixels out of its bitmap, the present (the host
// copying them to the window), and the whole frame. The host keeps them in histograms and, when the process ends,
// logs one `[win.apk] frame times` line with the median, p90, p99 and max of each, so every run
// shows whether drawing got faster or slower, on either graphics engine.
//
// A host without the native (the Mac binds none) throws UnsatisfiedLinkError once; the record
// is then off for the rest of the process.
package com.macapk.loader;

public final class FrameTimes {
    private FrameTimes() {}

    private static volatile boolean sUnavailable;

    /** One frame, in nanoseconds. warmup: one of the first frames of its window (class loading
     *  and the JIT's first passes), which the summary leaves out when later frames exist. */
    static void record(long layoutNs, long drawNs, long pixelsNs, long presentNs, long totalNs,
                       boolean warmup) {
        if (sUnavailable) return;
        try {
            nativeRecord(layoutNs, drawNs, pixelsNs, presentNs, totalNs, warmup);
        } catch (UnsatisfiedLinkError e) {
            sUnavailable = true;
        }
    }

    private static native void nativeRecord(long layoutNs, long drawNs, long pixelsNs, long presentNs,
                                            long totalNs, boolean warmup);
}
