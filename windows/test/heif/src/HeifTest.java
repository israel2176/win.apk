// HeifTest -- HEIF and AVIF pictures on AOSP's graphics (windows/java/aosp_gfx's HeifContainer and
// HostImageDecode; windows/README.md, "Milestone 2"). run.sh builds and runs it.
//
//   HeifTest <fixtures dir>                                         the boxes and the pixel transforms
//   HeifTest <fixtures dir> <agent> <native runtime> <icudt68l.dat> and AOSP's BitmapFactory and
//                                                                   ImageDecoder through them
//
// The second form runs on android-all 14 and Robolectric's native runtime as test/aospgfx does, with
// BitmapFactory's and ImageDecoder's natives redirected to HostImageDecode as build_aosp_gfx.sh does,
// and the test's NativeImageCodec (libheif's heif-convert) for the host's WIC; -Dheiftest.codec says
// how that one behaves (turned, stored, none). Each check prints [PASS] or [FAIL]; exit 1 when one failed.
//
// The fixtures (made for this test with libheif's heif-enc from a generated picture, no one's work):
// quad.png, 64x48, red / green above blue / white; quad.heic, it in HEVC (ispe 64x64 and a clap to
// 64x48); quad6.heic, the same with irot 270 (a JPEG with EXIF orientation 6 encoded: shown 48x64);
// quad.avif, it in AV1.

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.ImageDecoder;

import com.winapk.aospgfx.HeifContainer;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FilterInputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.List;

public final class HeifTest {
    static int failures;

    static void check(boolean ok, String what) {
        System.out.println((ok ? "[PASS] " : "[FAIL] ") + what);
        if (!ok) failures++;
    }

    static final int RED = 0xffff0000, GREEN = 0xff00ff00, BLUE = 0xff0000ff, WHITE = 0xffffffff;

    public static void main(String[] a) throws Exception {
        File dir = new File(a[0]);
        byte[] heic = Files.readAllBytes(new File(dir, "quad.heic").toPath());
        byte[] heic6 = Files.readAllBytes(new File(dir, "quad6.heic").toPath());
        byte[] avif = Files.readAllBytes(new File(dir, "quad.avif").toPath());
        byte[] png = Files.readAllBytes(new File(dir, "quad.png").toPath());
        boxes(heic, heic6, avif, png);
        pixels();
        if (a.length >= 4) {
            System.load(new File(a[1]).getAbsolutePath());
            System.setProperty("icu.data.path", new File(a[3]).getAbsolutePath());
            System.setProperty("icu.locale.default", "en-US");
            System.load(new File(a[2]).getAbsolutePath());
            String mode = System.getProperty("heiftest.codec", "turned");
            System.out.println("[info] the host decoder stand-in: " + mode);
            if (mode.equals("none")) noDecoder(heic, png);
            else android(dir, heic, heic6, avif, png);
        }
        System.out.println(failures == 0 ? "HEIFTEST OK" : "HEIFTEST FAILED: " + failures + " check(s)");
        System.exit(failures == 0 ? 0 : 1);
    }

    // ------------------------------------------------------------------ the boxes

    static void boxes(byte[] heic, byte[] heic6, byte[] avif, byte[] png) {
        check(HeifContainer.MIME_HEIF.equals(HeifContainer.sniff(heic, 0, heic.length)), "quad.heic is image/heif");
        check(HeifContainer.MIME_AVIF.equals(HeifContainer.sniff(avif, 0, avif.length)), "quad.avif is image/avif");
        check(HeifContainer.sniff(png, 0, png.length) == null, "a PNG is neither");
        byte[] mp4 = ftyp("isom", "isom", "mp41");
        check(HeifContainer.sniff(mp4, 0, mp4.length) == null, "an MP4 (ftyp isom) is neither");
        byte[] mif1avif = ftyp("mif1", "mif1", "avif", "miaf");
        check(HeifContainer.MIME_AVIF.equals(HeifContainer.sniff(mif1avif, 0, mif1avif.length)),
                "mif1 with a compatible avif is image/avif");
        byte[] mif1heic = ftyp("mif1", "mif1", "heic");
        check(HeifContainer.MIME_HEIF.equals(HeifContainer.sniff(mif1heic, 0, mif1heic.length)),
                "mif1 with a compatible heic is image/heif");
        check(HeifContainer.sniff(heic, 0, 11) == null, "11 bytes tell nothing");
        // Only the head, as a stream's first bytes.
        check(HeifContainer.MIME_HEIF.equals(HeifContainer.sniff(heic, 0, HeifContainer.SNIFF_BYTES)),
                "the first " + HeifContainer.SNIFF_BYTES + " bytes tell");

        HeifContainer.Info i = HeifContainer.parse(heic, 0, heic.length);
        check(i.width == 64 && i.height == 48 && i.transforms.isEmpty(),
                "quad.heic: 64x48 by its clap (ispe 64x64), no transform: " + i.width + "x" + i.height + " "
                        + i.transforms);
        HeifContainer.Info i6 = HeifContainer.parse(heic6, 0, heic6.length);
        check(i6.width == 64 && i6.height == 48 && i6.transforms.size() == 1
                && i6.transforms.get(0).rotateCcw == 270 && i6.quarterTurn(),
                "quad6.heic: stored 64x48, irot 270: " + i6.width + "x" + i6.height + " " + i6.transforms);
        HeifContainer.Info ia = HeifContainer.parse(avif, 0, avif.length);
        check(ia.width == 64 && ia.height == 48 && ia.mime.equals(HeifContainer.MIME_AVIF),
                "quad.avif: 64x48 by its ispe: " + ia.width + "x" + ia.height);
        check(HeifContainer.parse(png, 0, png.length) == null, "a PNG has no HEIF description");
        HeifContainer.Info cut = HeifContainer.parse(heic, 0, 100);
        check(cut != null && cut.width == 0, "a truncated file parses without throwing, its size unknown");
    }

    static byte[] ftyp(String major, String... compatible) {
        ByteBuffer b = ByteBuffer.allocate(16 + 4 * compatible.length);
        b.putInt(b.capacity()).put("ftyp".getBytes()).put(major.getBytes()).putInt(0);
        for (String c : compatible) b.put(c.getBytes());
        return b.array();
    }

    // ------------------------------------------------------------------ the pixel transforms

    static void pixels() {
        int[] px = {1, 2, 3, 4, 5, 6};  // 3x2
        int[] d = {3, 2};
        int[] r = HeifContainer.transform(px, d, List.of(rot(90)));
        check(d[0] == 2 && d[1] == 3 && Arrays.equals(r, new int[] {3, 6, 2, 5, 1, 4}),
                "irot 90 turns a quarter anticlockwise: " + Arrays.toString(r));
        d = new int[] {3, 2};
        r = HeifContainer.transform(px, d, List.of(rot(270)));
        check(d[0] == 2 && d[1] == 3 && Arrays.equals(r, new int[] {4, 1, 5, 2, 6, 3}),
                "irot 270 turns a quarter clockwise: " + Arrays.toString(r));
        d = new int[] {3, 2};
        r = HeifContainer.transform(px, d, List.of(rot(180)));
        check(d[0] == 3 && Arrays.equals(r, new int[] {6, 5, 4, 3, 2, 1}), "irot 180: " + Arrays.toString(r));
        d = new int[] {3, 2};
        r = HeifContainer.transform(px, d, List.of(mir(false)));
        check(Arrays.equals(r, new int[] {3, 2, 1, 6, 5, 4}), "imir axis 0 mirrors left and right");
        d = new int[] {3, 2};
        r = HeifContainer.transform(px, d, List.of(mir(true)));
        check(Arrays.equals(r, new int[] {4, 5, 6, 1, 2, 3}), "imir axis 1 mirrors top and bottom");
        d = new int[] {3, 2};
        List<HeifContainer.Transform> both = List.of(rot(90), mir(false));
        r = HeifContainer.untransform(HeifContainer.transform(px, d, both), d, both);
        check(d[0] == 3 && d[1] == 2 && Arrays.equals(r, px), "untransform undoes transform");
        int[] sq = new int[16];
        for (int k = 0; k < 16; k++) sq[k] = k;
        d = new int[] {4, 4};
        r = HeifContainer.sample(sq, d, 2);
        check(d[0] == 2 && d[1] == 2 && Arrays.equals(r, new int[] {5, 7, 13, 15}),
                "sample size 2 keeps Skia's pixels (from n/2): " + Arrays.toString(r));
        d = new int[] {3, 2};
        r = HeifContainer.sample(px, d, 4);
        check(d[0] == 1 && d[1] == 1, "a sample size over the size gives 1x1");
    }

    static HeifContainer.Transform rot(int deg) { return parseOne("irot", (byte) (deg / 90)); }

    static HeifContainer.Transform mir(boolean topBottom) { return parseOne("imir", (byte) (topBottom ? 1 : 0)); }

    /** A transform as the parser makes it: a minimal file with the one property on item 1. */
    static HeifContainer.Transform parseOne(String type, byte value) {
        ByteBuffer ipco = ByteBuffer.allocate(8 + 9);
        ipco.putInt(17).put("ipco".getBytes()).putInt(9).put(type.getBytes()).put(value);
        ByteBuffer ipma = ByteBuffer.allocate(16 + 2 + 1 + 1);  // version 0: 16-bit item ids, 8-bit indices
        ipma.putInt(20).put("ipma".getBytes()).putInt(0).putInt(1).putShort((short) 1).put((byte) 1)
                .put((byte) 0x81);
        int iprpSize = 8 + 17 + 20;
        int metaSize = 12 + iprpSize;
        ByteBuffer f = ByteBuffer.allocate(20 + metaSize);
        f.putInt(20).put("ftyp".getBytes()).put("heic".getBytes()).putInt(0).put("mif1".getBytes());
        f.putInt(metaSize).put("meta".getBytes()).putInt(0);
        f.putInt(iprpSize).put("iprp".getBytes()).put(ipco.array()).put(ipma.array());
        HeifContainer.Info i = HeifContainer.parse(f.array(), 0, f.capacity());
        if (i.transforms.size() != 1) throw new AssertionError(type + ": " + i.transforms);
        return i.transforms.get(0);
    }

    // ------------------------------------------------------------------ AOSP's decoders

    static boolean near(int argb, int want) {
        for (int s = 0; s <= 24; s += 8) {
            if (Math.abs(((argb >>> s) & 255) - ((want >>> s) & 255)) > 40) return false;
        }
        return true;
    }

    /** The four quadrants' middles, as {top-left, top-right, bottom-left, bottom-right}. */
    static boolean quads(Bitmap b, int tl, int tr, int bl, int br) {
        int w = b.getWidth(), h = b.getHeight();
        return near(b.getPixel(w / 4, h / 4), tl) && near(b.getPixel(3 * w / 4, h / 4), tr)
                && near(b.getPixel(w / 4, 3 * h / 4), bl) && near(b.getPixel(3 * w / 4, 3 * h / 4), br);
    }

    static String desc(Bitmap b) {
        if (b == null) return "null";
        int w = b.getWidth(), h = b.getHeight();
        return w + "x" + h + " " + b.getConfig() + (b.isMutable() ? " mutable" : " immutable") + String.format(
                " [%08x %08x %08x %08x]", b.getPixel(w / 4, h / 4), b.getPixel(3 * w / 4, h / 4),
                b.getPixel(w / 4, 3 * h / 4), b.getPixel(3 * w / 4, 3 * h / 4));
    }

    /** A stream that cannot mark, as a FileInputStream. */
    static InputStream plain(byte[] b) {
        return new FilterInputStream(new ByteArrayInputStream(b)) {
            @Override public boolean markSupported() { return false; }
        };
    }

    static void android(File dir, byte[] heic, byte[] heic6, byte[] avif, byte[] png) throws Exception {
        // Everything else goes through AOSP's natives as before.
        Bitmap p = BitmapFactory.decodeByteArray(png, 0, png.length);
        check(p != null && p.getWidth() == 64 && quads(p, RED, GREEN, BLUE, WHITE), "PNG, byte array: " + desc(p));
        int before = com.macapk.loader.NativeImageCodec.calls;
        BitmapFactory.Options o = new BitmapFactory.Options();
        p = BitmapFactory.decodeStream(plain(png), null, o);
        check(p != null && quads(p, RED, GREEN, BLUE, WHITE) && "image/png".equals(o.outMimeType),
                "PNG, a stream that cannot mark (its head read ahead and put back): " + desc(p));
        InputStream marked = new java.io.BufferedInputStream(new ByteArrayInputStream(png));
        marked.mark(1 << 16);
        o = new BitmapFactory.Options();
        o.inJustDecodeBounds = true;
        BitmapFactory.decodeStream(marked, null, o);
        marked.reset();
        p = BitmapFactory.decodeStream(marked);
        check(o.outWidth == 64 && p != null, "PNG: the app's own mark and reset still work around a decode");
        ImageDecoder.Source src = ImageDecoder.createSource(ByteBuffer.wrap(png));
        p = ImageDecoder.decodeBitmap(src);
        check(p != null && quads(p, RED, GREEN, BLUE, WHITE), "PNG, ImageDecoder: " + desc(p));
        check(com.macapk.loader.NativeImageCodec.calls == before, "the host's decoder never asked for a PNG");

        // HEIF through BitmapFactory: as stored, never turned.
        o = new BitmapFactory.Options();
        Bitmap h = BitmapFactory.decodeByteArray(heic, 0, heic.length, o);
        check(h != null && h.getWidth() == 64 && h.getHeight() == 48 && quads(h, RED, GREEN, BLUE, WHITE),
                "HEIC, byte array: " + desc(h));
        check(h != null && !h.isMutable() && h.getConfig() == Bitmap.Config.ARGB_8888,
                "immutable ARGB_8888 unless asked otherwise");
        check("image/heif".equals(o.outMimeType) && o.outWidth == 64 && o.outHeight == 48,
                "outMimeType " + o.outMimeType + ", out size " + o.outWidth + "x" + o.outHeight);

        o = new BitmapFactory.Options();
        o.inJustDecodeBounds = true;
        before = com.macapk.loader.NativeImageCodec.calls;
        Bitmap none = BitmapFactory.decodeStream(plain(heic), null, o);
        check(none == null && o.outWidth == 64 && o.outHeight == 48 && "image/heif".equals(o.outMimeType),
                "bounds only: " + o.outWidth + "x" + o.outHeight + " " + o.outMimeType);
        check(com.macapk.loader.NativeImageCodec.calls == before, "bounds only read the boxes, decoded nothing");

        o = new BitmapFactory.Options();
        o.inSampleSize = 2;
        o.inMutable = true;
        try (FileInputStream in = new FileInputStream(new File(dir, "quad.heic"))) {
            h = BitmapFactory.decodeStream(in, null, o);
        }
        check(h != null && h.getWidth() == 32 && h.getHeight() == 24 && h.isMutable()
                && quads(h, RED, GREEN, BLUE, WHITE), "HEIC, a file stream, inSampleSize 2, inMutable: " + desc(h));

        o = new BitmapFactory.Options();
        o.inPreferredConfig = Bitmap.Config.RGB_565;
        h = BitmapFactory.decodeByteArray(heic, 0, heic.length, o);
        check(h != null && h.getConfig() == Bitmap.Config.RGB_565 && o.outConfig == Bitmap.Config.RGB_565
                && quads(h, RED, GREEN, BLUE, WHITE), "RGB_565 for an opaque picture: " + desc(h));

        o = new BitmapFactory.Options();
        o.inDensity = 160;
        o.inTargetDensity = 320;
        h = BitmapFactory.decodeByteArray(heic, 0, heic.length, o);
        check(h != null && h.getWidth() == 128 && h.getHeight() == 96 && h.getDensity() == 320,
                "inDensity 160 to inTargetDensity 320 scales it: " + desc(h) + " density " + h.getDensity());

        Bitmap reuse = Bitmap.createBitmap(100, 100, Bitmap.Config.ARGB_8888);
        o = new BitmapFactory.Options();
        o.inBitmap = reuse;
        o.inMutable = true;
        h = BitmapFactory.decodeByteArray(heic, 0, heic.length, o);
        check(h == reuse && h.getWidth() == 64 && h.getHeight() == 48 && quads(h, RED, GREEN, BLUE, WHITE),
                "into inBitmap: " + desc(h));

        o = new BitmapFactory.Options();
        h = BitmapFactory.decodeByteArray(heic6, 0, heic6.length, o);
        check(h != null && h.getWidth() == 64 && h.getHeight() == 48 && quads(h, RED, GREEN, BLUE, WHITE),
                "HEIC with irot 270 through BitmapFactory: as stored, not turned: " + desc(h));

        o = new BitmapFactory.Options();
        Bitmap av = BitmapFactory.decodeByteArray(avif, 0, avif.length, o);
        check(av != null && av.getWidth() == 64 && quads(av, RED, GREEN, BLUE, WHITE)
                && "image/avif".equals(o.outMimeType), "AVIF, byte array: " + desc(av) + " " + o.outMimeType);

        // ImageDecoder: upright.
        Bitmap id = ImageDecoder.decodeBitmap(ImageDecoder.createSource(ByteBuffer.wrap(heic6)));
        check(id != null && id.getWidth() == 48 && id.getHeight() == 64 && quads(id, BLUE, RED, WHITE, GREEN),
                "HEIC with irot 270 through ImageDecoder (a heap ByteBuffer): turned upright: " + desc(id));
        String[] mime = new String[1];
        int[] size = new int[2];
        id = ImageDecoder.decodeBitmap(ImageDecoder.createSource(null, new ByteArrayInputStream(heic6)),
                (decoder, info, s) -> {
                    mime[0] = info.getMimeType();
                    size[0] = info.getSize().getWidth();
                    size[1] = info.getSize().getHeight();
                    decoder.setTargetSize(24, 32);
                    decoder.setMutableRequired(true);
                });
        check("image/heif".equals(mime[0]) && size[0] == 48 && size[1] == 64,
                "the header: " + mime[0] + " " + size[0] + "x" + size[1]);
        check(id != null && id.getWidth() == 24 && id.getHeight() == 32 && id.isMutable()
                && quads(id, BLUE, RED, WHITE, GREEN), "an InputStream source, target size 24x32, mutable: " + desc(id));
        id = ImageDecoder.decodeBitmap(ImageDecoder.createSource(heic, 0, heic.length), (decoder, info, s) -> {
            decoder.setCrop(new android.graphics.Rect(32, 0, 64, 24));
            decoder.setPostProcessor(canvas -> android.graphics.PixelFormat.OPAQUE);
        });
        check(id != null && id.getWidth() == 32 && id.getHeight() == 24 && !id.isMutable()
                && near(id.getPixel(16, 12), GREEN), "a byte array source cropped to the top right: " + desc(id));
        ByteBuffer direct = ByteBuffer.allocateDirect(avif.length).put(avif);
        direct.flip();
        id = ImageDecoder.decodeBitmap(ImageDecoder.createSource(direct));
        check(id != null && id.getWidth() == 64 && quads(id, RED, GREEN, BLUE, WHITE),
                "AVIF through ImageDecoder (a direct ByteBuffer): " + desc(id));
    }

    static void noDecoder(byte[] heic, byte[] png) throws Exception {
        BitmapFactory.Options o = new BitmapFactory.Options();
        Bitmap h = BitmapFactory.decodeByteArray(heic, 0, heic.length, o);
        check(h == null && o.outWidth == -1 && o.outMimeType == null,
                "no host decoder: BitmapFactory gives null, as for any format it cannot read: " + h + " "
                        + o.outWidth + " " + o.outMimeType);
        h = BitmapFactory.decodeStream(plain(heic));
        check(h == null, "and from a stream");
        o = new BitmapFactory.Options();
        o.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(heic, 0, heic.length, o);
        check(o.outWidth == -1, "bounds too, once it is known the host cannot: " + o.outWidth);
        try {
            ImageDecoder.decodeBitmap(ImageDecoder.createSource(ByteBuffer.wrap(heic)));
            check(false, "ImageDecoder threw");
        } catch (ImageDecoder.DecodeException e) {
            check(true, "ImageDecoder throws AOSP's DecodeException: " + e.getMessage());
        }
        Bitmap p = BitmapFactory.decodeByteArray(png, 0, png.length);
        check(p != null && p.getWidth() == 64, "a PNG still decodes");
    }
}
