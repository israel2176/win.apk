// GfxBackend -- the one door from the rest of the framework into what it needs of
// android.graphics / android.text beyond Android's API (windows/README.md,
// "Milestone 2: the runtime on AOSP graphics"): the frame's pixels, the fonts,
// the view's clip and transform, text helpers, SurfaceTexture's producers,
// drawable setters, debug dumps.
//
// It was the seam between Win.apk's own Java2D graphics and AOSP's; the Java2D
// side is deleted, and AospGfx is its one implementation. It stays because
// Win.apk's own views and resources (TextView, View, Resources, ViewCanvasDriver)
// still call through it; it can be folded into its callers once those layers
// are AOSP's too (docs/research/aosp-views.md).
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

    /** The backend: AOSP's graphics, the only ones there are. */
    static GfxBackend get() { return AospGfx.INSTANCE; }

    /**
     * Always true: android.graphics is AOSP's (lib/aosp-graphics.jar first on the class path). Kept
     * for the views' and resources' callers that still branch on it, until those layers are AOSP's.
     */
    static boolean isAosp() { return true; }

    // ---- Present a frame: the pixels of a Bitmap, the draw-op counter ----

    /** A bitmap a frame (or a Surface's canvas) draws into, ARGB 8888 (AOSP: Bitmap.createBitmap). */
    Bitmap createFrameBitmap(int width, int height);

    /** The bitmap's ARGB pixels, a copy brought up to date. */
    int[] argbBuffer(Bitmap bitmap);

    /** The bitmap's live ARGB backing when it has one, else null. */
    int[] liveArgbOrNull(Bitmap bitmap);

    /**
     * A frame's pixels for the present: the same ARGB as argbBuffer, but the live backing when there
     * is one, and otherwise written into {@code reuse} when it holds exactly width x height (else a new
     * array), so a frame a vsync costs no 4 MB allocation. The caller keeps nothing past the present.
     */
    int[] frameArgb(Bitmap bitmap, int[] reuse);

    /** Row {@code y} of the bitmap's ARGB pixels (as argbBuffer has them) into out[0, width). */
    void argbRow(Bitmap bitmap, int y, int[] out);

    /** How many draw ops the canvas has run: a frame drew ink when it moved. */
    int drawOps(Canvas canvas);

    // ---- Fonts and Typeface creation ----

    /** Publishes /system/fonts from the host's fonts; the directory, or null. */
    String materializeSystemFonts(Function<String, String> hostFontFile);

    /** A Typeface.Builder over a font file's bytes. */
    Typeface.Builder typefaceBuilderFromStream(InputStream in);

    /** The Typeface of a family registerAppFamily returned. */
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

    /** A font file's face, or null when it does not parse. */
    FontFace loadFontFace(InputStream in);

    /** Registers an app's font family, each face {weight, italic, FontFace.font}; its name. */
    String registerAppFamily(List<Object[]> faces);

    // ---- The view's clip and transform ----

    /** Clips to a child's rectangle. */
    boolean clipChildRect(Canvas canvas, int l, int t, int r, int b);

    /** True when the rectangle is outside the clip. */
    boolean quickRejectChild(Canvas canvas, float l, float t, float r, float b);

    /** The device origin and clip box when the matrix is a whole-pixel translation. */
    boolean deviceOrigin(Canvas canvas, int[] out6);

    /** The matrix's x translation. */
    float currentTx(Canvas canvas);

    /** The matrix's y translation. */
    float currentTy(Canvas canvas);

    /** The outline's shape as a clip path. */
    Path outlineClipPath(Outline outline);

    /** A path outline's largest corner radius, NaN when none. */
    float outlinePathShadowRadius(Outline outline);

    // ---- Text: measuring, direction, bidi, replacement spans ----

    /** True when the locale's script is right to left. */
    boolean isLocaleRtl(Locale locale);

    /** The width of a spanned range with its spans applied. */
    float measureSpanned(CharSequence src, int start, int end, TextPaint base, TextPaint work);

    /** The AWT font the paint draws with. */
    java.awt.Font resolvedFont(Paint paint);

    /** The FontRenderContext text is measured in: AWT, unused on AOSP's graphics. */
    java.awt.font.FontRenderContext textFontRenderContext();

    /** The fallback font that has a code point the primary lacks, or null: AWT, unused on AOSP's graphics. */
    java.awt.Font fallbackFontFor(int codePoint, java.awt.Font primary);

    /** A font at a size and AWT style: AWT, unused on AOSP's graphics. */
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

    /** A colour value of a resource ("#rgb", "#argb", "#rrggbb", "#aarrggbb"). */
    int parseResourceColor(String s);

    /** A <vector> drawable from its resource file (VectorDrawable(Resources, int, Theme)). */
    Drawable vectorFromResource(android.content.res.Resources res, int id, android.content.res.Resources.Theme theme);

    /** A <vector> drawable from its element (VectorDrawable(Element, Theme, Resources)). */
    Drawable vectorFromElement(org.w3c.dom.Element e, android.content.res.Resources.Theme theme,
                               android.content.res.Resources res);

    /**
     * A drawable resource's XML (its root element; {@code id} for the log) inflated by the drawable
     * classes themselves, as on Android: AOSP's Drawable.createFromXmlInner, its DrawableInflater and
     * each drawable's inflate(). Null leaves it to Resources' own inflater (AOSP's inflate failed).
     */
    Drawable inflateXmlDrawable(android.content.res.Resources res, org.w3c.dom.Element root,
                                android.content.res.Resources.Theme theme, int id);

    /** A colour file's {@code <gradient>} (its root) as AOSP's GradientColor, for
     *  TypedArray.getComplexColor; null when it does not inflate. */
    android.content.res.ComplexColor gradientColor(android.content.res.Resources res, org.w3c.dom.Element root,
                                                   android.content.res.Resources.Theme theme);

    /** An animator's {@code valueType="pathType"} values (AnimatorInflater): a holder of AOSP's
     *  PathParser.PathData from {@code from} to {@code to} (either may be null) with AOSP's PathData
     *  evaluator, as AOSP's AnimatorInflater.getPVH makes it. Throws as AOSP does when the two
     *  paths cannot morph into each other. */
    android.animation.PropertyValuesHolder pathDataValues(String propertyName, String from, String to);

    /** A path from its SVG-like path data (AOSP's PathParser.createPathFromPathData), for an
     *  animator's motion along a path. */
    android.graphics.Path pathFromPathData(String pathData);

    /** An empty <layer-list> to add layers to (LayerDrawable(), which AOSP keeps to its package). */
    android.graphics.drawable.LayerDrawable layerDrawable();

    /** An empty <ripple> to add layers to (RippleDrawable(), which AOSP keeps to its package). */
    android.graphics.drawable.RippleDrawable rippleDrawable();

    /** A <rotate> drawable around the child (RotateDrawable(Drawable)). */
    android.graphics.drawable.RotateDrawable rotateDrawable(Drawable child);

    void setNinePatchDensities(NinePatchDrawable d, int source, int screen);

    void setNinePatchOpticalInsets(NinePatchDrawable d, int left, int top, int right, int bottom);

    /** An <animated-vector> over its base drawable. */
    AnimatedVectorDrawable animatedVector(Drawable inner);

    void setAnimatedImageBitmap(AnimatedImageDrawable aid, Bitmap bm);

    void setUseLevelForShape(GradientDrawable gd, boolean useLevel);

    boolean getUseLevelForShape(GradientDrawable gd);

    /** Whether Resources reads a <shape>'s ring attributes. */
    boolean readsShapeRing();

    /** Whether Resources reads a <shape>'s tint and tintMode. */
    boolean readsShapeTint();

    /** The drawable's tint, or null (Drawable.getTintList). */
    android.content.res.ColorStateList tintList(Drawable d);

    /** True when the vector drawable will paint nothing. */
    boolean vectorPaintsNothing(VectorDrawable vd);

    /** True when the vector drawable stands in for a reference that did not resolve. */
    boolean isUnresolvedStub(VectorDrawable vd);

    void setUnresolvedStub(VectorDrawable vd, boolean stub);

    // ---- Debug dumps ----

    /** RenderNode's recording statistics, when asked for. */
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
