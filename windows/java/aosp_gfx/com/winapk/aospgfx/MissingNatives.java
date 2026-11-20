package com.winapk.aospgfx;

import android.graphics.BaseCanvas;
import android.graphics.BlendMode;
import android.graphics.Color;
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
