// The windows' layers in the graphics DLL (aosp-hwui.dll, windows/hwui_build/glue/layers.cpp): what
// SurfaceFlinger holds for each window's SurfaceControl. ViewRootImpl's ThreadedRenderer draws a
// window through HWUI into its layer (the layer is the ANativeWindow behind the window's Surface:
// the DLL's ANativeWindow_fromSurface finds it by the Surface's layer post, windowOf); a window
// that draws in software posts its bitmap here instead. The compositor, on HWUI's render thread,
// draws the layers on the GPU into the host window, in the order and at the places setScene gave.
package com.winapk.aospviews;

import java.util.Map;
import java.util.WeakHashMap;

final class HwuiLayers {
    private HwuiLayers() {}

    /** The layer of each layer post (Surface$MacapkLayerPost, one per SurfaceControl). */
    private static final Map<Object, Long> sByPost = new WeakHashMap<>();

    static synchronized long create(Object post, int width, int height) {
        long layer = nCreate(width, height);
        sByPost.put(post, layer);
        return layer;
    }

    static synchronized void release(Object post) {
        Long layer = sByPost.remove(post);
        if (layer != null) nRelease(layer);
    }

    /** The DLL's: the layer a Surface draws into, by its layer post; 0 for none. Any thread. */
    static synchronized long windowOf(Object post) {
        Long layer = sByPost.get(post);
        return layer != null ? layer : 0;
    }

    /** A software frame of a layer (Surface.unlockCanvasAndPost of a window drawn in software). */
    static void postBitmap(long layer, android.graphics.Bitmap frame) {
        nPostBitmap(layer, frame.getNativeInstance());
    }

    /** The layers bottom to top, each at (x, y) in the display with the dim (0..255) before it. */
    static void setScene(long[] layers, int[] xy, int[] dim, int displayWidth, int displayHeight, boolean clearBelow) {
        nSetScene(layers, xy, dim, displayWidth, displayHeight, clearBelow);
    }

    private static native long nCreate(int width, int height);
    private static native void nResize(long layer, int width, int height);
    private static native void nRelease(long layer);
    private static native void nPostBitmap(long layer, long bitmap);
    private static native void nSetScene(long[] layers, int[] xy, int[] dim, int displayWidth, int displayHeight,
                                         boolean clearBelow);
}
