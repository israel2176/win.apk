package com.winapk.aospgfx;

import java.util.ArrayList;
import java.util.List;

/**
 * What HostImageDecode needs to know of a HEIF or AVIF file (ISO/IEC 23008-12, ISOBMFF), read from its
 * boxes without decoding it: whether the bytes are one at all (the ftyp brand), its MIME type as Android
 * names it, the primary image's size (ispe, then clap's clean aperture) and the transforms to show it
 * upright (irot, imir, in the order its ipma lists them). No Android class: the plain-JVM test
 * (windows/test/heif) reads it directly.
 */
public final class HeifContainer {
    private HeifContainer() {}

    /** Bytes read ahead of a stream or a file to tell a HEIF or AVIF file from the rest. */
    public static final int SNIFF_BYTES = 64;

    public static final String MIME_HEIF = "image/heif";
    public static final String MIME_AVIF = "image/avif";

    /** A transform: rotate {@code degrees} anticlockwise (irot), or mirror (imir). */
    public static final class Transform {
        public final int rotateCcw;      // 0, 90, 180, 270 (irot); -1 when a mirror
        public final boolean mirrorTopBottom; // imir: axis 1 flips top and bottom, axis 0 left and right
        Transform(int rotateCcw, boolean mirrorTopBottom) {
            this.rotateCcw = rotateCcw;
            this.mirrorTopBottom = mirrorTopBottom;
        }
        public boolean isMirror() { return rotateCcw < 0; }
        @Override public String toString() {
            return isMirror() ? (mirrorTopBottom ? "imir(top-bottom)" : "imir(left-right)") : "irot(" + rotateCcw + ")";
        }
    }

    /** The primary image as stored (width, height: after clap, before the transforms) and its transforms. */
    public static final class Info {
        public final String mime;
        public final int width, height;  // 0 when the file does not say
        public final List<Transform> transforms;
        Info(String mime, int width, int height, List<Transform> transforms) {
            this.mime = mime;
            this.width = width;
            this.height = height;
            this.transforms = transforms;
        }
        /** Whether the transforms turn the picture a quarter (its width and height trade places). */
        public boolean quarterTurn() {
            int q = 0;
            for (Transform t : transforms) if (!t.isMirror() && (t.rotateCcw == 90 || t.rotateCcw == 270)) q ^= 1;
            return q != 0;
        }
    }

    /**
     * The MIME type Android gives a HEIF or AVIF file (SkHeifCodec::IsSupported's brands, and AVIF's),
     * or null for anything else: an ftyp box first, its major brand one of HEIF's, or mif1 / msf1 (the
     * image file brands) with AVIF's among the compatible ones.
     */
    public static String sniff(byte[] b, int off, int len) {
        if (b == null || len < 12 || off < 0 || off + len > b.length) return null;
        if (b[off + 4] != 'f' || b[off + 5] != 't' || b[off + 6] != 'y' || b[off + 7] != 'p') return null;
        String major = brand(b, off + 8);
        switch (major) {
            case "heic": case "heix": case "hevc": case "hevx": case "heim": case "heis": case "hevm": case "hevs":
                return MIME_HEIF;
            case "avif": case "avis":
                return MIME_AVIF;
            case "mif1": case "msf1":
                break;
            default:
                return null;
        }
        // mif1 / msf1: AVIF when a compatible brand says so and none says HEVC.
        long size = u32(b, off);
        int end = (int) Math.min((long) len, size < 16 ? 16 : size);
        boolean avif = false, hevc = false;
        for (int p = 16; p + 4 <= end; p += 4) {
            String c = brand(b, off + p);
            if (c.equals("avif") || c.equals("avis")) avif = true;
            if (c.startsWith("hei") || c.startsWith("hev")) hevc = true;
        }
        return avif && !hevc ? MIME_AVIF : MIME_HEIF;
    }

    /** The whole file's description, or null when it is no HEIF / AVIF file. Never throws on a bad file. */
    public static Info parse(byte[] b, int off, int len) {
        String mime = sniff(b, off, len);
        if (mime == null) return null;
        int w = 0, h = 0;
        List<Transform> transforms = new ArrayList<>();
        try {
            int end = off + len;
            int meta = findBox(b, off, end, "meta");
            if (meta >= 0) {
                int metaEnd = boxEnd(b, meta, end);
                int kids = meta + headerSize(b, meta) + 4;  // a full box
                long primary = -1;
                int pitm = findBox(b, kids, metaEnd, "pitm");
                if (pitm >= 0) {
                    int p = pitm + headerSize(b, pitm);
                    primary = (b[p] & 0xff) == 0 ? u16(b, p + 4) : u32(b, p + 4);
                }
                int iprp = findBox(b, kids, metaEnd, "iprp");
                if (iprp >= 0) {
                    int iprpEnd = boxEnd(b, iprp, metaEnd);
                    int ipco = findBox(b, iprp + headerSize(b, iprp), iprpEnd, "ipco");
                    List<Integer> props = new ArrayList<>();
                    if (ipco >= 0) {
                        int ipcoEnd = boxEnd(b, ipco, iprpEnd);
                        for (int p = ipco + headerSize(b, ipco); p + 8 <= ipcoEnd; p = boxEnd(b, p, ipcoEnd)) {
                            props.add(p);
                        }
                    }
                    for (int q = iprp + headerSize(b, iprp); q + 8 <= iprpEnd; q = boxEnd(b, q, iprpEnd)) {
                        if (!type(b, q).equals("ipma")) continue;
                        int p = q + headerSize(b, q);
                        int version = b[p] & 0xff;
                        boolean wide = (b[p + 3] & 1) != 0;
                        long entries = u32(b, p + 4);
                        p += 8;
                        for (long e = 0; e < entries; e++) {
                            long item;
                            if (version < 1) { item = u16(b, p); p += 2; } else { item = u32(b, p); p += 4; }
                            int n = b[p++] & 0xff;
                            for (int i = 0; i < n; i++) {
                                int index;
                                if (wide) { index = u16(b, p) & 0x7fff; p += 2; } else { index = b[p++] & 0x7f; }
                                if (primary >= 0 && item != primary) continue;
                                if (index < 1 || index > props.size()) continue;
                                int box = props.get(index - 1);
                                int body = box + headerSize(b, box);
                                switch (type(b, box)) {
                                    case "ispe":
                                        w = (int) u32(b, body + 4);
                                        h = (int) u32(b, body + 8);
                                        break;
                                    case "clap": {
                                        long wn = u32(b, body), wd = u32(b, body + 4);
                                        long hn = u32(b, body + 8), hd = u32(b, body + 12);
                                        if (wd > 0 && hd > 0) {
                                            w = (int) Math.round((double) wn / wd);
                                            h = (int) Math.round((double) hn / hd);
                                        }
                                        break;
                                    }
                                    case "irot":
                                        transforms.add(new Transform((b[body] & 3) * 90, false));
                                        break;
                                    case "imir":
                                        transforms.add(new Transform(-1, (b[body] & 1) != 0));
                                        break;
                                    default:
                                        break;
                                }
                            }
                            if (primary < 0 && n > 0) primary = item;  // no pitm: the first item
                        }
                    }
                }
            }
        } catch (RuntimeException badFile) {
            // A truncated or malformed box: what was read stands; the decoder has the last word.
        }
        if (w <= 0 || h <= 0) w = h = 0;
        return new Info(mime, w, h, transforms);
    }

    /** Pixels after the transforms, in order. {@code dims} is {width, height} in and out. */
    public static int[] transform(int[] px, int[] dims, List<Transform> transforms) {
        for (Transform t : transforms) {
            if (t.isMirror()) px = mirror(px, dims[0], dims[1], t.mirrorTopBottom);
            else if (t.rotateCcw != 0) px = rotateCcw(px, dims, t.rotateCcw);
        }
        return px;
    }

    /** The inverse of {@link #transform}: what a decoder that applied the transforms gives back, as stored. */
    public static int[] untransform(int[] px, int[] dims, List<Transform> transforms) {
        for (int i = transforms.size() - 1; i >= 0; i--) {
            Transform t = transforms.get(i);
            if (t.isMirror()) px = mirror(px, dims[0], dims[1], t.mirrorTopBottom);
            else if (t.rotateCcw != 0) px = rotateCcw(px, dims, 360 - t.rotateCcw);
        }
        return px;
    }

    static int[] mirror(int[] px, int w, int h, boolean topBottom) {
        int[] out = new int[w * h];
        for (int y = 0; y < h; y++) {
            int sy = topBottom ? h - 1 - y : y;
            if (topBottom) {
                System.arraycopy(px, sy * w, out, y * w, w);
            } else {
                for (int x = 0; x < w; x++) out[y * w + x] = px[sy * w + (w - 1 - x)];
            }
        }
        return out;
    }

    static int[] rotateCcw(int[] px, int[] dims, int degrees) {
        int w = dims[0], h = dims[1];
        degrees = ((degrees % 360) + 360) % 360;
        if (degrees == 0) return px;
        int[] out = new int[w * h];
        if (degrees == 180) {
            for (int i = 0, n = w * h; i < n; i++) out[n - 1 - i] = px[i];
            return out;
        }
        // A quarter: the new width is the old height.
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int nx, ny;
                if (degrees == 90) { nx = y; ny = w - 1 - x; }      // anticlockwise
                else { nx = h - 1 - y; ny = x; }                    // 270 anticlockwise = 90 clockwise
                out[ny * h + nx] = px[y * w + x];
            }
        }
        dims[0] = h;
        dims[1] = w;
        return out;
    }

    /**
     * Every {@code n}th pixel each way, as Skia's samplers keep them (get_start_coord: the first at n / 2),
     * floor(w / n) by floor(h / n), at least 1 (SkSampledCodec's get_scaled_dimension).
     */
    public static int[] sample(int[] px, int[] dims, int n) {
        if (n <= 1) return px;
        int w = dims[0], h = dims[1];
        int ow = Math.max(1, w / n), oh = Math.max(1, h / n);
        int x0 = Math.min(n / 2, w - 1), y0 = Math.min(n / 2, h - 1);
        int[] out = new int[ow * oh];
        for (int y = 0; y < oh; y++) {
            int sy = Math.min(y0 + y * n, h - 1);
            for (int x = 0; x < ow; x++) out[y * ow + x] = px[sy * w + Math.min(x0 + x * n, w - 1)];
        }
        dims[0] = ow;
        dims[1] = oh;
        return out;
    }

    // --- boxes ---

    private static int findBox(byte[] b, int from, int end, String type) {
        for (int p = from; p + 8 <= end; p = boxEnd(b, p, end)) {
            if (type(b, p).equals(type)) return p;
        }
        return -1;
    }

    private static int headerSize(byte[] b, int p) { return u32(b, p) == 1 ? 16 : 8; }

    private static int boxEnd(byte[] b, int p, int end) {
        long size = u32(b, p);
        if (size == 1) size = (u32(b, p + 8) << 32) | u32(b, p + 12);
        else if (size == 0) return end;
        if (size < 8) return end;  // malformed: stop here
        return (int) Math.min((long) end, p + size);
    }

    private static String type(byte[] b, int p) { return brand(b, p + 4); }

    private static String brand(byte[] b, int p) {
        return new String(new char[] {(char) (b[p] & 0xff), (char) (b[p + 1] & 0xff), (char) (b[p + 2] & 0xff),
                (char) (b[p + 3] & 0xff)});
    }

    private static long u32(byte[] b, int p) {
        return ((long) (b[p] & 0xff) << 24) | ((b[p + 1] & 0xff) << 16) | ((b[p + 2] & 0xff) << 8) | (b[p + 3] & 0xff);
    }

    private static int u16(byte[] b, int p) { return ((b[p] & 0xff) << 8) | (b[p + 1] & 0xff); }
}
