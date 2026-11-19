// Java2DGfx -- GfxBackend on Win.apk's own android.graphics / android.text
// (Java2D and AWT): each method calls the macapk* method or MacApk* class it
// stands for, and nothing else.
package com.macapk.loader.gfx;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.MacApkFont;
import android.graphics.MacApkFontFamilies;
import android.graphics.MacApkSystemFonts;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RenderNode;
import android.graphics.SurfaceTexture;
import android.graphics.Typeface;
import android.graphics.drawable.AnimatedImageDrawable;
import android.graphics.drawable.AnimatedVectorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.NinePatchDrawable;
import android.graphics.drawable.ShapeDrawable;
import android.graphics.drawable.VectorDrawable;
import android.text.Layout;
import android.text.MacApkBidi;
import android.text.MacApkReplacement;
import android.text.Spanned;
import android.text.TextPaint;
import android.text.TextUtils;
import android.text.style.MetricAffectingSpan;
import android.text.style.ReplacementSpan;

import java.io.InputStream;
import java.util.List;
import java.util.Locale;
import java.util.function.Consumer;
import java.util.function.Function;

final class Java2DGfx implements GfxBackend {

    static final Java2DGfx INSTANCE = new Java2DGfx();

    private Java2DGfx() {}

    // ---- Present a frame ----

    @Override public int[] argbBuffer(Bitmap bitmap) { return bitmap.macapkArgbBuffer(); }

    @Override public int[] liveArgbOrNull(Bitmap bitmap) { return bitmap.macapkLiveArgbOrNull(); }

    @Override public java.awt.image.BufferedImage awtImage(Bitmap bitmap) { return bitmap.macapkAwtImage(); }

    @Override public int drawOps(Canvas canvas) { return canvas.macapkDrawOps; }

    // ---- Fonts and Typeface creation ----

    @Override public String materializeSystemFonts(Function<String, String> hostFontFile) {
        return MacApkSystemFonts.materialize(hostFontFile);
    }

    @Override public Typeface.Builder typefaceBuilderFromStream(InputStream in) {
        return Typeface.Builder.macapkFromStream(in);
    }

    @Override public Typeface typefaceFromAppFamily(String registeredFamilyName) {
        return Typeface.macapkFromAppFamily(registeredFamilyName);
    }

    @Override public FontFace loadFontFace(InputStream in) {
        MacApkFont.LoadedFace lf = MacApkFont.loadFace(in);
        return lf == null ? null : new FontFace(lf.font, lf.weight, lf.italic);
    }

    @Override public String registerAppFamily(List<Object[]> faces) {
        return MacApkFontFamilies.registerAppFamily(faces);
    }

    // ---- The view's clip and transform ----

    @Override public boolean clipChildRect(Canvas canvas, int l, int t, int r, int b) {
        return canvas.macapkClipChildRect(l, t, r, b);
    }

    @Override public boolean quickRejectChild(Canvas canvas, float l, float t, float r, float b) {
        return canvas.macapkQuickRejectChild(l, t, r, b);
    }

    @Override public boolean deviceOrigin(Canvas canvas, int[] out6) { return canvas.macapkDeviceOrigin(out6); }

    @Override public float currentTx(Canvas canvas) { return canvas.macapkGetCurrentTx(); }

    @Override public float currentTy(Canvas canvas) { return canvas.macapkGetCurrentTy(); }

    @Override public Path outlineClipPath(Outline outline) { return outline.macapkClipPath(); }

    @Override public float outlinePathShadowRadius(Outline outline) { return outline.macapkPathShadowRadius; }

    // ---- Text ----

    @Override public boolean isLocaleRtl(Locale locale) { return TextUtils.macapkIsLocaleRtl(locale); }

    @Override public float measureSpanned(CharSequence src, int start, int end, TextPaint base, TextPaint work) {
        return Layout.macapkMeasureSpanned(src, start, end, base, work);
    }

    @Override public java.awt.Font resolvedFont(Paint paint) { return paint.macapkResolvedFont(); }

    @Override public java.awt.font.FontRenderContext textFontRenderContext() { return MacApkFont.textFrc(); }

    @Override public java.awt.Font fallbackFontFor(int codePoint, java.awt.Font primary) {
        return MacApkFont.fallbackBaseFor(codePoint, primary);
    }

    @Override public java.awt.Font deriveFont(java.awt.Font base, float sizePx, int awtStyle) {
        return MacApkFont.getFrom(base, sizePx, awtStyle);
    }

    @Override public boolean hasRtl(CharSequence s, int start, int end) { return MacApkBidi.hasRtl(s, start, end); }

    @Override public boolean isRtlParagraph(CharSequence s, int pos, boolean defaultRtl) {
        return MacApkBidi.isRtlParagraph(s, pos, defaultRtl);
    }

    @Override public int[] bidiVisualPieces(String line, int[] bounds, int n, boolean paraRtl) {
        return MacApkBidi.visualPieces(line, bounds, n, paraRtl);
    }

    @Override public MetricAffectingSpan replacementCovering(Spanned sp, int start, int end) {
        return MacApkReplacement.covering(sp, start, end);
    }

    @Override public float drawReplacement(Canvas canvas, ReplacementSpan r, Spanned sp, int start, int end,
                                           float x, float baselineY, TextPaint p) {
        return MacApkReplacement.draw(canvas, r, sp, start, end, x, baselineY, p);
    }

    // ---- SurfaceTexture ----

    @Override public int defaultWidth(SurfaceTexture st) { return st.macapkGetDefaultWidth(); }

    @Override public int defaultHeight(SurfaceTexture st) { return st.macapkGetDefaultHeight(); }

    @Override public void attachCameraSource(SurfaceTexture st, long sessionHandle, int width, int height) {
        st.macapkAttachCameraSource(sessionHandle, width, height);
    }

    @Override public void detachCameraSource(SurfaceTexture st) { st.macapkDetachCameraSource(); }

    @Override public boolean hasCameraSource(SurfaceTexture st) { return st.macapkHasCameraSource(); }

    @Override public void queueFrame(SurfaceTexture st, long timestampNanos, float[] transform, int dataSpace) {
        st.macapkQueueFrame(timestampNanos, transform, dataSpace);
    }

    @Override public boolean connectProducer(SurfaceTexture st, Object producer) {
        return st.macapkConnectProducer(producer);
    }

    @Override public void disconnectProducer(SurfaceTexture st, Object producer) {
        st.macapkDisconnectProducer(producer);
    }

    @Override public boolean hasProducer(SurfaceTexture st) { return st.macapkHasProducer(); }

    @Override public void queueProducerFrame(SurfaceTexture st, int[] argb, int width, int height,
                                             int strideInPixels, float[] transform, Consumer<int[]> recycler,
                                             boolean premultipliedSource) {
        st.macapkQueueProducerFrame(argb, width, height, strideInPixels, transform, recycler, premultipliedSource);
    }

    @Override public void registerVideoTarget(SurfaceTexture st) { st.macapkRegisterVideoTarget(); }

    @Override public void unregisterVideoTargetIfSelf(SurfaceTexture st) { st.macapkUnregisterVideoTargetIfSelf(); }

    @Override public boolean hasActiveVideoTarget() { return SurfaceTexture.macapkHasActiveVideoTarget(); }

    @Override public boolean offerDecodedFrame(int[] argb, int width, int height, int strideInPixels) {
        return SurfaceTexture.macapkOfferDecodedFrame(argb, width, height, strideInPixels);
    }

    @Override public Bitmap latchBitmap(SurfaceTexture st) { return st.macapkLatchBitmap(); }

    @Override public int latchGeneration(SurfaceTexture st) { return st.macapkLatchGeneration(); }

    // ---- Resources' colours and drawable setters ----

    @Override public int parseResourceColor(String s) { return Color.macapkParseResourceColor(s); }

    @Override public void setNinePatchDensities(NinePatchDrawable d, int source, int screen) {
        d.macapkSetDensities(source, screen);
    }

    @Override public void setNinePatchOpticalInsets(NinePatchDrawable d, int left, int top, int right, int bottom) {
        d.macapkSetOpticalInsets(left, top, right, bottom);
    }

    @Override public void setAnimatedVectorInner(AnimatedVectorDrawable avd, Drawable inner) {
        avd.macapkSetInner(inner);
    }

    @Override public void setAnimatedImageBitmap(AnimatedImageDrawable aid, Bitmap bm) { aid.macapkSetBitmap(bm); }

    @Override public void setUseLevelForShape(GradientDrawable gd, boolean useLevel) {
        gd.macapkSetUseLevelForShape(useLevel);
    }

    @Override public boolean getUseLevelForShape(GradientDrawable gd) { return gd.macapkGetUseLevelForShape(); }

    @Override public boolean vectorPaintsNothing(VectorDrawable vd) { return vd.macapkPaintsNothing(); }

    @Override public boolean isUnresolvedStub(VectorDrawable vd) { return vd.macapkUnresolvedStub; }

    @Override public void setUnresolvedStub(VectorDrawable vd, boolean stub) { vd.macapkUnresolvedStub = stub; }

    // ---- Debug dumps ----

    @Override public void renderNodeStatsDump(String where) { RenderNode.macapkStatsDump(where); }

    @Override public int dumpFillColor(GradientDrawable gd) { return gd.macapkDumpFillColor(); }

    @Override public boolean dumpHasFill(GradientDrawable gd) { return gd.macapkDumpHasFill(); }

    @Override public int dumpTint(GradientDrawable gd) { return gd.macapkDumpTint(); }

    @Override public int dumpStroke(GradientDrawable gd) { return gd.macapkDumpStroke(); }

    @Override public int dumpPaintColor(ShapeDrawable sd) { return sd.macapkDumpPaintColor(); }

    @Override public String dumpShape(ShapeDrawable sd) { return sd.macapkDumpShape(); }

    @Override public int dumpAlpha(ShapeDrawable sd) { return sd.macapkDumpAlpha(); }

    @Override public int dumpTint(ShapeDrawable sd) { return sd.macapkDumpTint(); }
}
