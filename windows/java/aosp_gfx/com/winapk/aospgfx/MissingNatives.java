package com.winapk.aospgfx;

import android.graphics.BaseCanvas;
import android.graphics.BlendMode;
import android.graphics.Color;
import android.graphics.Gainmap;
import android.graphics.Paint;

import java.lang.reflect.Method;

/**
 * Natives of AOSP 14's classes that Robolectric's native runtime does not have, in Java: build_aosp_gfx.sh
 * points the AOSP class's calls to each here (scripts/AospCallRedirect.java), same name and descriptor.
 * Without them the call threw UnsatisfiedLinkError.
 */
public final class MissingNatives {
    private MissingNatives() {}

    /**
     * Typeface's static initializer registers the locale list of every family of the device's
     * fonts.xml that has one: hwui's Typeface_registerLocaleList only puts the list in minikin's cache
     * ahead of its first use (minikin::registerLocaleList), which the native runtime then does on that
     * use. Without this, Typeface could not initialize once /system/etc/fonts.xml
     * is the runtime's (AospGfx.materializeSystemFonts).
     */
    public static void nativeRegisterLocaleList(String locales) {}

    /**
     * Bitmap's HDR gain map (API 34): the library predates it, so Bitmap.hasGainmap threw, and with it
     * Bitmap.createBitmap(source, x, y, width, height, matrix, filter) and createScaledBitmap, which ask
     * the source for one. No bitmap here has one (none of the decoders makes one).
     */
    public static boolean nativeHasGainmap(long bitmap) { return false; }

    /** Bitmap.getGainmap: none, as {@link #nativeHasGainmap} says. */
    public static Gainmap nativeExtractGainmap(long bitmap) { return null; }

    /**
     * Bitmap.setGainmap: BitmapFactory clears the gain map of every Options.inBitmap before decoding into
     * it (Options.nativeInBitmap), so decoding into a reused bitmap (Glide's pool) threw. There is none
     * to clear, nor any to set (a Gainmap needs the library's natives too).
     */
    public static void nativeSetGainmap(long bitmap, long gainmap) {}

    private static Method drawRoundRect;
    private static final ThreadLocal<Paint> PAINT = ThreadLocal.withInitial(() -> {
        Paint p = new Paint();
        p.setColor(Color.BLACK);
        p.setBlendMode(BlendMode.DST_OUT);
        return p;
    });

    /**
     * Canvas.punchHole: SurfaceView draws one where its surface shows (SDL's, a video's), and AOSP's
     * BaseCanvas and BaseRecordingCanvas call nPunchHole, which threw and cost the view its draw. This
     * draws what hwui's SkiaCanvas::punchHole does (libs/hwui/SkiaCanvas.cpp): the round rect in black
     * at the alpha, DST_OUT, so what was drawn there is cleared by that much, with the native
     * drawRoundRect both canvases share.
     */
    public static void nPunchHole(long canvas, float left, float top, float right, float bottom, float rx, float ry,
            float alpha) {
        if (!(alpha > 0f) || right <= left || bottom <= top) return;
        try {
            if (drawRoundRect == null) {
                Method m = BaseCanvas.class.getDeclaredMethod("nDrawRoundRect", long.class, float.class,
                        float.class, float.class, float.class, float.class, float.class, long.class);
                m.setAccessible(true);
                drawRoundRect = m;
            }
            Paint p = PAINT.get();
            p.setAlpha(Math.round(Math.min(alpha, 1f) * 255f));
            drawRoundRect.invoke(null, canvas, left, top, right, bottom, rx, ry, p.getNativeInstance());
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("Canvas.punchHole", e);
        }
    }
}
