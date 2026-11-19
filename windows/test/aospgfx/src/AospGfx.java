// AospGfx -- AOSP's real android.graphics / android.text (android-all 14, API 34) drawing through
// Robolectric's native runtime (Skia raster, minikin, HarfBuzz, FreeType, ICU, libpng) in a HotSpot
// JVM: milestone 1 of moving Win.apk's drawing to Android's own (windows/README.md, "AOSP graphics").
//
//   AospGfx <redirect agent lib> <native runtime lib> <icudt68l.dat> <fonts dir> <out dir> [bench W H warmup frames]
//
// Loads the redirect agent (redirect_agent.c) and then the native runtime, sets the system fonts up
// from the runtime's fonts.xml, draws a picture (rects, a linear gradient, a stroked cubic path,
// "Hello Skia" + Hebrew, a PNG decoded by libpng) and checks pixels of it, lays the mixed line out
// with StaticLayout (minikin's line breaker, bidi), writes <out>/aospgfx.png and reads it back.
// Each check prints [PASS] or [FAIL]; the exit code is 1 when one failed. With "bench" it then
// draws the list frame the Skia benchmark measured AWT, Win.apk's Canvas and Skija with (a header,
// 40 list rows, a sheet in a 75% layer; windows/README.md has the numbers) W x H, and prints a
// RESULT line with that benchmark's fields.
//
// Every class on the class path is android-all's except the stand-ins in this folder (Log, Trace,
// SystemProperties, VMRuntime, NativeAllocationRegistry, RuntimeEnvironment): the parts of ART and
// Robolectric the graphics classes call into. They go first on the class path.

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.management.ManagementFactory;
import java.util.Arrays;
import java.util.Map;
import java.util.zip.CRC32;
import java.util.zip.Deflater;

public final class AospGfx {
    static final String HEBREW = "שלום";  // shalom
    static final String HELLO = "Hello Skia " + HEBREW;
    static int failures;

    static native int[] agentCounts();     // redirect_agent.c
    static native boolean agentRestore();

    static void check(boolean ok, String what) {
        System.out.println((ok ? "[PASS] " : "[FAIL] ") + what);
        if (!ok) failures++;
    }

    static double ms(long t0) { return (System.nanoTime() - t0) / 1e6; }

    public static void main(String[] a) throws Exception {
        if (a.length < 5) {
            System.err.println("usage: AospGfx <agent> <native runtime> <icu .dat> <fonts dir> <out dir> [bench W H warmup frames]");
            System.exit(2);
        }
        System.out.printf("[info] java %s (%s) on %s %s%n", System.getProperty("java.version"),
                System.getProperty("java.vm.name"), System.getProperty("os.name"), System.getProperty("os.arch"));
        long rss0 = rssKb();
        System.out.printf("[info] RSS before the libraries: %d KB%n", rss0);
        long t0 = System.nanoTime();
        System.load(new File(a[0]).getAbsolutePath());
        System.out.printf("[time] agent loaded in %.1f ms%n", ms(t0));

        // The library's JNI_OnLoad reads these, then registers every native it has.
        System.setProperty("icu.data.path", new File(a[2]).getAbsolutePath());
        System.setProperty("icu.locale.default", "en-US");
        t0 = System.nanoTime();
        try {
            System.load(new File(a[1]).getAbsolutePath());
        } catch (Throwable t) {
            int[] c = agentCounts();
            System.out.printf("[agent] table installed=%d bound=%d adapted=%d skipped=%d classes missing=%d%n",
                    c[0], c[1], c[2], c[3], c[4]);
            throw t;
        }
        double loadMs = ms(t0);
        int[] c = agentCounts();
        System.out.printf("[time] native runtime loaded in %.1f ms%s%n", loadMs, rssGrowth(rss0));
        System.out.printf("[agent] table installed=%d bound=%d adapted=%d skipped=%d classes missing=%d%n",
                c[0], c[1], c[2], c[3], c[4]);
        check(c[0] == 1, "the redirect agent swapped the JNI function table");
        // About 860 bind (most of API 34's android.graphics); far fewer means the redirect broke.
        check(c[1] + c[2] > 600, "natives bound onto AOSP classes: " + (c[1] + c[2]));
        check(c[2] == 4, "the four API 34 signature adapters used: " + c[2]);
        check(agentRestore(), "the original JNI function table is back");

        long rss1 = rssKb();
        System.out.println("[info] (Typeface's static initializer looks for /system/etc/fonts.xml first: the"
                + " SystemFonts warning about it is expected)");
        t0 = System.nanoTime();
        int typefaces = setUpFonts(new File(a[3]));
        System.out.printf("[time] system fonts set up in %.1f ms%s%n", ms(t0), rssGrowth(rss1));
        check(typefaces > 10, "system typefaces from fonts.xml: " + typefaces);

        File out = new File(a[4]);
        out.mkdirs();
        t0 = System.nanoTime();
        drawAndCheck(out);
        System.out.printf("[time] the test picture, its checks and the PNG in %.1f ms%n", ms(t0));
        textLayout();

        if (a.length >= 10 && a[5].equals("bench")) {
            bench(Integer.parseInt(a[6]), Integer.parseInt(a[7]), Integer.parseInt(a[8]), Integer.parseInt(a[9]),
                    new File(out, "aospgfx-bench.png"));
        }
        System.out.printf("[info] RSS at the end: %d KB%n", rssKb());
        System.out.println(failures == 0 ? "AOSPGFX OK" : "AOSPGFX FAILED: " + failures + " check(s)");
        System.out.flush();
        System.exit(failures == 0 ? 0 : 1);
    }

    // SystemFonts reads /system/etc/fonts.xml; read the runtime's own fonts.xml from its folder instead
    // (as Robolectric's ShadowSystemFonts does). Milestone 2 would give Win.apk's runtime the same call.
    static int setUpFonts(File dir) throws Exception {
        String d = dir.getAbsolutePath().replace('\\', '/') + "/";
        java.lang.reflect.Method m = android.graphics.fonts.SystemFonts.class.getDeclaredMethod(
                "getSystemFontConfigInternal", String.class, String.class, String.class, String.class, Map.class,
                long.class, int.class);
        m.setAccessible(true);
        android.text.FontConfig cfg = (android.text.FontConfig) m.invoke(null, d + "fonts.xml", d,
                "/nonexistent.xml", "/nonexistent/", new java.util.HashMap<>(), 0L, 0);
        Map<String, android.graphics.fonts.FontFamily[]> fallback = android.graphics.fonts.SystemFonts.buildSystemFallback(cfg);
        Map<String, Typeface> map = android.graphics.fonts.SystemFonts.buildSystemTypefaces(cfg, fallback);
        Typeface.setSystemFontMap(map);
        return map.size();
    }

    static int gray(int argb) { return (((argb >> 16) & 255) + ((argb >> 8) & 255) + (argb & 255)) / 3; }

    static boolean near(int argb, int want, int tol) {
        for (int s = 0; s <= 24; s += 8) {
            if (Math.abs(((argb >>> s) & 255) - ((want >>> s) & 255)) > tol) return false;
        }
        return true;
    }

    /** Pixels in [l,t,r,b) darker than mid gray. */
    static int dark(Bitmap b, int l, int t, int r, int bt) {
        int n = 0;
        for (int y = t; y < bt; y++) {
            for (int x = l; x < r; x++) {
                if (gray(b.getPixel(x, y)) < 128) n++;
            }
        }
        return n;
    }

    static String hex(int v) { return String.format("%08x", v); }

    static void drawAndCheck(File out) throws Exception {
        final int w = 480, h = 260;
        Bitmap b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        Canvas cv = new Canvas(b);
        cv.drawColor(0xffffffff);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        p.setColor(0xffe53935);
        cv.drawRect(10, 10, 110, 70, p);
        int px = b.getPixel(60, 40);
        check(px == 0xffe53935, "drawRect fills with the paint's colour: " + hex(px));
        check(b.getPixel(5, 5) == 0xffffffff, "outside the rect stays white: " + hex(b.getPixel(5, 5)));

        p.setShader(new LinearGradient(130, 0, 330, 0, 0xff1e88e5, 0xff43a047, Shader.TileMode.CLAMP));
        cv.drawRect(130, 10, 330, 70, p);
        p.setShader(null);
        int gl = b.getPixel(131, 40), gr = b.getPixel(328, 40), gm = b.getPixel(230, 40);
        check(near(gl, 0xff1e88e5, 6) && near(gr, 0xff43a047, 6),
                "the gradient runs from its first colour to its second: " + hex(gl) + " .. " + hex(gr));
        check(!near(gm, 0xff1e88e5, 20) && !near(gm, 0xff43a047, 20), "and blends in between: " + hex(gm));

        // A cubic from (350,70) to (470,70) through controls (350,0) and (470,0): its midpoint is (410,17.5).
        Path path = new Path();
        path.moveTo(350, 70);
        path.cubicTo(350, 0, 470, 0, 470, 70);
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(6);
        p.setColor(0xff8e24aa);
        cv.drawPath(path, p);
        p.setStyle(Paint.Style.FILL);
        int pm = b.getPixel(410, 17), inside = b.getPixel(410, 50);
        check(near(pm, 0xff8e24aa, 40), "the stroked path passes through the curve's midpoint: " + hex(pm));
        check(inside == 0xffffffff, "and leaves its inside unfilled: " + hex(inside));

        // Text: Roboto for the Latin, the Hebrew from fonts.xml's fallback (Noto Sans Hebrew), shaped by
        // HarfBuzz, ordered right to left by minikin's bidi.
        p.setColor(0xff000000);
        p.setTextSize(28);
        p.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
        check(p.hasGlyph(HEBREW.substring(0, 1)), "a font for Hebrew is in the fallback chain");
        float full = p.measureText(HELLO), latin = p.measureText("Hello Skia "), heb = p.measureText(HEBREW);
        System.out.printf("[info] advance of \"%s\" = %.1f px (Latin part %.1f, Hebrew part %.1f)%n",
                "Hello Skia <shalom>", full, latin, heb);
        check(latin > 100 && heb > 30 && Math.abs(full - latin - heb) < 2, "text measures: the parts add up");
        cv.drawText(HELLO, 10, 115, p);
        int inkLatin = dark(b, 10, 90, (int) (10 + latin) - 4, 122);
        int inkHebrew = dark(b, (int) (10 + latin), 90, (int) (10 + full), 122);
        check(inkLatin > 150 && inkHebrew > 60, "drawText inks both runs: " + inkLatin + " and " + inkHebrew + " dark px");

        // A PNG this test encodes by hand (not with Skia): 8x8, four quadrants. libpng decodes it.
        byte[] png = quadrantPng();
        Bitmap q = BitmapFactory.decodeByteArray(png, 0, png.length);
        check(q != null && q.getWidth() == 8 && q.getHeight() == 8, "BitmapFactory decodes a PNG");
        if (q != null) {
            check(q.getPixel(1, 1) == 0xffff0000 && q.getPixel(6, 1) == 0xff00ff00 && q.getPixel(1, 6) == 0xff0000ff
                            && q.getPixel(6, 6) == 0xffffff00,
                    "the decoded PNG's pixels: " + hex(q.getPixel(1, 1)) + " " + hex(q.getPixel(6, 1)) + " "
                            + hex(q.getPixel(1, 6)) + " " + hex(q.getPixel(6, 6)));
            cv.drawBitmap(q, new Rect(0, 0, 8, 8), new Rect(10, 140, 90, 220), null);  // scaled 10x, filtered off
            check(b.getPixel(20, 150) == 0xffff0000 && b.getPixel(80, 210) == 0xffffff00,
                    "drawBitmap draws it scaled: " + hex(b.getPixel(20, 150)) + " " + hex(b.getPixel(80, 210)));
        }

        // A circle filled with a BitmapShader (the API 34 adapter), and a rounded rect in a layer.
        Bitmap av = Bitmap.createBitmap(avatarPixels(), 256, 256, Bitmap.Config.ARGB_8888);
        Paint sp = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        BitmapShader bs = new BitmapShader(av, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP);
        android.graphics.Matrix mx = new android.graphics.Matrix();
        mx.setScale(80 / 256f, 80 / 256f);
        mx.postTranslate(110, 140);
        bs.setLocalMatrix(mx);
        sp.setShader(bs);
        cv.drawCircle(150, 180, 40, sp);
        int face = b.getPixel(150, 172);
        check(near(face, 0xfffadcbe, 12), "a BitmapShader fills a circle with the bitmap: " + hex(face));
        cv.saveLayerAlpha(220, 140, 470, 250, 128);
        p.setColor(0xff000000);
        cv.drawRoundRect(new RectF(220, 140, 470, 250), 24, 24, p);
        cv.restore();
        int half = b.getPixel(345, 195), corner = b.getPixel(221, 141);
        check(gray(half) > 110 && gray(half) < 145, "saveLayerAlpha(128) composites at half: " + hex(half));
        check(corner == 0xffffffff, "drawRoundRect leaves its corners: " + hex(corner));

        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        check(b.compress(Bitmap.CompressFormat.PNG, 100, bo), "Bitmap.compress writes a PNG");
        File f = new File(out, "aospgfx.png");
        try (FileOutputStream o = new FileOutputStream(f)) {
            o.write(bo.toByteArray());
        }
        System.out.println("[info] wrote " + f + " (" + bo.size() + " bytes)");
        Bitmap back = BitmapFactory.decodeFile(f.getAbsolutePath());
        boolean same = back != null && back.getWidth() == w && back.getHeight() == h;
        if (same) {
            int[] p1 = new int[w * h], p2 = new int[w * h];
            b.getPixels(p1, 0, w, 0, 0, w, h);
            back.getPixels(p2, 0, w, 0, 0, w, h);
            same = Arrays.equals(p1, p2);
        }
        check(same, "the PNG reads back pixel for pixel");
    }

    // StaticLayout: minikin's line breaker and Android's bidi on a mixed line, in a left-to-right
    // paragraph. The Hebrew run is laid out right to left: its first letter sits at its right end.
    static void textLayout() {
        TextPaint tp = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        tp.setTextSize(28);
        long t0 = System.nanoTime();
        StaticLayout l = StaticLayout.Builder.obtain(HELLO, 0, HELLO.length(), tp, 1000)
                .setAlignment(Layout.Alignment.ALIGN_NORMAL).build();
        System.out.printf("[time] StaticLayout of the line in %.2f ms%n", ms(t0));
        check(l.getLineCount() == 1, "StaticLayout keeps the line whole at 1000 px: " + l.getLineCount() + " line(s)");
        int i = HELLO.indexOf(HEBREW);
        float x1 = l.getPrimaryHorizontal(i + 1), x2 = l.getPrimaryHorizontal(i + 2), x3 = l.getPrimaryHorizontal(i + 3);
        System.out.printf("[info] Hebrew carets at %.1f, %.1f, %.1f px; paragraph direction %d%n", x1, x2, x3,
                l.getParagraphDirection(0));
        check(x1 > x2 && x2 > x3, "the Hebrew run goes right to left");
        check(l.getParagraphDirection(0) == Layout.DIR_LEFT_TO_RIGHT, "in a left-to-right paragraph");

        String rtl = HEBREW + " 123 abc";
        StaticLayout r = StaticLayout.Builder.obtain(rtl, 0, rtl.length(), tp, 1000).build();
        check(r.getParagraphDirection(0) == Layout.DIR_RIGHT_TO_LEFT, "a Hebrew-first paragraph is right to left");
    }

    // ---- an 8x8 PNG, written with java.util.zip: red, green / blue, yellow quadrants ----
    static byte[] quadrantPng() throws Exception {
        ByteArrayOutputStream raw = new ByteArrayOutputStream();
        for (int y = 0; y < 8; y++) {
            raw.write(0);  // filter: none
            for (int x = 0; x < 8; x++) {
                int rgb = y < 4 ? (x < 4 ? 0xff0000 : 0x00ff00) : (x < 4 ? 0x0000ff : 0xffff00);
                raw.write(rgb >> 16);
                raw.write((rgb >> 8) & 255);
                raw.write(rgb & 255);
            }
        }
        Deflater d = new Deflater();
        d.setInput(raw.toByteArray());
        d.finish();
        byte[] buf = new byte[1024];
        int n = d.deflate(buf);
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        o.write(new byte[] {(byte) 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'});
        chunk(o, "IHDR", new byte[] {0, 0, 0, 8, 0, 0, 0, 8, 8, 2, 0, 0, 0});  // 8-bit RGB
        chunk(o, "IDAT", Arrays.copyOf(buf, n));
        chunk(o, "IEND", new byte[0]);
        return o.toByteArray();
    }

    static void chunk(ByteArrayOutputStream o, String type, byte[] data) {
        int len = data.length;
        o.write(len >>> 24);
        o.write(len >>> 16);
        o.write(len >>> 8);
        o.write(len);
        byte[] t = type.getBytes(java.nio.charset.StandardCharsets.US_ASCII);
        o.write(t, 0, 4);
        o.write(data, 0, len);
        CRC32 crc = new CRC32();
        crc.update(t);
        crc.update(data);
        long v = crc.getValue();
        o.write((int) (v >>> 24));
        o.write((int) (v >>> 16));
        o.write((int) (v >>> 8));
        o.write((int) v);
    }

    /** The benchmark's avatar: 256x256, opaque, a face and shoulders on a gradient. */
    static int[] avatarPixels() {
        int n = 256;
        int[] px = new int[n * n];
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                int r = (x * 255) / n, g = (y * 255) / n, b = 160;
                double dx = x - 128, dy = y - 100;
                if (dx * dx + dy * dy < 50 * 50) { r = 250; g = 220; b = 190; }
                if (y > 170 && Math.abs(dx) < (y - 150)) { r = 40; g = 90; b = 200; }
                px[y * n + x] = 0xff000000 | (r << 16) | (g << 8) | b;
            }
        }
        return px;
    }

    // ---- the benchmark frame (the Skia benchmark's AWT scene, call for call), on AOSP's Canvas ----
    static final int ROWS = 40;

    static void bench(int w, int h, int warm, int n, File png) throws Exception {
        int cols = w > h ? 2 : 1, rowsPerCol = ROWS / cols;
        float headerH = h * 0.09f, rowH = (h - headerH) / rowsPerCol, colW = (float) w / cols;
        float avatarD = rowH * 0.72f, t1 = rowH * 0.30f, t2 = rowH * 0.24f, pad = rowH * 0.12f;
        String title = "Inbox  תיבת דואר";
        String sheet = "Undo archive? לבטל העברה לארכיון?";
        String[] line1 = new String[ROWS], line2 = new String[ROWS];
        for (int i = 0; i < ROWS; i++) {
            line1[i] = "Contact number " + (i + 1) + " — Quarterly report draft";
            line2[i] = (i % 2 == 0)
                    ? "שלום, זוהי הודעה חדשה מספר " + (i + 1)
                    : "Meeting at 10:" + (10 + i) + " — פגישה במשרד הראשי";
        }
        Bitmap frame = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        Canvas cv = new Canvas(frame);
        Bitmap avatar = Bitmap.createBitmap(avatarPixels(), 256, 256, Bitmap.Config.ARGB_8888);
        Typeface tf = Typeface.create("sans-serif", Typeface.NORMAL);
        Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTypeface(tf);
        Paint av = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        BitmapShader avShader = new BitmapShader(avatar, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP);
        android.graphics.Matrix avM = new android.graphics.Matrix();
        av.setShader(avShader);
        LinearGradient header = new LinearGradient(0, 0, w, headerH, 0xff6200ee, 0xff03dac5, Shader.TileMode.CLAMP);
        float iconS = rowH * 0.36f, s = iconS;
        Path heart = new Path();
        heart.moveTo(.5f * s, .9f * s);
        heart.cubicTo(.1f * s, .6f * s, 0, .3f * s, .25f * s, .15f * s);
        heart.cubicTo(.4f * s, .07f * s, .5f * s, .2f * s, .5f * s, .25f * s);
        heart.cubicTo(.5f * s, .2f * s, .6f * s, .07f * s, .75f * s, .15f * s);
        heart.cubicTo(1f * s, .3f * s, .9f * s, .6f * s, .5f * s, .9f * s);
        heart.close();
        int layerY = (int) (h * 0.72f);
        RectF rr = new RectF();

        Runnable draw = () -> {
            cv.drawColor(0xffffffff, android.graphics.PorterDuff.Mode.SRC);
            fill.setShader(header);
            cv.drawRect(0, 0, w, headerH, fill);
            fill.setShader(null);
            text.setTextSize(headerH * 0.32f);
            text.setColor(0xffffffff);
            cv.drawText(title, pad * 3, headerH * 0.62f, text);
            for (int i = 0; i < ROWS; i++) {
                float x0 = (i / rowsPerCol) * colW, y0 = headerH + (i % rowsPerCol) * rowH;
                float tx = x0 + pad * 3 + avatarD;
                float r = rowH * 0.18f;
                fill.setColor(0xfff3f0f7);
                rr.set(x0 + pad, y0 + pad * 0.5f, x0 + colW - pad, y0 + rowH - pad * 0.5f);
                cv.drawRoundRect(rr, r, r, fill);
                float ax = x0 + pad * 2, ay = y0 + (rowH - avatarD) / 2;
                avM.setScale(avatarD / 256f, avatarD / 256f);
                avM.postTranslate(ax, ay);
                avShader.setLocalMatrix(avM);
                cv.drawCircle(ax + avatarD / 2, ay + avatarD / 2, avatarD / 2, av);
                text.setTextSize(t1);
                text.setColor(0xff1c1b1f);
                cv.drawText(line1[i], tx, y0 + rowH * 0.45f, text);
                text.setTextSize(t2);
                text.setColor(0xff605d66);
                cv.drawText(line2[i], tx, y0 + rowH * 0.80f, text);
                cv.save();
                cv.translate(x0 + colW - pad * 2 - iconS, y0 + (rowH - iconS) / 2);
                fill.setColor(0xffe91e63);
                cv.drawPath(heart, fill);
                cv.restore();
                fill.setColor(0x1f000000);
                cv.drawRect(tx, y0 + rowH - 1, x0 + colW - pad, y0 + rowH, fill);
            }
            cv.saveLayerAlpha(0, layerY, w, h, 192);
            float r = rowH * 0.4f;
            fill.setColor(0xff2b2930);
            rr.set(0, layerY, w, h + 2 * r);
            cv.drawRoundRect(rr, r, r, fill);
            fill.setColor(0xffd0bcff);
            rr.set(w - pad * 4 - colW * 0.3f, layerY + rowH * 1.6f, w - pad * 4, layerY + rowH * 2.5f);
            cv.drawRoundRect(rr, rowH * 0.45f, rowH * 0.45f, fill);
            text.setTextSize(t1 * 1.2f);
            text.setColor(0xffffffff);
            cv.drawText(sheet, pad * 3, layerY + rowH * 1.1f, text);
            cv.restore();
        };

        long t0 = System.nanoTime();
        draw.run();
        System.out.printf("[time] bench: first frame %.1f ms%n", ms(t0));
        for (int i = 1; i < warm; i++) draw.run();
        com.sun.management.ThreadMXBean tmx = (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();
        System.gc();
        long rss0 = rssKb(), alloc0 = tmx.getCurrentThreadAllocatedBytes(), cpu0 = tmx.getCurrentThreadCpuTime();
        long[] t = new long[n];
        for (int i = 0; i < n; i++) {
            long f0 = System.nanoTime();
            draw.run();
            t[i] = System.nanoTime() - f0;
        }
        long cpu = tmx.getCurrentThreadCpuTime() - cpu0, alloc = tmx.getCurrentThreadAllocatedBytes() - alloc0;
        long rss1 = rssKb();
        long gcCount = 0, gcMs = 0;
        for (var gc : ManagementFactory.getGarbageCollectorMXBeans()) {
            gcCount += gc.getCollectionCount();
            gcMs += gc.getCollectionTime();
        }
        Arrays.sort(t);
        System.out.printf("RESULT variant=aosp-canvas size=%dx%d mode=full median_ms=%.2f p90_ms=%.2f min_ms=%.2f "
                        + "threadcpu_ms_per_frame=%.2f alloc_kb_per_frame=%.1f rss_kb=%d rss_growth_kb=%d gc=%d gc_ms=%d%n",
                w, h, t[n / 2] / 1e6, t[(int) (n * 0.9)] / 1e6, t[0] / 1e6, cpu / 1e6 / n, alloc / 1024.0 / n, rss1,
                rss1 - rss0, gcCount, gcMs);
        try (FileOutputStream o = new FileOutputStream(png)) {
            frame.compress(Bitmap.CompressFormat.PNG, 100, o);
        }
        System.out.println("[info] wrote " + png);
    }

    static String rssGrowth(long before) {
        long now = rssKb();
        return before < 0 || now < 0 ? "" : " (RSS +" + (now - before) + " KB)";
    }

    // Resident set in KB from /proc on Linux; -1 elsewhere (on Windows the CI log shows times only;
    // under Wine /proc would be the Linux side's).
    static long rssKb() {
        if (!System.getProperty("os.name").startsWith("Linux")) return -1;
        try {
            for (String line : java.nio.file.Files.readAllLines(java.nio.file.Path.of("/proc/self/status"))) {
                if (line.startsWith("VmRSS:")) return Long.parseLong(line.replaceAll("[^0-9]", ""));
            }
        } catch (Exception ignored) {
        }
        return -1;
    }
}
