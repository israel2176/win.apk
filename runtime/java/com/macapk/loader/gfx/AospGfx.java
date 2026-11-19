// AospGfx -- GfxBackend on AOSP's own android.graphics / android.text (WINAPK_AOSP_GFX=1,
// windows/README.md, "Milestone 2: the runtime on AOSP graphics", step 2).
//
// With the switch on, lib/aosp-graphics.jar is first on the class path, so android.graphics.Canvas,
// Paint, Bitmap, Typeface... are AOSP's, drawn by Skia in robolectric-nativeruntime.dll; the rest of
// the framework (View, TextView, Resources) is still Win.apk's and draws through the public API in
// software, as View.draw(Canvas) does into a bitmap on a device. This class answers the seam with
// the AOSP equivalents and touches no macapk* member or MacApk* graphics class: those are not there
// (NoSuchMethodError). It compiles against Win.apk's android.graphics, so whatever only AOSP has
// (Typeface.setSystemFontMap, SystemFonts, a drawable's state fields) is reached by reflection.
// The methods whose Java2D answer uses only classes the jar does not replace (bidi, replacement
// spans, SurfaceTexture, which stays Win.apk's) go to Java2DGfx.
package com.macapk.loader.gfx;

import android.content.res.ColorStateList;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Matrix;
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

import java.io.File;
import java.io.InputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.WeakHashMap;
import java.util.function.Consumer;
import java.util.function.Function;

final class AospGfx implements GfxBackend {

    static final AospGfx INSTANCE = new AospGfx();

    private static final String TAG = "Win.apk.AospGfx";

    private final GfxBackend j2d = Java2DGfx.INSTANCE;

    /** VectorDrawables Resources made to stand in for a reference that did not resolve. */
    private final Map<VectorDrawable, Boolean> stubs = Collections.synchronizedMap(new WeakHashMap<>());

    /** drawOps' answer: AOSP's Canvas counts nothing, so every frame is taken to have drawn. */
    private int ops;

    private AospGfx() {}

    private static void log(String s) {
        try { com.macapk.loader.NativeLog.i(TAG, s); } catch (Throwable t) { System.err.println(TAG + ": " + s); }
    }

    // ---- Present a frame ----

    @Override public Bitmap createFrameBitmap(int width, int height) {
        return Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
    }

    /** Skia's pixels are native: one copy out, unpremultiplied ARGB as the Java2D bitmap holds them. */
    @Override public int[] argbBuffer(Bitmap bitmap) {
        int w = bitmap.getWidth(), h = bitmap.getHeight();
        int[] out = new int[w * h];
        bitmap.getPixels(out, 0, w, 0, 0, w, h);
        return out;
    }

    @Override public int[] liveArgbOrNull(Bitmap bitmap) { return null; }

    @Override public java.awt.image.BufferedImage awtImage(Bitmap bitmap) {
        int w = bitmap.getWidth(), h = bitmap.getHeight();
        java.awt.image.BufferedImage img = new java.awt.image.BufferedImage(w, h,
                java.awt.image.BufferedImage.TYPE_INT_ARGB);
        img.setRGB(0, 0, w, h, argbBuffer(bitmap), 0, w);
        return img;
    }

    @Override public int drawOps(Canvas canvas) { return ++ops; }

    // ---- Fonts and Typeface creation ----

    /**
     * AOSP's Typeface has no default until someone hands it the system font map (its static
     * initializer only preloads files): on a device the zygote does, under Robolectric
     * loadPreinstalledSystemFontMap. Until then a Paint with no typeface measures through HWUI's
     * Typeface::resolveDefault(nullptr), which aborts the process ("Assertion failed: src ==
     * nullptr && gDefaultTypeface == nullptr"). So: the families of the runtime's own fonts.xml
     * (bin/aosp/fonts, -Dwinapk.aosp.fonts), as the aospgfx test does, and that folder is
     * /system/fonts for native code too, so both read the same files.
     */
    @Override public String materializeSystemFonts(Function<String, String> hostFontFile) {
        String dir = System.getProperty("winapk.aosp.fonts");
        if (dir == null || dir.isEmpty()) {
            log("no -Dwinapk.aosp.fonts: the system fonts are not set up");
            return null;
        }
        String d = dir.replace('\\', '/');
        if (!d.endsWith("/")) d += "/";
        try {
            Class<?> systemFonts = Class.forName("android.graphics.fonts.SystemFonts");
            Method config = systemFonts.getDeclaredMethod("getSystemFontConfigInternal", String.class,
                    String.class, String.class, String.class, Map.class, long.class, int.class);
            config.setAccessible(true);
            Object cfg = config.invoke(null, d + "fonts.xml", d, "/nonexistent.xml", "/nonexistent/",
                    new java.util.HashMap<>(), 0L, 0);
            Class<?> fontConfig = Class.forName("android.text.FontConfig");
            Object fallback = systemFonts.getMethod("buildSystemFallback", fontConfig).invoke(null, cfg);
            Object map = systemFonts.getMethod("buildSystemTypefaces", fontConfig, Map.class)
                    .invoke(null, cfg, fallback);
            Typeface.class.getMethod("setSystemFontMap", Map.class).invoke(null, map);
            log("system fonts from " + d + "fonts.xml: " + ((Map<?, ?>) map).size() + " families");
        } catch (Throwable t) {
            Throwable c = t instanceof java.lang.reflect.InvocationTargetException && t.getCause() != null
                    ? t.getCause() : t;
            log("system fonts from " + d + "fonts.xml failed: " + c);
            return null;
        }
        return new File(dir).getAbsolutePath();
    }

    @Override public Typeface.Builder typefaceBuilderFromStream(InputStream in) {
        // AOSP's Builder takes a file; an app's font resource is a stream.
        try {
            File f = File.createTempFile("font", ".ttf");
            f.deleteOnExit();
            try (java.io.OutputStream out = new java.io.FileOutputStream(f)) { in.transferTo(out); }
            return new Typeface.Builder(f);
        } catch (java.io.IOException e) {
            throw new RuntimeException(e);
        }
    }

    // Font families of an app (res/font/*.xml): not yet; Resources falls back to the default face.
    @Override public Typeface typefaceFromAppFamily(String registeredFamilyName) { return null; }

    /** The face's bytes, weight 400 upright: enough for Resources to pick a file of the family. */
    @Override public FontFace loadFontFace(InputStream in) {
        try {
            return new FontFace(in.readAllBytes(), 400, false);
        } catch (java.io.IOException e) {
            return null;
        }
    }

    @Override public String registerAppFamily(List<Object[]> faces) { return null; }

    // ---- The view's clip and transform ----

    @Override public boolean clipChildRect(Canvas canvas, int l, int t, int r, int b) {
        return canvas.clipRect(l, t, r, b);
    }

    @Override public boolean quickRejectChild(Canvas canvas, float l, float t, float r, float b) {
        if (!(r > l) || !(b > t)) return false;
        return canvas.quickReject(l, t, r, b);
    }

    // Only a cache's key in View (its recording epoch); false turns that cache off.
    @Override public boolean deviceOrigin(Canvas canvas, int[] out6) { return false; }

    @Override public float currentTx(Canvas canvas) { return matrixValue(canvas, Matrix.MTRANS_X); }

    @Override public float currentTy(Canvas canvas) { return matrixValue(canvas, Matrix.MTRANS_Y); }

    private static float matrixValue(Canvas canvas, int index) {
        Matrix m = new Matrix();
        canvas.getMatrix(m);
        float[] v = new float[9];
        m.getValues(v);
        return v[index];
    }

    @Override public Path outlineClipPath(Outline outline) {
        if (outline.mMode == Outline.MODE_PATH && outline.mPath != null) return outline.mPath;
        if (outline.mMode == Outline.MODE_ROUND_RECT && !outline.mRect.isEmpty()) {
            Path p = new Path();
            float r = Math.max(0f, outline.mRadius);
            p.addRoundRect(outline.mRect.left, outline.mRect.top, outline.mRect.right, outline.mRect.bottom,
                    r, r, Path.Direction.CW);
            return p;
        }
        return null;
    }

    @Override public float outlinePathShadowRadius(Outline outline) { return Float.NaN; }

    // ---- Text ----

    @Override public boolean isLocaleRtl(Locale locale) { return j2d.isLocaleRtl(locale); }

    /** AOSP's Layout.getDesiredWidth: TextLine over the spans, as a line is measured for drawing. */
    @Override public float measureSpanned(CharSequence src, int start, int end, TextPaint base, TextPaint work) {
        return android.text.Layout.getDesiredWidth(src, start, end, base);
    }

    // The AWT side of TextView's fallback line spacing: no AWT font here, so the line keeps its
    // paint's own metrics.
    @Override public java.awt.Font resolvedFont(Paint paint) { return null; }

    @Override public java.awt.font.FontRenderContext textFontRenderContext() {
        return new java.awt.font.FontRenderContext(null, true, true);
    }

    @Override public java.awt.Font fallbackFontFor(int codePoint, java.awt.Font primary) { return null; }

    @Override public java.awt.Font deriveFont(java.awt.Font base, float sizePx, int awtStyle) {
        return base.deriveFont(awtStyle, sizePx);
    }

    @Override public boolean hasRtl(CharSequence s, int start, int end) { return j2d.hasRtl(s, start, end); }

    @Override public boolean isRtlParagraph(CharSequence s, int pos, boolean defaultRtl) {
        return j2d.isRtlParagraph(s, pos, defaultRtl);
    }

    @Override public int[] bidiVisualPieces(String line, int[] bounds, int n, boolean paraRtl) {
        return j2d.bidiVisualPieces(line, bounds, n, paraRtl);
    }

    @Override public MetricAffectingSpan replacementCovering(Spanned sp, int start, int end) {
        return j2d.replacementCovering(sp, start, end);
    }

    @Override public float drawReplacement(Canvas canvas, ReplacementSpan r, Spanned sp, int start, int end,
                                           float x, float baselineY, TextPaint p) {
        return j2d.drawReplacement(canvas, r, sp, start, end, x, baselineY, p);
    }

    // ---- SurfaceTexture: Win.apk's, left out of the AOSP jar ----

    @Override public int defaultWidth(SurfaceTexture st) { return j2d.defaultWidth(st); }

    @Override public int defaultHeight(SurfaceTexture st) { return j2d.defaultHeight(st); }

    @Override public void attachCameraSource(SurfaceTexture st, long sessionHandle, int width, int height) {
        j2d.attachCameraSource(st, sessionHandle, width, height);
    }

    @Override public void detachCameraSource(SurfaceTexture st) { j2d.detachCameraSource(st); }

    @Override public boolean hasCameraSource(SurfaceTexture st) { return j2d.hasCameraSource(st); }

    @Override public void queueFrame(SurfaceTexture st, long timestampNanos, float[] transform, int dataSpace) {
        j2d.queueFrame(st, timestampNanos, transform, dataSpace);
    }

    @Override public boolean connectProducer(SurfaceTexture st, Object producer) {
        return j2d.connectProducer(st, producer);
    }

    @Override public void disconnectProducer(SurfaceTexture st, Object producer) {
        j2d.disconnectProducer(st, producer);
    }

    @Override public boolean hasProducer(SurfaceTexture st) { return j2d.hasProducer(st); }

    @Override public void queueProducerFrame(SurfaceTexture st, int[] argb, int width, int height,
                                             int strideInPixels, float[] transform, Consumer<int[]> recycler,
                                             boolean premultipliedSource) {
        j2d.queueProducerFrame(st, argb, width, height, strideInPixels, transform, recycler, premultipliedSource);
    }

    @Override public void registerVideoTarget(SurfaceTexture st) { j2d.registerVideoTarget(st); }

    @Override public void unregisterVideoTargetIfSelf(SurfaceTexture st) { j2d.unregisterVideoTargetIfSelf(st); }

    @Override public boolean hasActiveVideoTarget() { return j2d.hasActiveVideoTarget(); }

    @Override public boolean offerDecodedFrame(int[] argb, int width, int height, int strideInPixels) {
        return j2d.offerDecodedFrame(argb, width, height, strideInPixels);
    }

    @Override public Bitmap latchBitmap(SurfaceTexture st) { return j2d.latchBitmap(st); }

    @Override public int latchGeneration(SurfaceTexture st) { return j2d.latchGeneration(st); }

    // ---- Resources' colours and drawable setters ----

    /** "#rgb", "#argb", "#rrggbb", "#aarrggbb", as aapt accepts them (Color.parseColor takes the long two). */
    @Override public int parseResourceColor(String s) {
        if (s.charAt(0) != '#') return android.graphics.Color.parseColor(s);
        String h = s.substring(1).trim();
        if (h.length() == 3 || h.length() == 4) {
            StringBuilder e = new StringBuilder(h.length() * 2);
            for (int i = 0; i < h.length(); i++) e.append(h.charAt(i)).append(h.charAt(i));
            h = e.toString();
        }
        long c = Long.parseLong(h, 16);
        if (h.length() == 6) return (int) (c | 0xff000000L);
        if (h.length() == 8) return (int) c;
        throw new IllegalArgumentException("Unknown color");
    }

    /** AOSP's VectorDrawable from the resource's XML (AospVector); an empty one when it is not a <vector>. */
    @Override public Drawable vectorFromResource(android.content.res.Resources res, int id,
                                                 android.content.res.Resources.Theme theme) {
        org.w3c.dom.Document doc = res.macapkDrawableXml(id);
        org.w3c.dom.Element root = doc == null ? null : doc.getDocumentElement();
        String tag = root == null ? null : root.getLocalName() != null ? root.getLocalName() : root.getTagName();
        if ("vector".equals(tag)) return vectorFromElement(root, theme, res);
        return new VectorDrawable();
    }

    @Override public Drawable vectorFromElement(org.w3c.dom.Element e, android.content.res.Resources.Theme theme,
                                                android.content.res.Resources res) {
        try {
            return AospVector.build(e, theme, res);
        } catch (Throwable t) {
            Throwable c = t instanceof java.lang.reflect.InvocationTargetException && t.getCause() != null
                    ? t.getCause() : t;
            log("vector: " + c);
            return new VectorDrawable();
        }
    }

    @Override public android.graphics.drawable.LayerDrawable layerDrawable() {
        return new android.graphics.drawable.LayerDrawable(new Drawable[0]);
    }

    /** AOSP's public constructor wants a colour; Resources sets the real one after. */
    @Override public android.graphics.drawable.RippleDrawable rippleDrawable() {
        return new android.graphics.drawable.RippleDrawable(ColorStateList.valueOf(0), null, null);
    }

    @Override public android.graphics.drawable.RotateDrawable rotateDrawable(Drawable child) {
        android.graphics.drawable.RotateDrawable r = new android.graphics.drawable.RotateDrawable();
        r.setDrawable(child);
        return r;
    }

    /** The picture's folder density on its bitmap, the screen's as the target: AOSP scales a nine-patch
     *  (and its padding) from one to the other itself. */
    @Override public void setNinePatchDensities(NinePatchDrawable d, int source, int screen) {
        try {
            Object state = field(d, "mNinePatchState");
            Object ninePatch = field(state, "mNinePatch");
            ((android.graphics.NinePatch) ninePatch).getBitmap().setDensity(source);
        } catch (Throwable t) {
            log("nine-patch density: " + t);
        }
        d.setTargetDensity(screen);
    }

    @Override public void setNinePatchOpticalInsets(NinePatchDrawable d, int left, int top, int right, int bottom) {
        try {
            Object state = field(d, "mNinePatchState");
            setField(state, "mOpticalInsets", android.graphics.Insets.of(left, top, right, bottom));
            // The drawable's own insets are the state's scaled to its density, made here.
            Method compute = NinePatchDrawable.class.getDeclaredMethod("computeBitmapSize");
            compute.setAccessible(true);
            compute.invoke(d);
        } catch (Throwable t) {
            log("nine-patch optical insets: " + t);
        }
    }

    /** AOSP's AnimatedVectorDrawable drawing the base vector (its state's mVectorDrawable, which its
     *  own inflate sets); the <target> animators are not read, as on the Java2D side. */
    @Override public AnimatedVectorDrawable animatedVector(Drawable inner) {
        AnimatedVectorDrawable avd = new AnimatedVectorDrawable();
        if (inner instanceof VectorDrawable) {
            try {
                Object state = field(avd, "mAnimatedVectorState");
                setField(state, "mVectorDrawable", inner);
                inner.setCallback((Drawable.Callback) field(avd, "mCallback"));
            } catch (Throwable t) {
                log("animated-vector: " + t);
            }
        }
        return avd;
    }

    // Win.apk's own inflation of <animated-image>: not on AOSP's classes yet.

    @Override public void setAnimatedImageBitmap(AnimatedImageDrawable aid, Bitmap bm) {}

    @Override public void setUseLevelForShape(GradientDrawable gd, boolean useLevel) {
        try {
            setField(field(gd, "mGradientState"), "mUseLevelForShape", useLevel);
        } catch (Throwable t) {
            log("useLevel: " + t);
        }
    }

    @Override public boolean getUseLevelForShape(GradientDrawable gd) {
        try {
            return (Boolean) field(field(gd, "mGradientState"), "mUseLevelForShape");
        } catch (Throwable t) {
            return true;
        }
    }

    @Override public boolean readsShapeRing() { return true; }

    @Override public boolean readsShapeTint() { return true; }

    @Override public ColorStateList tintList(Drawable d) {
        // AOSP keeps the tint in each drawable's own state; only the common kinds say.
        for (String state : new String[] {"mGradientState", "mBitmapState", "mVectorState", "mColorState",
                "mNinePatchState", "mShapeState"}) {
            try {
                Object s = field(d, state);
                Object tint = field(s, "mTint");
                return tint instanceof ColorStateList ? (ColorStateList) tint : null;
            } catch (Throwable ignored) {
            }
        }
        return null;
    }

    @Override public boolean vectorPaintsNothing(VectorDrawable vd) { return false; }

    @Override public boolean isUnresolvedStub(VectorDrawable vd) { return stubs.containsKey(vd); }

    @Override public void setUnresolvedStub(VectorDrawable vd, boolean stub) {
        if (stub) stubs.put(vd, Boolean.TRUE); else stubs.remove(vd);
    }

    // ---- Debug dumps: Java2D's only ----

    @Override public void renderNodeStatsDump(String where) {}

    @Override public int dumpFillColor(GradientDrawable gd) { return 0; }

    @Override public boolean dumpHasFill(GradientDrawable gd) { return false; }

    @Override public int dumpTint(GradientDrawable gd) { return 0; }

    @Override public int dumpStroke(GradientDrawable gd) { return 0; }

    @Override public int dumpPaintColor(ShapeDrawable sd) { return sd.getPaint().getColor(); }

    @Override public String dumpShape(ShapeDrawable sd) {
        return sd.getShape() == null ? "null" : sd.getShape().getClass().getSimpleName();
    }

    @Override public int dumpAlpha(ShapeDrawable sd) { return sd.getAlpha(); }

    @Override public int dumpTint(ShapeDrawable sd) { return 0; }

    // ---- Reflection on AOSP's members Win.apk's classes do not declare ----

    private static Field find(Class<?> c, String name) throws NoSuchFieldException {
        for (Class<?> k = c; k != null; k = k.getSuperclass()) {
            try {
                Field f = k.getDeclaredField(name);
                f.setAccessible(true);
                return f;
            } catch (NoSuchFieldException ignored) {
            }
        }
        throw new NoSuchFieldException(c.getName() + "." + name);
    }

    private static Object field(Object o, String name) throws ReflectiveOperationException {
        return find(o.getClass(), name).get(o);
    }

    private static void setField(Object o, String name, Object v) throws ReflectiveOperationException {
        find(o.getClass(), name).set(o, v);
    }
}
