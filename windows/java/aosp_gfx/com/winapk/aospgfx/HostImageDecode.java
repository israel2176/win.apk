package com.winapk.aospgfx;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.ColorSpace;
import android.graphics.ImageDecoder;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.util.Log;
import android.util.Size;

import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.PushbackInputStream;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.util.Arrays;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

/**
 * HEIF and AVIF pictures on AOSP's graphics. Robolectric's native runtime has no HEIF codec (Android's
 * comes from the device's media stack), so AOSP's BitmapFactory gave null and ImageDecoder threw on a
 * phone's photo; Win.apk's Java2D graphics handed such files to the host's decoder, Windows' WIC (with
 * the HEIF Image Extensions, and the AV1 Video Extension for AVIF). build_aosp_gfx.sh points
 * BitmapFactory's and ImageDecoder's calls to their decoding natives here (AospCallRedirect.java, same
 * name and descriptor), and each goes on to AOSP's own native as before. Only when that fails, and the
 * file is a HEIF or AVIF one by its ftyp box (HeifContainer), are the bytes given to the host's
 * NativeImageCodec (windows/host/native_image.cpp) and a Bitmap built from the pixels it returns, as
 * Android's decoders would have: BitmapFactory's Options (bounds, sample size, density scaling, config,
 * mutability, premultiplication, inBitmap) and not turned upright; ImageDecoder's target size, crop,
 * post-processor and mutability, turned upright by the file's irot / imir as Android's ImageDecoder does.
 * A stream or a file descriptor has its first {@link HeifContainer#SNIFF_BYTES} bytes read ahead to tell
 * (a stream read once cannot be given to a second decoder); a byte array is looked at only after AOSP's
 * decoder failed. When Windows has no decoder for it the result is the one Android gives for a format it
 * cannot read: null from BitmapFactory, AOSP's DecodeException from ImageDecoder.
 */
public final class HostImageDecode {
    private HostImageDecode() {}

    private static final String TAG = "HostImageDecode";

    private static final MethodHandle BF_BYTES, BF_STREAM, BF_FD;
    private static final MethodHandle ID_BYTES, ID_BUFFER, ID_STREAM, ID_FD;
    private static final MethodHandle ID_DECODE, ID_SAMPLED, ID_PADDING, ID_CLOSE, ID_MIME, ID_COLORSPACE;
    private static final MethodHandle ID_NEW, ID_POST_PROCESS, SET_IMMUTABLE;

    static {
        try {
            MethodHandles.Lookup me = MethodHandles.lookup();
            MethodHandles.Lookup bf = MethodHandles.privateLookupIn(BitmapFactory.class, me);
            Class<?> o = BitmapFactory.Options.class;
            BF_BYTES = bf.findStatic(BitmapFactory.class, "nativeDecodeByteArray", MethodType.methodType(
                    Bitmap.class, byte[].class, int.class, int.class, o, long.class, long.class));
            BF_STREAM = bf.findStatic(BitmapFactory.class, "nativeDecodeStream", MethodType.methodType(
                    Bitmap.class, InputStream.class, byte[].class, Rect.class, o, long.class, long.class));
            BF_FD = bf.findStatic(BitmapFactory.class, "nativeDecodeFileDescriptor", MethodType.methodType(
                    Bitmap.class, FileDescriptor.class, Rect.class, o, long.class, long.class));

            MethodHandles.Lookup id = MethodHandles.privateLookupIn(ImageDecoder.class, me);
            Class<?> d = ImageDecoder.class, s = ImageDecoder.Source.class;
            ID_BYTES = id.findStatic(d, "nCreate",
                    MethodType.methodType(d, byte[].class, int.class, int.class, boolean.class, s));
            ID_BUFFER = id.findStatic(d, "nCreate",
                    MethodType.methodType(d, ByteBuffer.class, int.class, int.class, boolean.class, s));
            ID_STREAM = id.findStatic(d, "nCreate",
                    MethodType.methodType(d, InputStream.class, byte[].class, boolean.class, s));
            ID_FD = id.findStatic(d, "nCreate",
                    MethodType.methodType(d, FileDescriptor.class, long.class, boolean.class, s));
            ID_DECODE = id.findStatic(d, "nDecodeBitmap", MethodType.methodType(Bitmap.class, long.class, d,
                    boolean.class, int.class, int.class, Rect.class, boolean.class, int.class, boolean.class,
                    boolean.class, boolean.class, long.class, boolean.class));
            ID_SAMPLED = id.findStatic(d, "nGetSampledSize", MethodType.methodType(Size.class, long.class, int.class));
            ID_PADDING = id.findStatic(d, "nGetPadding", MethodType.methodType(void.class, long.class, Rect.class));
            ID_CLOSE = id.findStatic(d, "nClose", MethodType.methodType(void.class, long.class));
            ID_MIME = id.findStatic(d, "nGetMimeType", MethodType.methodType(String.class, long.class));
            ID_COLORSPACE = id.findStatic(d, "nGetColorSpace", MethodType.methodType(ColorSpace.class, long.class));
            ID_NEW = id.findConstructor(d, MethodType.methodType(void.class, long.class, int.class, int.class,
                    boolean.class, boolean.class));
            ID_POST_PROCESS = id.findVirtual(d, "postProcessAndRelease", MethodType.methodType(int.class, Canvas.class));

            SET_IMMUTABLE = MethodHandles.privateLookupIn(Bitmap.class, me)
                    .findVirtual(Bitmap.class, "setImmutable", MethodType.methodType(void.class));
        } catch (ReflectiveOperationException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    // ---------------------------------------------------------------- BitmapFactory

    public static Bitmap nativeDecodeByteArray(byte[] data, int offset, int length, BitmapFactory.Options opts,
            long inBitmap, long colorSpace) {
        Bitmap bm;
        try {
            bm = (Bitmap) BF_BYTES.invokeExact(data, offset, length, opts, inBitmap, colorSpace);
        } catch (Throwable t) {
            throw unchecked(t);
        }
        if (bm != null || boundsRead(opts)) return bm;
        String mime = HeifContainer.sniff(data, offset, length);
        return mime == null ? null : decodeHost(data, offset, length, mime, null, opts);
    }

    public static Bitmap nativeDecodeStream(InputStream is, byte[] storage, Rect padding, BitmapFactory.Options opts,
            long inBitmap, long colorSpace) {
        byte[] head = new byte[HeifContainer.SNIFF_BYTES];
        int n;
        try {
            n = readUpTo(is, head);
        } catch (IOException e) {
            return null;  // as AOSP's stream adaptor: a read that throws is a decode that failed
        }
        String mime = HeifContainer.sniff(head, 0, n);
        if (mime == null) {
            InputStream rest = is;
            if (n > 0) {
                PushbackInputStream p = new PushbackInputStream(is, n);
                try {
                    p.unread(head, 0, n);
                } catch (IOException cannot) {
                    return null;
                }
                rest = p;
            }
            try {
                return (Bitmap) BF_STREAM.invokeExact(rest, storage, padding, opts, inBitmap, colorSpace);
            } catch (Throwable t) {
                throw unchecked(t);
            }
        }
        byte[] all;
        try {
            all = readRest(head, n, is);
        } catch (IOException e) {
            return null;
        }
        return decodeWhole(all, mime, padding, opts, inBitmap, colorSpace);
    }

    public static Bitmap nativeDecodeFileDescriptor(FileDescriptor fd, Rect padding, BitmapFactory.Options opts,
            long inBitmap, long colorSpace) {
        byte[] all = heifFromFd(fd, -1);
        if (all == null) {
            try {
                return (Bitmap) BF_FD.invokeExact(fd, padding, opts, inBitmap, colorSpace);
            } catch (Throwable t) {
                throw unchecked(t);
            }
        }
        return decodeWhole(all, HeifContainer.sniff(all, 0, all.length), padding, opts, inBitmap, colorSpace);
    }

    /** A whole HEIF / AVIF file: AOSP's decoder first, as for any file, then the host's. */
    private static Bitmap decodeWhole(byte[] all, String mime, Rect padding, BitmapFactory.Options opts,
            long inBitmap, long colorSpace) {
        Bitmap bm;
        try {
            bm = (Bitmap) BF_BYTES.invokeExact(all, 0, all.length, opts, inBitmap, colorSpace);
        } catch (Throwable t) {
            throw unchecked(t);
        }
        if (bm != null || boundsRead(opts)) {
            noPadding(padding);
            return bm;
        }
        return decodeHost(all, 0, all.length, mime, padding, opts);
    }

    /** AOSP's decoder sets outWidth to -1 first thing and to the width once it has read the header. */
    private static boolean boundsRead(BitmapFactory.Options opts) {
        return opts != null && opts.inJustDecodeBounds && opts.outWidth >= 0;
    }

    /** BitmapFactory's decode of a HEIF / AVIF file the host reads: hwui's doDecode (BitmapFactory.cpp). */
    private static Bitmap decodeHost(byte[] data, int off, int len, String mime, Rect padding,
            BitmapFactory.Options opts) {
        HeifContainer.Info info = HeifContainer.parse(data, off, len);
        int sample = opts == null ? 1 : Math.max(1, opts.inSampleSize);
        float scale = 1f;
        if (opts != null && opts.inScaled && opts.inDensity != 0 && opts.inTargetDensity != 0
                && opts.inDensity != opts.inScreenDensity) {
            scale = (float) opts.inTargetDensity / opts.inDensity;
        }
        Bitmap.Config requested = opts == null || opts.inPreferredConfig == null ? Bitmap.Config.ARGB_8888
                : opts.inPreferredConfig;

        // Bounds from the boxes alone, as Android reads them from the header, unless this machine has been
        // seen not to read the format (then nothing, as from a decoder that cannot).
        if (opts != null && opts.inJustDecodeBounds && info.width > 0 && !undecodable(mime)) {
            int[] dims = {info.width, info.height};
            sampledSize(dims, sample);
            setOuts(opts, scaled(dims[0], scale), scaled(dims[1], scale), mime, config(requested, true), null);
            noPadding(padding);
            return null;
        }

        int[] dims = new int[2];
        int[] px = hostDecode(data, off, len, mime, dims);
        if (px == null) return null;
        // As stored, not upright: Android's BitmapFactory leaves the turn to the app (ExifInterface).
        if (decoderTurned(info, dims)) px = HeifContainer.untransform(px, dims, info.transforms);
        px = HeifContainer.sample(px, dims, sample);
        boolean opaque = opaque(px);
        Bitmap.Config cfg = config(requested, opaque);
        int outW = scaled(dims[0], scale), outH = scaled(dims[1], scale);
        if (opts != null) {
            setOuts(opts, outW, outH, mime, cfg, null);
            if (opts.inJustDecodeBounds) {
                noPadding(padding);
                return null;
            }
        }
        boolean premultiplied = opts == null || opts.inPremultiplied;
        Bitmap bm = argb(px, dims[0], dims[1], opaque, premultiplied);
        if (outW != dims[0] || outH != dims[1]) bm = Bitmap.createScaledBitmap(bm, outW, outH, true);
        if (cfg != Bitmap.Config.ARGB_8888) bm = bm.copy(cfg, true);

        Bitmap reuse = opts == null ? null : opts.inBitmap;
        if (reuse != null) {
            // Into the app's bitmap when it holds the pixels (BitmapFactory then throws for null otherwise).
            if (reuse.isRecycled() || !reuse.isMutable() || reuse.getAllocationByteCount() < bm.getByteCount()) {
                return null;
            }
            reuse.reconfigure(outW, outH, cfg);
            reuse.setPremultiplied(premultiplied);
            int[] row = new int[outW * outH];
            bm.getPixels(row, 0, outW, 0, 0, outW, outH);
            reuse.setPixels(row, 0, outW, 0, 0, outW, outH);
            reuse.setHasAlpha(!opaque);
            bm.recycle();
            bm = reuse;
        } else if (opts == null || !opts.inMutable) {
            immutable(bm);
        }
        if (opts != null) opts.outColorSpace = bm.getColorSpace();
        noPadding(padding);
        return bm;
    }

    private static void setOuts(BitmapFactory.Options opts, int w, int h, String mime, Bitmap.Config cfg,
            ColorSpace cs) {
        opts.outWidth = w;
        opts.outHeight = h;
        opts.outMimeType = mime;
        opts.outConfig = cfg;
        opts.outColorSpace = cs != null ? cs : ColorSpace.get(cfg == Bitmap.Config.RGBA_F16
                ? ColorSpace.Named.LINEAR_EXTENDED_SRGB : ColorSpace.Named.SRGB);
    }

    /**
     * The config Android decodes a colour picture to (SkAndroidCodec::computeOutputColorType): RGB_565 when
     * asked for and opaque, RGBA_F16 when asked for, else ARGB_8888 (ALPHA_8 is for grey pictures only;
     * HARDWARE: Win.apk has no GPU bitmaps, so the software one HARDWARE is copied from).
     */
    private static Bitmap.Config config(Bitmap.Config requested, boolean opaque) {
        if (requested == Bitmap.Config.RGB_565 && opaque) return Bitmap.Config.RGB_565;
        if (requested == Bitmap.Config.RGBA_F16) return Bitmap.Config.RGBA_F16;
        return Bitmap.Config.ARGB_8888;
    }

    private static void sampledSize(int[] dims, int sample) {
        if (sample <= 1) return;
        dims[0] = Math.max(1, dims[0] / sample);
        dims[1] = Math.max(1, dims[1] / sample);
    }

    private static int scaled(int v, float scale) { return scale == 1f ? v : (int) (v * scale + 0.5f); }

    private static void noPadding(Rect padding) {
        if (padding != null) padding.set(-1, -1, -1, -1);  // NinePatchPeeker::getPadding with no patch
    }

    // ---------------------------------------------------------------- ImageDecoder

    /** A picture the host decoded, upright, behind a decoder whose native pointer is negative (its key). */
    private static final class Decoded {
        final int[] px;
        final int width, height;
        final boolean opaque;
        final String mime;
        Decoded(int[] px, int width, int height, boolean opaque, String mime) {
            this.px = px;
            this.width = width;
            this.height = height;
            this.opaque = opaque;
            this.mime = mime;
        }
    }

    private static final ConcurrentHashMap<Long, Decoded> DECODED = new ConcurrentHashMap<>();
    private static final AtomicLong NEXT = new AtomicLong();

    public static ImageDecoder nCreate(byte[] data, int offset, int length, boolean preferAnimation,
            ImageDecoder.Source src) throws IOException {
        try {
            return (ImageDecoder) ID_BYTES.invokeExact(data, offset, length, preferAnimation, src);
        } catch (IOException e) {
            ImageDecoder d = hostDecoder(data, offset, length);
            if (d != null) return d;
            throw e;
        } catch (Throwable t) {
            throw unchecked(t);
        }
    }

    public static ImageDecoder nCreate(ByteBuffer buffer, int position, int limit, boolean preferAnimation,
            ImageDecoder.Source src) throws IOException {
        try {
            return (ImageDecoder) ID_BUFFER.invokeExact(buffer, position, limit, preferAnimation, src);
        } catch (IOException e) {
            if (limit - position >= 12) {
                ByteBuffer b = buffer.duplicate();
                b.position(position).limit(limit);
                byte[] head = new byte[Math.min(HeifContainer.SNIFF_BYTES, limit - position)];
                b.duplicate().get(head);
                if (HeifContainer.sniff(head, 0, head.length) != null) {
                    byte[] all = new byte[limit - position];
                    b.get(all);
                    ImageDecoder d = hostDecoder(all, 0, all.length);
                    if (d != null) return d;
                }
            }
            throw e;
        } catch (Throwable t) {
            throw unchecked(t);
        }
    }

    public static ImageDecoder nCreate(InputStream is, byte[] storage, boolean preferAnimation,
            ImageDecoder.Source src) throws IOException {
        byte[] head = new byte[HeifContainer.SNIFF_BYTES];
        int n = readUpTo(is, head);
        if (HeifContainer.sniff(head, 0, n) != null) {
            byte[] all = readRest(head, n, is);
            return nCreate(all, 0, all.length, preferAnimation, src);
        }
        InputStream rest = is;
        if (n > 0) {
            PushbackInputStream p = new PushbackInputStream(is, n);
            p.unread(head, 0, n);
            rest = p;
        }
        try {
            return (ImageDecoder) ID_STREAM.invokeExact(rest, storage, preferAnimation, src);
        } catch (IOException e) {
            throw e;
        } catch (Throwable t) {
            throw unchecked(t);
        }
    }

    public static ImageDecoder nCreate(FileDescriptor fd, long length, boolean preferAnimation,
            ImageDecoder.Source src) throws IOException {
        byte[] all = heifFromFd(fd, length);
        if (all != null) return nCreate(all, 0, all.length, preferAnimation, src);
        try {
            return (ImageDecoder) ID_FD.invokeExact(fd, length, preferAnimation, src);
        } catch (IOException e) {
            throw e;
        } catch (Throwable t) {
            throw unchecked(t);
        }
    }

    /** A decoder over the host's pixels, upright (ImageDecoder honours the file's orientation), or null. */
    private static ImageDecoder hostDecoder(byte[] data, int off, int len) {
        String mime = HeifContainer.sniff(data, off, len);
        if (mime == null) return null;
        HeifContainer.Info info = HeifContainer.parse(data, off, len);
        int[] dims = new int[2];
        int[] px = hostDecode(data, off, len, mime, dims);
        if (px == null) return null;
        if (!decoderTurned(info, dims)) px = HeifContainer.transform(px, dims, info.transforms);
        long key = -NEXT.incrementAndGet();
        DECODED.put(key, new Decoded(px, dims[0], dims[1], opaque(px), mime));
        try {
            // Never animated: the first picture of an image sequence, with no AnimatedImageDrawable over it.
            return (ImageDecoder) ID_NEW.invokeExact(key, dims[0], dims[1], false, false);
        } catch (Throwable t) {
            DECODED.remove(key);
            throw unchecked(t);
        }
    }

    public static Bitmap nDecodeBitmap(long ptr, ImageDecoder decoder, boolean doPostProcess, int width,
            int height, Rect crop, boolean mutable, int allocator, boolean unpremultiplied, boolean conserveMemory,
            boolean asAlphaMask, long colorSpace, boolean extended) throws IOException {
        if (ptr >= 0) {
            try {
                return (Bitmap) ID_DECODE.invokeExact(ptr, decoder, doPostProcess, width, height, crop, mutable,
                        allocator, unpremultiplied, conserveMemory, asAlphaMask, colorSpace, extended);
            } catch (IOException e) {
                throw e;
            } catch (Throwable t) {
                throw unchecked(t);
            }
        }
        Decoded d = DECODED.get(ptr);
        if (d == null) throw new IllegalStateException("Cannot use closed ImageDecoder!");
        // ImageDecoder.cpp's DecodeBitmap: scaled to the target size, then cropped, then post-processed.
        // The allocator is software whatever was asked (no GPU bitmaps here); conserveMemory and
        // asAlphaMask change nothing for a colour picture; the colour space is sRGB's.
        Bitmap bm = argb(d.px, d.width, d.height, d.opaque, !unpremultiplied);
        if (width != d.width || height != d.height) bm = Bitmap.createScaledBitmap(bm, width, height, true);
        if (crop != null) {
            Bitmap c = Bitmap.createBitmap(crop.width(), crop.height(), Bitmap.Config.ARGB_8888);
            c.setPremultiplied(!unpremultiplied);
            int[] row = new int[crop.width() * crop.height()];
            bm.getPixels(row, 0, crop.width(), crop.left, crop.top, crop.width(), crop.height());
            c.setPixels(row, 0, crop.width(), 0, 0, crop.width(), crop.height());
            c.setHasAlpha(!d.opaque);
            bm = c;
        }
        if (doPostProcess) {
            int format;
            try {
                format = (int) ID_POST_PROCESS.invokeExact(decoder, new Canvas(bm));
            } catch (Throwable t) {
                throw unchecked(t);
            }
            if (format == PixelFormat.OPAQUE) bm.setHasAlpha(false);
            else if (format == PixelFormat.TRANSLUCENT) bm.setHasAlpha(true);
        }
        if (!mutable) immutable(bm);
        return bm;
    }

    public static Size nGetSampledSize(long ptr, int sampleSize) {
        if (ptr >= 0) {
            try {
                return (Size) ID_SAMPLED.invokeExact(ptr, sampleSize);
            } catch (Throwable t) {
                throw unchecked(t);
            }
        }
        Decoded d = decoded(ptr);
        int[] dims = {d.width, d.height};
        sampledSize(dims, sampleSize);
        return new Size(dims[0], dims[1]);
    }

    public static void nGetPadding(long ptr, Rect outPadding) {
        if (ptr >= 0) {
            try {
                ID_PADDING.invokeExact(ptr, outPadding);
            } catch (Throwable t) {
                throw unchecked(t);
            }
            return;
        }
        noPadding(outPadding);
    }

    public static void nClose(long ptr) {
        if (ptr >= 0) {
            try {
                ID_CLOSE.invokeExact(ptr);
            } catch (Throwable t) {
                throw unchecked(t);
            }
            return;
        }
        DECODED.remove(ptr);
    }

    public static String nGetMimeType(long ptr) {
        if (ptr >= 0) {
            try {
                return (String) ID_MIME.invokeExact(ptr);
            } catch (Throwable t) {
                throw unchecked(t);
            }
        }
        return decoded(ptr).mime;
    }

    public static ColorSpace nGetColorSpace(long ptr) {
        if (ptr >= 0) {
            try {
                return (ColorSpace) ID_COLORSPACE.invokeExact(ptr);
            } catch (Throwable t) {
                throw unchecked(t);
            }
        }
        decoded(ptr);
        return ColorSpace.get(ColorSpace.Named.SRGB);
    }

    private static Decoded decoded(long ptr) {
        Decoded d = DECODED.get(ptr);
        if (d == null) throw new IllegalStateException("Cannot use closed ImageDecoder!");
        return d;
    }

    // ---------------------------------------------------------------- the host's decoder

    private static volatile MethodHandle codec;
    private static volatile boolean heifFails, avifFails;

    private static boolean undecodable(String mime) {
        return HeifContainer.MIME_AVIF.equals(mime) ? avifFails : heifFails;
    }

    /**
     * The host's decode (com.macapk.loader.NativeImageCodec, Win.apk's framework: WIC), as stored by the
     * file, {@code dims} set to its size; null when Windows has no decoder for it, which is logged once.
     */
    private static int[] hostDecode(byte[] data, int off, int len, String mime, int[] dims) {
        int[] r = null;
        try {
            MethodHandle m = codec;
            if (m == null) {
                Class<?> c = Class.forName("com.macapk.loader.NativeImageCodec");
                codec = m = MethodHandles.publicLookup().findStatic(c, "decode",
                        MethodType.methodType(int[].class, byte[].class));
            }
            byte[] bytes = off == 0 && len == data.length ? data : Arrays.copyOfRange(data, off, off + len);
            r = (int[]) m.invokeExact(bytes);
        } catch (Throwable t) {
            if (t instanceof VirtualMachineError) throw (VirtualMachineError) t;
            Log.w(TAG, "the host's image decoder: " + t);
        }
        if (r == null || r.length < 2 || r[0] <= 0 || r[1] <= 0 || (long) r.length < 2L + (long) r[0] * r[1]) {
            boolean avif = HeifContainer.MIME_AVIF.equals(mime);
            if (!(avif ? avifFails : heifFails)) {
                if (avif) avifFails = true; else heifFails = true;
                Log.w(TAG, (avif ? "AVIF" : "HEIF") + " picture: Windows has no decoder for it (it needs "
                        + (avif ? "the AV1 Video Extension" : "the HEIF Image Extensions")
                        + " from the Microsoft Store), so it decodes as an unsupported format");
            }
            return null;
        }
        if (HeifContainer.MIME_AVIF.equals(mime)) avifFails = false; else heifFails = false;
        dims[0] = r[0];
        dims[1] = r[1];
        return Arrays.copyOfRange(r, 2, 2 + r[0] * r[1]);
    }

    /**
     * Whether the host's decoder already turned the picture by its irot (the file says the stored picture
     * is w x h, and a quarter turn; the decoder gave h x w). Windows' HEIF decoder is documented to leave
     * that to the app (its System.Photo.Orientation); this keeps either right for the quarter turns.
     */
    private static boolean decoderTurned(HeifContainer.Info info, int[] dims) {
        return info.width > 0 && info.width != info.height && info.quarterTurn() && dims[0] == info.height
                && dims[1] == info.width;
    }

    // ---------------------------------------------------------------- helpers

    private static Bitmap argb(int[] px, int w, int h, boolean opaque, boolean premultiplied) {
        Bitmap bm = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        if (!premultiplied) bm.setPremultiplied(false);
        bm.setPixels(px, 0, w, 0, 0, w, h);
        bm.setHasAlpha(!opaque);
        return bm;
    }

    private static boolean opaque(int[] px) {
        for (int p : px) if ((p >>> 24) != 0xff) return false;
        return true;
    }

    private static void immutable(Bitmap bm) {
        try {
            SET_IMMUTABLE.invokeExact(bm);
        } catch (Throwable t) {
            throw unchecked(t);
        }
    }

    /** Reads until {@code buf} is full or the stream ends; the count read. */
    private static int readUpTo(InputStream is, byte[] buf) throws IOException {
        int n = 0;
        while (n < buf.length) {
            int r = is.read(buf, n, buf.length - n);
            if (r < 0) break;
            n += r;
        }
        return n;
    }

    private static byte[] readRest(byte[] head, int n, InputStream is) throws IOException {
        byte[] rest = is.readAllBytes();
        byte[] all = Arrays.copyOf(head, n + rest.length);
        System.arraycopy(rest, 0, all, n, rest.length);
        return all;
    }

    /**
     * A HEIF / AVIF file's bytes from the descriptor's offset ({@code length} of them, or to its end when
     * negative), read without moving the offset; null when the file is something else or cannot be read so.
     */
    private static byte[] heifFromFd(FileDescriptor fd, long length) {
        if (fd == null || !fd.valid()) return null;
        try {
            // Not closed: closing would close the app's descriptor.
            FileChannel ch = new FileInputStream(fd).getChannel();
            long pos = ch.position();
            ByteBuffer head = ByteBuffer.allocate(HeifContainer.SNIFF_BYTES);
            while (head.hasRemaining() && ch.read(head, pos + head.position()) > 0) {}
            if (HeifContainer.sniff(head.array(), 0, head.position()) == null) return null;
            long end = ch.size();
            if (length >= 0) end = Math.min(end, pos + length);
            if (end - pos > Integer.MAX_VALUE - 16) return null;
            ByteBuffer all = ByteBuffer.allocate((int) (end - pos));
            while (all.hasRemaining() && ch.read(all, pos + all.position()) > 0) {}
            return all.position() == all.capacity() ? all.array() : Arrays.copyOf(all.array(), all.position());
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }

    private static RuntimeException unchecked(Throwable t) {
        if (t instanceof RuntimeException) throw (RuntimeException) t;
        if (t instanceof Error) throw (Error) t;
        throw new IllegalStateException(t);
    }
}
