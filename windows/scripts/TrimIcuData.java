// TrimIcuData.java -- rewrites an ICU common data file (icudtNNl.dat, "CmnD") without the items
// nothing in the program reading it can open, as ICU's own `icupkg -r` would. build_aosp_gfx.sh runs
// it on Robolectric's native runtime's ICU data (windows/README.md, "Footprint").
//
//   java TrimIcuData.java <in.dat> <out.dat> <drop>...
//
// A <drop> ending in "/" drops a tree (every item under "<package>/<drop>"); one starting with "*."
// drops the items of that type; anything else is one item's name, without the package prefix.
// Every other item is copied byte for byte, in the same order (ICU finds an item by a binary search
// over the names), each at a 16-byte boundary as icupkg places them. Only little-endian ASCII data
// (an "l" package, the one Windows reads) is handled; anything else is refused.

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

public class TrimIcuData {
    public static void main(String[] a) throws IOException {
        if (a.length < 3) throw new IllegalArgumentException("usage: TrimIcuData <in.dat> <out.dat> <drop>...");
        ByteBuffer in = ByteBuffer.wrap(Files.readAllBytes(Path.of(a[0]))).order(ByteOrder.LITTLE_ENDIAN);
        int headerSize = in.getShort(0) & 0xffff;
        if ((in.get(2) & 0xff) != 0xda || (in.get(3) & 0xff) != 0x27) throw new IOException(a[0] + ": not ICU data");
        // UDataInfo after the 4-byte MappedData: size, reserved, isBigEndian, charsetFamily, ..., dataFormat.
        String format = new String(in.array(), 4 + 8, 4, StandardCharsets.US_ASCII);
        if (in.get(4 + 4) != 0 || in.get(4 + 5) != 0 || !format.equals("CmnD"))
            throw new IOException(a[0] + ": not little-endian ASCII common data (" + format + ")");

        int toc = headerSize, count = in.getInt(toc);
        List<String> names = new ArrayList<>();
        int[] start = new int[count], end = new int[count];
        for (int i = 0; i < count; i++) {
            int nameOff = in.getInt(toc + 4 + 8 * i);
            start[i] = toc + in.getInt(toc + 8 + 8 * i);
            int z = toc + nameOff;
            while (in.get(z) != 0) z++;
            names.add(new String(in.array(), toc + nameOff, z - toc - nameOff, StandardCharsets.US_ASCII));
        }
        for (int i = 0; i < count; i++) end[i] = i + 1 < count ? start[i + 1] : in.capacity();

        List<Integer> keep = new ArrayList<>();
        int[] hits = new int[a.length];
        for (int i = 0; i < count; i++) {
            String n = names.get(i);
            String item = n.substring(n.indexOf('/') + 1);
            boolean drop = false;
            for (int k = 2; k < a.length && !drop; k++) {
                String d = a[k];
                drop = d.endsWith("/") ? item.startsWith(d) : d.startsWith("*.") ? item.endsWith(d.substring(1)) : item.equals(d);
                if (drop) hits[k]++;
            }
            if (!drop) keep.add(i);
        }
        // A rule that drops nothing means other data than the one the rules were written for: look
        // again at what the program reads rather than trim it blind.
        for (int k = 2; k < a.length; k++)
            if (hits[k] == 0) throw new IOException(a[0] + ": nothing is " + a[k] + "; are these the data the rules are for?");
        if (keep.isEmpty()) throw new IOException("nothing left");

        // Header as it was; the table of contents: count, then (name offset, data offset) pairs, both
        // from the table's start; the names; the items, each 16-byte aligned from the file's start.
        int namesAt = 4 + 8 * keep.size(), namesLen = 0;
        for (int i : keep) namesLen += names.get(i).length() + 1;
        int dataAt = align(toc + namesAt + namesLen) - toc, size = toc + dataAt;
        for (int i : keep) size = align(size) + (end[i] - start[i]);
        ByteBuffer out = ByteBuffer.allocate(align(size)).order(ByteOrder.LITTLE_ENDIAN);
        out.put(in.array(), 0, headerSize);
        out.putInt(toc, keep.size());
        int nameAt = namesAt, at = toc + dataAt;
        for (int j = 0; j < keep.size(); j++) {
            int i = keep.get(j);
            byte[] nb = names.get(i).getBytes(StandardCharsets.US_ASCII);
            out.putInt(toc + 4 + 8 * j, nameAt);
            out.putInt(toc + 8 + 8 * j, at - toc);
            out.put(toc + nameAt, nb);
            nameAt += nb.length + 1;
            out.put(at, in.array(), start[i], end[i] - start[i]);
            at = align(at + end[i] - start[i]);
        }
        // An item's length is where the next one starts, so the last one ends at the end of the file
        // (the padding after it, as after the others, is read by no one).
        Files.write(Path.of(a[1]), out.array());
        System.out.printf("[aosp-gfx]   ICU data: %d of %d items kept, %.1f MB -> %.1f MB%n", keep.size(), count,
                in.capacity() / 1e6, out.capacity() / 1e6);
    }

    static int align(int n) { return (n + 15) & ~15; }
}
