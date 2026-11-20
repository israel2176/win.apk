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
        // That folder is /system/fonts and /system/etc/fonts.xml before anything below touches
        // Typeface: its static initializer reads the device's config by those names
        // (SystemFonts.getSystemPreinstalledFontConfig), for the families' locales and the fonts it
        // preloads; Boot sets the same folder again from what this returns.
        try {
            com.macapk.loader.NativeCocoa.setSystemFontsDir(new File(dir).getAbsolutePath());
        } catch (Throwable t) {
            log("/system/fonts not mapped: " + t);
        }
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

    /** The app font families built so far (registerAppFamily), by the name handed back, and the
     *  names by what they were built from. */
    private final Map<String, Typeface> appFamilies = new java.util.concurrent.ConcurrentHashMap<>();
    private final Map<String, String> appFamilyNames = new java.util.concurrent.ConcurrentHashMap<>();

    @Override public Typeface typefaceFromAppFamily(String registeredFamilyName) {
        return registeredFamilyName == null ? null : appFamilies.get(registeredFamilyName);
    }

    /** A font file of an app's family: AOSP's Font over its bytes, whose weight and slant come from
     *  the file's own OS/2 table (the native runtime reads it), as RESOLVE_BY_FONT_TABLE does. */
    @Override public FontFace loadFontFace(InputStream in) {
        try {
            byte[] bytes = in.readAllBytes();
            android.graphics.fonts.Font font = new android.graphics.fonts.Font.Builder(direct(bytes)).build();
            android.graphics.fonts.FontStyle style = font.getStyle();
            return new FontFace(bytes, style.getWeight(),
                    style.getSlant() == android.graphics.fonts.FontStyle.FONT_SLANT_ITALIC);
        } catch (Throwable t) {
            log("font file: " + t);
            return null;
        }
    }

    /**
     * An app's {@code <font-family>} (res/font/*.xml) as AOSP's Typeface.createFromResources builds a
     * FontFamilyFilesResourceEntry: a Font per {@code <font>} with its declared (or the file's) weight
     * and slant, one FontFamily of them all in the file's order, and a Typeface over it whose style is
     * the face closest to 400 upright (FontStyle.getMatchScore). Paint and minikin then pick the
     * family's own bold or italic file. Only the source differs: the file's bytes, where AOSP opens
     * it through its AssetManager. {@code faces}: {weight, italic, bytes} per {@code <font>}.
     */
    @Override public synchronized String registerAppFamily(List<Object[]> faces) {
        // One family per distinct list of faces (as Typeface's resource cache keeps one per font
        // resource): a family's fonts are native memory, and every getFont of it asks again.
        StringBuilder key = new StringBuilder();
        for (Object[] f : faces) {
            byte[] b = (byte[]) f[2];
            key.append(f[0]).append(f[1]).append(b.length).append('/').append(java.util.Arrays.hashCode(b)).append(';');
        }
        String known = appFamilyNames.get(key.toString());
        if (known != null) return known;
        try {
            android.graphics.fonts.FontFamily.Builder family = null;
            for (Object[] f : faces) {
                android.graphics.fonts.Font font = new android.graphics.fonts.Font.Builder(direct((byte[]) f[2]))
                        .setWeight((Integer) f[0])
                        .setSlant((Boolean) f[1] ? android.graphics.fonts.FontStyle.FONT_SLANT_ITALIC
                                : android.graphics.fonts.FontStyle.FONT_SLANT_UPRIGHT)
                        .build();
                if (family == null) family = new android.graphics.fonts.FontFamily.Builder(font);
                else family.addFont(font);
            }
            if (family == null) return null;
            android.graphics.fonts.FontFamily built = family.build();
            android.graphics.fonts.FontStyle best = built.getFont(0).getStyle();
            int bestScore = matchScore(best);
            for (int i = 1; i < built.getSize(); i++) {
                android.graphics.fonts.FontStyle candidate = built.getFont(i).getStyle();
                int score = matchScore(candidate);
                if (score < bestScore) {
                    best = candidate;
                    bestScore = score;
                }
            }
            Typeface tf = new Typeface.CustomFallbackBuilder(built).setStyle(best).build();
            String name = "winapk-app-family-" + appFamilies.size();
            appFamilies.put(name, tf);
            appFamilyNames.put(key.toString(), name);
            return name;
        } catch (Throwable t) {
            log("app font family: " + t);
            return null;
        }
    }

    /** AOSP FontStyle.getMatchScore (hidden) against 400 upright, the style createFromResources keeps. */
    private static int matchScore(android.graphics.fonts.FontStyle s) {
        return Math.abs(s.getWeight() - android.graphics.fonts.FontStyle.FONT_WEIGHT_NORMAL) / 100
                + (s.getSlant() == android.graphics.fonts.FontStyle.FONT_SLANT_UPRIGHT ? 0 : 2);
    }

    /** Font.Builder(ByteBuffer) wants a direct buffer (the native runtime reads it in place). */
    private static java.nio.ByteBuffer direct(byte[] bytes) {
        java.nio.ByteBuffer b = java.nio.ByteBuffer.allocateDirect(bytes.length);
        b.put(bytes).flip();
        return b;
    }

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

    /**
     * The drawable inflates itself, as on Android: AOSP's Drawable.createFromXmlInner hands the element
     * to AOSP's DrawableInflater (Resources.getDrawableInflater), which makes the drawable of its tag
     * (or class) and calls its inflate(), which reads its own attributes through
     * com.android.internal.R.styleable and Win.apk's TypedArray, its children through the same parser
     * and its references (android:drawable="@...") through Resources.getDrawable, so back here. A
     * failure is logged once per resource and answered null: Resources' own inflater then builds it.
     */
    @Override public Drawable inflateXmlDrawable(android.content.res.Resources res, org.w3c.dom.Element root,
                                                 android.content.res.Resources.Theme theme, int id) {
        org.xmlpull.v1.XmlPullParser parser = res.macapkXmlParserAt(root);
        if (parser == null) return null;
        try {
            return Drawable.createFromXmlInner(res, parser, (android.util.AttributeSet) parser, theme);
        } catch (Throwable t) {
            Throwable c = t instanceof java.lang.reflect.InvocationTargetException && t.getCause() != null
                    ? t.getCause() : t;
            if (inflateFailures.add(id)) {
                String name;
                try { name = res.getResourceName(id); } catch (Throwable ignored) { name = "0x" + Integer.toHexString(id); }
                log("drawable " + name + " <" + root.getTagName() + ">: AOSP's inflate failed (" + c
                        + "); Resources' inflater builds it");
            }
            return null;
        }
    }

    /** Resources whose AOSP inflate failed, logged once. */
    private final java.util.Set<Integer> inflateFailures = Collections.synchronizedSet(new java.util.HashSet<>());

    /** AOSP's AnimatorInflater.getPVH for VALUE_TYPE_PATH, over AOSP's android.util.PathParser (in the
     *  jar; Win.apk's framework has none, so by reflection). */
    @Override public android.animation.PropertyValuesHolder pathDataValues(String propertyName, String from,
                                                                         String to) {
        try {
            Class<?> parser = Class.forName("android.util.PathParser");
            Class<?> data = Class.forName("android.util.PathParser$PathData");
            java.lang.reflect.Constructor<?> fromString = data.getConstructor(String.class);
            Object nodesFrom = from == null ? null : fromString.newInstance(from);
            Object nodesTo = to == null ? null : fromString.newInstance(to);
            if (nodesFrom == null && nodesTo == null) return null;
            android.animation.TypeEvaluator<Object> evaluator = new PathDataEvaluator(parser, data);
            if (nodesFrom != null && nodesTo != null) {
                Method canMorph = parser.getMethod("canMorph", data, data);
                if (!(Boolean) canMorph.invoke(null, nodesFrom, nodesTo)) {
                    throw new android.view.InflateException(" Can't morph from " + from + " to " + to);
                }
                return android.animation.PropertyValuesHolder.ofObject(propertyName, evaluator, nodesFrom, nodesTo);
            }
            return android.animation.PropertyValuesHolder.ofObject(propertyName, evaluator,
                    nodesFrom != null ? nodesFrom : nodesTo);
        } catch (ReflectiveOperationException e) {
            Throwable c = e instanceof java.lang.reflect.InvocationTargetException && e.getCause() != null
                    ? e.getCause() : e;
            if (c instanceof RuntimeException) throw (RuntimeException) c;
            throw new android.view.InflateException("pathType values: " + c);
        }
    }

    @Override public Path pathFromPathData(String pathData) {
        try {
            return (Path) Class.forName("android.util.PathParser").getMethod("createPathFromPathData", String.class)
                    .invoke(null, pathData);
        } catch (ReflectiveOperationException e) {
            Throwable c = e instanceof java.lang.reflect.InvocationTargetException && e.getCause() != null
                    ? e.getCause() : e;
            if (c instanceof RuntimeException) throw (RuntimeException) c;
            throw new android.view.InflateException("pathData: " + c);
        }
    }

    /** AOSP AnimatorInflater.PathDataEvaluator: one PathData, interpolated into at each fraction. */
    private static final class PathDataEvaluator implements android.animation.TypeEvaluator<Object> {
        private final Object pathData;
        private final Method interpolate;

        PathDataEvaluator(Class<?> parser, Class<?> data) throws ReflectiveOperationException {
            pathData = data.getConstructor().newInstance();
            interpolate = parser.getMethod("interpolatePathData", data, data, data, float.class);
        }

        @Override public Object evaluate(float fraction, Object start, Object end) {
            boolean ok;
            try {
                ok = (Boolean) interpolate.invoke(null, pathData, start, end, fraction);
            } catch (ReflectiveOperationException e) {
                throw new IllegalStateException(e);
            }
            if (!ok) throw new IllegalArgumentException("Can't interpolate between two incompatible pathData");
            return pathData;
        }
    }

    /** AOSP's GradientColor.createFromXmlInner (package-private) over the element. */
    @Override public android.content.res.ComplexColor gradientColor(android.content.res.Resources res,
                                                                    org.w3c.dom.Element root,
                                                                    android.content.res.Resources.Theme theme) {
        org.xmlpull.v1.XmlPullParser parser = res.macapkXmlParserAt(root);
        if (parser == null) return null;
        try {
            Method m = Class.forName("android.content.res.GradientColor").getDeclaredMethod("createFromXmlInner",
                    android.content.res.Resources.class, org.xmlpull.v1.XmlPullParser.class,
                    android.util.AttributeSet.class, android.content.res.Resources.Theme.class);
            m.setAccessible(true);
            return (android.content.res.ComplexColor) m.invoke(null, res, parser, parser, theme);
        } catch (Throwable t) {
            Throwable c = t instanceof java.lang.reflect.InvocationTargetException && t.getCause() != null
                    ? t.getCause() : t;
            log("gradient colour: " + c);
            return null;
        }
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
     *  (and its padding) from one to the other itself. The drawable was made over a bitmap that had
     *  the decoder's density then, and works its size out once, as it is made and when its target
     *  density changes: when the target is already the screen's, nothing would work it out again
     *  and a bitmap decoded for the first time kept its size unscaled (an mdpi button background
     *  29 px wide on an xhdpi screen, where every later load of the cached picture was 58), so it
     *  is worked out here, as AOSP's own decode (bitmap density first, drawable after) has it. */
    @Override public void setNinePatchDensities(NinePatchDrawable d, int source, int screen) {
        try {
            Object state = field(d, "mNinePatchState");
            Object ninePatch = field(state, "mNinePatch");
            ((android.graphics.NinePatch) ninePatch).getBitmap().setDensity(source);
        } catch (Throwable t) {
            log("nine-patch density: " + t);
        }
        d.setTargetDensity(screen);
        computeNinePatchSize(d);
    }

    /** NinePatchDrawable.computeBitmapSize: its size, padding and insets from the bitmap's density to
     *  the target's. */
    private static void computeNinePatchSize(NinePatchDrawable d) {
        try {
            Method compute = NinePatchDrawable.class.getDeclaredMethod("computeBitmapSize");
            compute.setAccessible(true);
            compute.invoke(d);
        } catch (Throwable t) {
            log("nine-patch size: " + t);
        }
    }

    @Override public void setNinePatchOpticalInsets(NinePatchDrawable d, int left, int top, int right, int bottom) {
        try {
            Object state = field(d, "mNinePatchState");
            setField(state, "mOpticalInsets", android.graphics.Insets.of(left, top, right, bottom));
        } catch (Throwable t) {
            log("nine-patch optical insets: " + t);
            return;
        }
        // The drawable's own insets are the state's scaled to its density, made here.
        computeNinePatchSize(d);
    }

    /** AOSP's AnimatedVectorDrawable drawing the base vector (its state's mVectorDrawable, which its
     *  own inflate sets), for Resources' own inflater, which builds an <animated-vector> only when AOSP's
     *  inflate failed: the <target> animators are not read, as on the Java2D side. */
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
