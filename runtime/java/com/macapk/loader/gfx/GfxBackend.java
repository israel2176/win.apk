// GfxBackend -- the one door from the rest of the framework into Win.apk's own
// android.graphics / android.text plumbing (windows/README.md, "Milestone 2: the
// runtime on AOSP graphics", step 1).
//
// Win.apk's android.graphics and android.text declare macapk* methods and
// MacApk* classes that are not Android API: the frame's pixels, the fonts, the
// view's clip and transform, text helpers, SurfaceTexture's producers, drawable
// setters, debug dumps. Code outside those two packages calls them only through
// this interface, so that drawing with AOSP's own classes later needs an
// implementation of it (adapters), not patched copies of AOSP classes. Today the
// only implementation is Java2DGfx, which calls the methods it stands for, one
// for one: going through here changes nothing.
//
// Inside android.graphics / android.text the classes keep calling each other
// directly; this is only for the calls that cross out of them.
package com.macapk.loader.gfx;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.SurfaceTexture;
import android.graphics.Typeface;
import android.graphics.drawable.AnimatedImageDrawable;
import android.graphics.drawable.AnimatedVectorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.NinePatchDrawable;
import android.graphics.drawable.ShapeDrawable;
import android.graphics.drawable.VectorDrawable;
import android.text.Spanned;
import android.text.TextPaint;
import android.text.style.MetricAffectingSpan;
import android.text.style.ReplacementSpan;

import java.io.InputStream;
import java.util.List;
import java.util.Locale;
import java.util.function.Consumer;
import java.util.function.Function;

public interface GfxBackend {

    /** The backend of this JVM: chosen once, for the whole run. */
    static GfxBackend get() { return Java2DGfx.INSTANCE; }

    // ---- Present a frame: the pixels of a Bitmap, the draw-op counter ----

    /** The bitmap's ARGB pixels, a copy brought up to date (Bitmap.macapkArgbBuffer). */
    int[] argbBuffer(Bitmap bitmap);

    /** The bitmap's live ARGB backing when it has one, else null (Bitmap.macapkLiveArgbOrNull). */
    int[] liveArgbOrNull(Bitmap bitmap);

    /** The bitmap as an AWT image (Bitmap.macapkAwtImage). */
    java.awt.image.BufferedImage awtImage(Bitmap bitmap);

    /** How many draw ops the canvas has run (Canvas.macapkDrawOps): a frame drew ink when it moved. */
    int drawOps(Canvas canvas);

    // ---- Fonts and Typeface creation ----

    /** Publishes /system/fonts from the host's fonts; the directory, or null (MacApkSystemFonts.materialize). */
    String materializeSystemFonts(Function<String, String> hostFontFile);

    /** A Typeface.Builder over a font file's bytes (Typeface.Builder.macapkFromStream). */
    Typeface.Builder typefaceBuilderFromStream(InputStream in);

    /** The Typeface of a family registerAppFamily returned (Typeface.macapkFromAppFamily). */
    Typeface typefaceFromAppFamily(String registeredFamilyName);

    /** One face of a font file, with the weight and slant of its OS/2 table. */
    final class FontFace {
        /** The backend's face, opaque to the caller (handed back in registerAppFamily's list). */
        public final Object font;
        public final int weight;
        public final boolean italic;

        public FontFace(Object font, int weight, boolean italic) {
            this.font = font;
            this.weight = weight;
            this.italic = italic;
        }
    }

    /** A font file's face, or null when it does not parse (MacApkFont.loadFace). */
    FontFace loadFontFace(InputStream in);

    /** Registers an app's font family, each face {weight, italic, FontFace.font}; its name (MacApkFontFamilies). */
    String registerAppFamily(List<Object[]> faces);

    // ---- The view's clip and transform ----

    /** Clips to a child's rectangle (Canvas.macapkClipChildRect). */
    boolean clipChildRect(Canvas canvas, int l, int t, int r, int b);

    /** True when the rectangle is outside the clip (Canvas.macapkQuickRejectChild). */
    boolean quickRejectChild(Canvas canvas, float l, float t, float r, float b);

    /** The device origin and clip box when the matrix is a whole-pixel translation (Canvas.macapkDeviceOrigin). */
    boolean deviceOrigin(Canvas canvas, int[] out6);

    /** The matrix's x translation (Canvas.macapkGetCurrentTx). */
    float currentTx(Canvas canvas);

    /** The matrix's y translation (Canvas.macapkGetCurrentTy). */
    float currentTy(Canvas canvas);

    /** The outline's shape as a clip path (Outline.macapkClipPath). */
    Path outlineClipPath(Outline outline);

    /** A path outline's largest corner radius, NaN when none (Outline.macapkPathShadowRadius). */
    float outlinePathShadowRadius(Outline outline);

    // ---- Text: measuring, direction, bidi, replacement spans ----

    /** True when the locale's script is right to left (TextUtils.macapkIsLocaleRtl). */
    boolean isLocaleRtl(Locale locale);

    /** The width of a spanned range with its spans applied (Layout.macapkMeasureSpanned). */
    float measureSpanned(CharSequence src, int start, int end, TextPaint base, TextPaint work);

    /** The AWT font the paint draws with (Paint.macapkResolvedFont). */
    java.awt.Font resolvedFont(Paint paint);

    /** The FontRenderContext text is measured in (MacApkFont.textFrc). */
    java.awt.font.FontRenderContext textFontRenderContext();

    /** The fallback font that has a code point the primary lacks, or null (MacApkFont.fallbackBaseFor). */
    java.awt.Font fallbackFontFor(int codePoint, java.awt.Font primary);

    /** A font at a size and AWT style (MacApkFont.getFrom). */
    java.awt.Font deriveFont(java.awt.Font base, float sizePx, int awtStyle);

    /** True when the range has a right-to-left character (MacApkBidi.hasRtl). */
    boolean hasRtl(CharSequence s, int start, int end);

    /** The paragraph's direction from its first strong character (MacApkBidi.isRtlParagraph). */
    boolean isRtlParagraph(CharSequence s, int pos, boolean defaultRtl);

    /** A line cut at the bounds and bidi runs, in visual order: {start, end, level}... (MacApkBidi.visualPieces). */
    int[] bidiVisualPieces(String line, int[] bounds, int n, boolean paraRtl);

    /** The replacement span that covers the run, or null (MacApkReplacement.covering). */
    MetricAffectingSpan replacementCovering(Spanned sp, int start, int end);

    /** Paints a replacement span's run; its advance (MacApkReplacement.draw). */
    float drawReplacement(Canvas canvas, ReplacementSpan r, Spanned sp, int start, int end,
                          float x, float baselineY, TextPaint p);

    // ---- SurfaceTexture: camera, video, TextureView, the GL window ----

    int defaultWidth(SurfaceTexture st);

    int defaultHeight(SurfaceTexture st);

    void attachCameraSource(SurfaceTexture st, long sessionHandle, int width, int height);

    void detachCameraSource(SurfaceTexture st);

    boolean hasCameraSource(SurfaceTexture st);

    void queueFrame(SurfaceTexture st, long timestampNanos, float[] transform, int dataSpace);

    boolean connectProducer(SurfaceTexture st, Object producer);

    void disconnectProducer(SurfaceTexture st, Object producer);

    boolean hasProducer(SurfaceTexture st);

    void queueProducerFrame(SurfaceTexture st, int[] argb, int width, int height, int strideInPixels,
                            float[] transform, Consumer<int[]> recycler, boolean premultipliedSource);

    void registerVideoTarget(SurfaceTexture st);

    void unregisterVideoTargetIfSelf(SurfaceTexture st);

    boolean hasActiveVideoTarget();

    boolean offerDecodedFrame(int[] argb, int width, int height, int strideInPixels);

    Bitmap latchBitmap(SurfaceTexture st);

    int latchGeneration(SurfaceTexture st);

    // ---- Resources' colours and drawable setters ----

    /** A colour value of a resource ("#rgb", "#argb", "#rrggbb", "#aarrggbb") (Color.macapkParseResourceColor). */
    int parseResourceColor(String s);

    void setNinePatchDensities(NinePatchDrawable d, int source, int screen);

    void setNinePatchOpticalInsets(NinePatchDrawable d, int left, int top, int right, int bottom);

    void setAnimatedVectorInner(AnimatedVectorDrawable avd, Drawable inner);

    void setAnimatedImageBitmap(AnimatedImageDrawable aid, Bitmap bm);

    void setUseLevelForShape(GradientDrawable gd, boolean useLevel);

    boolean getUseLevelForShape(GradientDrawable gd);

    /** True when the vector drawable will paint nothing (VectorDrawable.macapkPaintsNothing). */
    boolean vectorPaintsNothing(VectorDrawable vd);

    /** True when the vector drawable stands in for a reference that did not resolve. */
    boolean isUnresolvedStub(VectorDrawable vd);

    void setUnresolvedStub(VectorDrawable vd, boolean stub);

    // ---- Debug dumps ----

    /** RenderNode's recording statistics, when asked for (RenderNode.macapkStatsDump). */
    void renderNodeStatsDump(String where);

    int dumpFillColor(GradientDrawable gd);

    boolean dumpHasFill(GradientDrawable gd);

    int dumpTint(GradientDrawable gd);

    int dumpStroke(GradientDrawable gd);

    int dumpPaintColor(ShapeDrawable sd);

    String dumpShape(ShapeDrawable sd);

    int dumpAlpha(ShapeDrawable sd);

    int dumpTint(ShapeDrawable sd);
}
