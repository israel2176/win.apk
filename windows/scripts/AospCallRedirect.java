// AospCallRedirect.java -- points a static call in one of AOSP's graphics classes (build_aosp_gfx.sh's)
// at a Win.apk method of the same name and descriptor, where AOSP's code relies on something ART does
// and HotSpot does not.
//
//   java AospCallRedirect.java <classes dir> <class> <owner> <name> <descriptor> <new owner>
//
// Every Methodref of <class> (a class file under <classes dir>, "a/b/C$D") to <owner>.<name><descriptor>
// is made to name <new owner> instead: a new CONSTANT_Class (and its name) is added at the end of the
// constant pool and the Methodref's class index set to it, nothing else in the file moves. The class's
// other references to <owner> (its own this_class, when <owner> is the class itself) stay. The count of
// calls redirected is printed; none stops the build (the call it was written for is gone).
//
// The one use (build_aosp_gfx.sh): android.graphics.fonts.Font$Builder reads a font that is compressed
// in the APK (aapt2 compresses .ttf and .otf) into ByteBuffer.allocateDirect(n).array(), which ART's
// direct buffers have and HotSpot's throw UnsupportedOperationException on; its createBuffer calls go to
// windows/java/aosp_gfx's com.winapk.aospgfx.FontBuffers.createBuffer, which reads it the way ART ends up
// with it.

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

public class AospCallRedirect {
    public static void main(String[] a) throws Exception {
        if (a.length != 6) throw new IllegalArgumentException(
                "usage: AospCallRedirect <classes dir> <class> <owner> <name> <descriptor> <new owner>");
        Path file = Path.of(a[0], a[1] + ".class");
        byte[] data = Files.readAllBytes(file);
        ByteBuffer b = ByteBuffer.wrap(data);
        if (b.getInt(0) != 0xCAFEBABE) throw new IOException(file + ": not a class file");
        int n = b.getShort(8) & 0xffff, off = 10;
        String[] utf8 = new String[n];
        int[][] refs = new int[n][];
        int[] at = new int[n];
        for (int i = 1; i < n; i++) {
            at[i] = off;
            int tag = data[off++] & 0xff;
            switch (tag) {
                case 1: { int len = b.getShort(off) & 0xffff;
                          utf8[i] = new String(data, off + 2, len, StandardCharsets.UTF_8); off += 2 + len; break; }
                case 3: case 4: off += 4; break;
                case 5: case 6: off += 8; i++; break;
                case 7: case 8: case 16: case 19: case 20: refs[i] = new int[] {tag, b.getShort(off) & 0xffff}; off += 2; break;
                case 9: case 10: case 11: case 12: case 17: case 18:
                    refs[i] = new int[] {tag, b.getShort(off) & 0xffff, b.getShort(off + 2) & 0xffff}; off += 4; break;
                case 15: off += 3; break;
                default: throw new IOException(file + ": constant pool tag " + tag);
            }
        }
        int poolEnd = off;
        // The new owner: CONSTANT_Utf8 (index n) and CONSTANT_Class naming it (index n + 1).
        byte[] name = a[5].getBytes(StandardCharsets.UTF_8);
        int newClass = n + 1;
        int redirected = 0;
        for (int i = 1; i < n; i++) {
            if (refs[i] == null || refs[i][0] != 10) continue;                // Methodref
            String owner = utf8[refs[refs[i][1]][1]];
            int[] nt = refs[refs[i][2]];
            if (!a[2].equals(owner) || !a[3].equals(utf8[nt[1]]) || !a[4].equals(utf8[nt[2]])) continue;
            b.putShort(at[i] + 1, (short) newClass);
            redirected++;
        }
        if (redirected == 0) {
            throw new IllegalStateException(a[1] + ": no call to " + a[2] + "." + a[3] + a[4]);
        }
        ByteArrayOutputStream out = new ByteArrayOutputStream(data.length + name.length + 6);
        out.write(data, 0, 8);
        out.write((n + 2) >> 8);
        out.write(n + 2);
        out.write(data, 10, poolEnd - 10);
        out.write(1);
        out.write(name.length >> 8);
        out.write(name.length);
        out.write(name);
        out.write(7);
        out.write(n >> 8);
        out.write(n);
        out.write(data, poolEnd, data.length - poolEnd);
        Files.write(file, out.toByteArray());
        System.out.println("[aosp-gfx]   " + a[1] + ": " + redirected + " call(s) to " + a[2] + "." + a[3]
                + " -> " + a[5]);
    }
}
