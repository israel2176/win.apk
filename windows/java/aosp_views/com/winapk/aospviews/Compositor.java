// The in-process window manager's compositor (docs/research/aosp-views.md, "V1 as built"): what
// SurfaceFlinger does with the windows' layers, in software, into the one frame the host window
// presents. Each window's Surface (Win.apk's, copied by ViewRootImpl from the SurfaceControl the
// session's relayout made) hands its posted bitmap here (SurfaceControl.macapkSetLayerPost); the
// frame is then the windows bottom to top, each at its frame less its surface insets, with
// WindowManagerService's dim layer (black at the window's dimAmount, over the whole display)
// under a FLAG_DIM_BEHIND window, and the bars' glyphs (Bars) on top. On the main thread only, as
// every ViewRootImpl draws there.
//
// Order: the activity's main window (TYPE_BASE_APPLICATION, which its WindowToken keeps at the
// bottom of its windows), then the other application windows and sub-windows (dialogs, panels,
// popups) in the order they were added, then the system types (toasts, overlays) in theirs.
// WindowManagerService orders by DisplayPolicy's layer of the type and then by the tokens'
// z-order; with one task and no system windows of its own, that is this order.
package com.winapk.aospviews;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.PorterDuff;
import android.view.WindowManager;

import com.macapk.loader.NativeCocoa;
import com.macapk.loader.gfx.GfxBackend;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

final class Compositor {
    private Compositor() {}

    private static Bitmap sFrame;
    private static Canvas sCanvas;

    private static java.lang.reflect.Method sSetLayerPost;
    private static Class<?> sLayerPost;

    /** The window's layer hands its frames here: Win.apk's SurfaceControl.macapkSetLayerPost
     *  (Win.apk-only members, which this jar is compiled without, so through reflection). */
    static void attach(android.view.SurfaceControl control, AospWindows.Win w) {
        try {
            if (sSetLayerPost == null) {
                sLayerPost = Class.forName("android.view.Surface$MacapkLayerPost");
                sSetLayerPost = android.view.SurfaceControl.class.getMethod("macapkSetLayerPost", sLayerPost);
            }
            Object post = java.lang.reflect.Proxy.newProxyInstance(sLayerPost.getClassLoader(),
                    new Class<?>[] {sLayerPost}, (proxy, m, args) -> {
                        if (m.getDeclaringClass() == Object.class) {
                            return switch (m.getName()) {
                                case "equals" -> proxy == args[0];
                                case "hashCode" -> System.identityHashCode(proxy);
                                default -> "layer of " + w.attrs.getTitle();
                            };
                        }
                        post(w, (Bitmap) args[1]);
                        return null;
                    });
            sSetLayerPost.invoke(control, post);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("Win.apk's SurfaceControl.macapkSetLayerPost", e);
        }
    }

    /** A window's Surface posted a frame. */
    private static String sLastOrder;
    private static boolean sDumpNext;
    /** WINAPK_AOSP_VIEWS_TRACE=1: each relayout, each window's first frame and each new window
     *  order in the log; with WINAPK_AOSP_VIEWS_TRACE_DIR also those frames as PNGs there. */
    static final boolean TRACE = "1".equals(System.getenv("WINAPK_AOSP_VIEWS_TRACE"));

    static void post(AospWindows.Win w, Bitmap frame) {
        if (TRACE && w.content == null) {
            System.err.println(AospWindows.TAG + "first frame of " + w.attrs.getTitle() + " type=" + w.attrs.type
                    + " frame=" + w.frame + " surface=" + frame.getWidth() + "x" + frame.getHeight()
                    + " flags=0x" + Integer.toHexString(w.attrs.flags) + " dim=" + w.attrs.dimAmount
                    + " insets=" + w.attrs.surfaceInsets);
            String dir = System.getenv("WINAPK_AOSP_VIEWS_TRACE_DIR");
            if (dir != null) {
                try (java.io.FileOutputStream out = new java.io.FileOutputStream(
                        new java.io.File(dir, "layer-" + w.added + ".png"))) {
                    frame.compress(Bitmap.CompressFormat.PNG, 100, out);
                } catch (Exception e) {
                    System.err.println(AospWindows.TAG + "layer dump: " + e);
                }
            }
        }
        w.content = frame;
        compose();
    }

    static int band(int type) {
        if (type >= WindowManager.LayoutParams.FIRST_SYSTEM_WINDOW) return 2;
        // An activity token's main window is the bottom of its windows (WindowToken keeps the
        // TYPE_BASE_APPLICATION window first), whenever its dialogs were added.
        return type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION ? 0 : 1;
    }

    static void compose() {
        List<AospWindows.Win> order = new ArrayList<>();
        for (AospWindows.Win w : AospWindows.sWindows.values()) {
            if (w.visible && w.content != null && w.attrs != null) order.add(w);
        }
        InputRouter.windowsChanged();  // the focus follows the windows shown (step V2)
        if (order.isEmpty()) return;
        order.sort(Comparator.comparingInt((AospWindows.Win w) -> band(w.attrs.type))
                .thenComparingLong(w -> w.added));
        if (TRACE) {
            StringBuilder sb = new StringBuilder();
            for (AospWindows.Win w : order) sb.append(w.attrs.type).append('@').append(w.frame).append(' ');
            String now = sb.toString();
            if (!now.equals(sLastOrder)) {
                sLastOrder = now;
                sDumpNext = true;
                System.err.println(AospWindows.TAG + "compose " + now);
            }
        }
        int width = AospWindows.widthPx(), height = AospWindows.heightPx();
        if (order.size() == 1 && !Bars.paintsAny()) {
            AospWindows.Win w = order.get(0);
            Bitmap b = w.content;
            if (w.frame.left == 0 && w.frame.top == 0 && b.getWidth() == width && b.getHeight() == height
                    && w.attrs.surfaceInsets.left == 0 && w.attrs.surfaceInsets.top == 0) {
                present(b);  // one window filling the display: its own frame, as before
                return;
            }
        }
        if (sFrame == null || sFrame.getWidth() != width || sFrame.getHeight() != height) {
            sFrame = GfxBackend.get().createFrameBitmap(width, height);
            sCanvas = new Canvas(sFrame);
        }
        Canvas c = sCanvas;
        c.drawColor(Color.BLACK, PorterDuff.Mode.SRC);
        for (AospWindows.Win w : order) {
            WindowManager.LayoutParams a = w.attrs;
            if ((a.flags & WindowManager.LayoutParams.FLAG_DIM_BEHIND) != 0 && a.dimAmount > 0f) {
                int alpha = Math.round(Math.min(1f, a.dimAmount) * 255f);
                c.drawColor(Color.argb(alpha, 0, 0, 0));
            }
            c.drawBitmap(w.content, w.frame.left - a.surfaceInsets.left, w.frame.top - a.surfaceInsets.top, null);
        }
        Bars.paintGlyphs(c, AospWindows.metrics().density);
        if (TRACE && sDumpNext) {
            sDumpNext = false;
            String dir = System.getenv("WINAPK_AOSP_VIEWS_TRACE_DIR");
            if (dir != null) {
                try (java.io.FileOutputStream out = new java.io.FileOutputStream(new java.io.File(dir, "frame.png"))) {
                    sFrame.compress(Bitmap.CompressFormat.PNG, 100, out);
                } catch (Exception e) {
                    System.err.println(AospWindows.TAG + "frame dump: " + e);
                }
            }
        }
        present(sFrame);
    }

    /** The frame to the host window, as Win.apk's Surface presents one. */
    private static void present(Bitmap b) {
        int[] pixels = GfxBackend.get().liveArgbOrNull(b);
        if (pixels == null) pixels = GfxBackend.get().argbBuffer(b);
        if (pixels == null) return;
        try {
            NativeCocoa.presentBitmap(pixels, b.getWidth(), b.getHeight());
        } catch (UnsatisfiedLinkError e) {
            // A plain JVM (no host): nothing to show it on.
        }
    }
}
