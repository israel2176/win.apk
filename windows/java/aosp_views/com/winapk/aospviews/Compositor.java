// The in-process window manager's compositor (docs/research/aosp-views.md, "V3"): what
// SurfaceFlinger does with the windows' layers, on the GPU. Each window's SurfaceControl has a
// layer in the graphics DLL (HwuiLayers), which the window's ThreadedRenderer draws into through
// HWUI (ViewRootImpl's Surface, copied from the SurfaceControl, carries the layer post the DLL finds
// the layer by); a window drawn in software (hardwareAccelerated="false") posts its bitmap to the
// layer instead. This class says which layers are shown, bottom to top, where in the display, and
// the dim layer under a FLAG_DIM_BEHIND window (black at the window's dimAmount over the whole
// display), as WindowManagerService's surface placement does; the DLL's compositor draws them into
// the host window on HWUI's render thread whenever a window posts a frame or the scene changes.
// Main thread only, as every ViewRootImpl lays out there.
//
// Order: the activity's main window (TYPE_BASE_APPLICATION, which its WindowToken keeps at the
// bottom of its windows), then the other application windows and sub-windows (dialogs, panels,
// popups) in the order they were added, then the system types (toasts, overlays) in theirs.
// WindowManagerService orders by DisplayPolicy's layer of the type and then by the tokens'
// z-order; with one task and no system windows of its own, that is this order.
package com.winapk.aospviews;

import android.view.WindowManager;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

final class Compositor {
    private Compositor() {}

    private static java.lang.reflect.Method sSetLayerPost;
    private static Class<?> sLayerPost;

    /** The window's SurfaceControl gets a layer: the DLL's, of the control's size, and the layer post
     *  its Surface takes (Win.apk's SurfaceControl.macapkSetLayerPost; Win.apk-only members, which
     *  this jar is compiled without, so through reflection) for a software frame. */
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
                        post(w, (android.graphics.Bitmap) args[1]);
                        return null;
                    });
            w.post = post;
            w.layer = HwuiLayers.create(post, control.getWidth(), control.getHeight());
            w.drew = false;
            sSetLayerPost.invoke(control, post);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("Win.apk's SurfaceControl.macapkSetLayerPost", e);
        }
    }

    /** The window's SurfaceControl goes: its layer goes with the last reference to it (HWUI may still
     *  hold the window's ANativeWindow until its next frame). */
    static void detach(AospWindows.Win w) {
        if (w.post != null) HwuiLayers.release(w.post);
        w.post = null;
        w.layer = 0;
        w.drew = false;
    }

    /** WINAPK_AOSP_VIEWS_TRACE=1: each relayout, each window's first frame and each new window order
     *  in the log. */
    static final boolean TRACE = "1".equals(System.getenv("WINAPK_AOSP_VIEWS_TRACE"));
    private static String sLastOrder;

    /** A window drawn in software posted a frame (its Surface's bitmap), on the drawing thread. */
    static void post(AospWindows.Win w, android.graphics.Bitmap frame) {
        if (TRACE && !w.drew) {
            System.err.println(AospWindows.TAG + "first software frame of " + w.attrs.getTitle() + " type="
                    + w.attrs.type + " frame=" + w.frame + " surface=" + frame.getWidth() + "x" + frame.getHeight());
        }
        long layer = w.layer;
        if (layer == 0) return;
        HwuiLayers.postBitmap(layer, frame);
        if (!w.drew) {
            w.drew = true;
            compose();
        }
    }

    static int band(int type) {
        if (type >= WindowManager.LayoutParams.FIRST_SYSTEM_WINDOW) return 2;
        // An activity token's main window is the bottom of its windows (WindowToken keeps the
        // TYPE_BASE_APPLICATION window first), whenever its dialogs were added.
        return type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION ? 0 : 1;
    }

    /** The scene changed (a window shown, hidden, moved, added or removed, its dim): the DLL composes
     *  with it at once, and at every frame a window posts. */
    static void compose() {
        List<AospWindows.Win> order = new ArrayList<>();
        for (AospWindows.Win w : AospWindows.sWindows.values()) {
            if (w.visible && w.layer != 0 && w.attrs != null) order.add(w);
        }
        InputRouter.windowsChanged();  // the focus follows the windows shown (step V2)
        order.sort(Comparator.comparingInt((AospWindows.Win w) -> band(w.attrs.type))
                .thenComparingLong(w -> w.added));
        if (TRACE) {
            StringBuilder sb = new StringBuilder();
            for (AospWindows.Win w : order) sb.append(w.attrs.type).append('@').append(w.frame).append(' ');
            String now = sb.toString();
            if (!now.equals(sLastOrder)) {
                sLastOrder = now;
                System.err.println(AospWindows.TAG + "compose " + now);
            }
        }
        long[] layers = new long[order.size()];
        int[] xy = new int[2 * order.size()];
        int[] dim = new int[order.size()];
        for (int i = 0; i < layers.length; i++) {
            AospWindows.Win w = order.get(i);
            WindowManager.LayoutParams a = w.attrs;
            layers[i] = w.layer;
            // Each at its frame less its surface insets (a floating window's room for its shadow).
            xy[2 * i] = w.frame.left - a.surfaceInsets.left;
            xy[2 * i + 1] = w.frame.top - a.surfaceInsets.top;
            if ((a.flags & WindowManager.LayoutParams.FLAG_DIM_BEHIND) != 0 && a.dimAmount > 0f) {
                dim[i] = Math.round(Math.min(1f, a.dimAmount) * 255f);
            }
        }
        // Nothing under the windows is black, as SurfaceFlinger's empty display; but while a
        // SurfaceView's GL view is under them (AospWindows.showSurfaceBelow) the display starts
        // clear, so the host's layer shows through the SurfaceView's hole.
        HwuiLayers.setScene(layers, xy, dim, AospWindows.widthPx(), AospWindows.heightPx(),
                AospWindows.surfaceBelow());
    }
}
