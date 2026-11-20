// The test's stand-in for Win.apk's NativeImageCodec (windows/host/native_image.cpp: WIC on Windows),
// same contract: [width, height, 0xAARRGGBB pixels row by row], or null. It decodes with libheif's
// heif-convert (libheif-examples and its plugins), which applies the file's clap and irot, as a decoder
// that turns the picture would. -Dheiftest.codec=stored turns the picture back first (a decoder that
// leaves the turn to the app, as Windows' HEIF decoder is documented to: these fixtures have at most
// irot 270); -Dheiftest.codec=none answers null for everything (Windows without the extension).
package com.macapk.loader;

import java.awt.image.BufferedImage;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import javax.imageio.ImageIO;

public final class NativeImageCodec {
    private NativeImageCodec() {}

    public static int calls;

    public static int[] decode(byte[] data) {
        calls++;
        String mode = System.getProperty("heiftest.codec", "turned");
        if (mode.equals("none")) return null;
        try {
            File in = File.createTempFile("heiftest", ".heif"), out = File.createTempFile("heiftest", ".png");
            try {
                Files.write(in.toPath(), data);
                Process p = new ProcessBuilder("heif-convert", "--quiet", in.getPath(), out.getPath())
                        .redirectErrorStream(true).redirectOutput(ProcessBuilder.Redirect.DISCARD).start();
                if (p.waitFor() != 0) return null;
                BufferedImage img = ImageIO.read(out);
                if (img == null) return null;
                int w = img.getWidth(), h = img.getHeight();
                int[] px = img.getRGB(0, 0, w, h, null, 0, w);
                if (mode.equals("stored") && new String(data, StandardCharsets.ISO_8859_1).contains("irot")) {
                    // Undo irot 270 (a quarter clockwise): a quarter anticlockwise.
                    int[] back = new int[w * h];
                    for (int y = 0; y < h; y++)
                        for (int x = 0; x < w; x++) back[(w - 1 - x) * h + y] = px[y * w + x];
                    px = back;
                    int t = w; w = h; h = t;
                }
                int[] r = new int[2 + w * h];
                r[0] = w;
                r[1] = h;
                System.arraycopy(px, 0, r, 2, w * h);
                return r;
            } finally {
                in.delete();
                out.delete();
            }
        } catch (Exception e) {
            return null;
        }
    }
}
