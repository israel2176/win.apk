// AospResIds.java -- links AOSP's graphics classes (build_aosp_gfx.sh's) to the platform resource
// table Win.apk ships (bin/tools/framework-res.apk), whose private ids are not android-14's.
//
//   java AospResIds.java <android-all.jar> <classes dir> <framework-res.apk> <R.java out>
//
// AOSP's drawables read their XML through com.android.internal.R.styleable's arrays of attribute ids
// (GradientDrawable, VectorDrawablePath...) and javac put the index constants into their code, so
// only the arrays need to exist at run time. This writes R.java with exactly the arrays those classes
// name (their class files' field references), each in android-14's order (the indices are
// compiled in) with every id as the shipped table has it: a public id is the same in both, a
// private one (Lighting's lightRadius...) is looked up by name; 0 when the table has no such
// attribute. android-all's own R is never taken: its private ids name other resources here.
//
// Second, an int constant in those classes that is one of android-14's private resource ids
// (AdaptiveIconDrawable's R.string.config_icon_mask, compiled in as 0x01040252, which is
// config_defaultShutdownVibrationFile in the shipped table) is rewritten in place to the id the same
// type and name have in the shipped table. Each rewrite is printed; one with no counterpart stops the
// build.

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.stream.Stream;
import java.util.zip.ZipFile;

public class AospResIds {
    private static final String STYLEABLE = "com/android/internal/R$styleable";

    public static void main(String[] a) throws Exception {
        if (a.length != 4) throw new IllegalArgumentException(
                "usage: AospResIds <android-all.jar> <classes dir> <framework-res.apk> <R.java out>");
        URLClassLoader all = new URLClassLoader(new URL[] {new File(a[0]).toURI().toURL()}, null);
        Path classes = Path.of(a[1]);
        Map<String, Integer> shipped = shippedIds(Path.of(a[2]));

        // android-14's ids: every resource of the platform by "type/name", and which are public.
        Map<Integer, String> aospPrivate = new HashMap<>();
        Map<Integer, String> aospAttr = new HashMap<>();
        java.util.Set<Integer> aospPublic = new java.util.HashSet<>();
        for (Class<?> c : all.loadClass("android.R").getClasses()) {
            for (Field f : c.getFields()) if (f.getType() == int.class) aospPublic.add(f.getInt(null));
        }
        for (Class<?> c : all.loadClass("com.android.internal.R").getClasses()) {
            String type = c.getSimpleName();
            for (Field f : c.getFields()) {
                if (f.getType() != int.class || !Modifier.isStatic(f.getModifiers())) continue;
                if (type.equals("styleable")) continue;
                int id = f.getInt(null);
                if ((id >>> 24) != 0x01) continue;
                if (type.equals("attr")) aospAttr.put(id, f.getName());
                if (!aospPublic.contains(id)) aospPrivate.put(id, type + "/" + f.getName());
            }
        }

        List<Path> files;
        try (Stream<Path> s = Files.walk(classes)) {
            files = s.filter(p -> p.toString().endsWith(".class")).sorted().toList();
        }
        TreeSet<String> styleables = new TreeSet<>();
        int rewritten = 0;
        for (Path p : files) {
            byte[] b = Files.readAllBytes(p);
            ClassFile cf = new ClassFile(b);
            styleables.addAll(cf.styleableFields);
            boolean changed = false;
            for (Map.Entry<Integer, Integer> e : cf.intConstants.entrySet()) {
                int v = e.getValue();
                String name = aospPrivate.get(v);
                if (name == null || name.startsWith("attr/")) continue;
                Integer now = shipped.get(name);
                if (now == null) throw new IOException(classes.relativize(p) + ": android-14's " + name
                        + String.format(" (0x%08x)", v) + " is not in " + a[2]);
                if (now != v) {
                    ByteBuffer.wrap(b).putInt(e.getKey(), now);
                    changed = true;
                    rewritten++;
                    System.out.printf("[aosp-gfx]   %s: %s 0x%08x -> 0x%08x%n", classes.relativize(p), name, v, now);
                }
            }
            if (changed) Files.write(p, b);
        }

        Class<?> st = all.loadClass("com.android.internal.R$styleable");
        StringBuilder out = new StringBuilder();
        out.append("// Generated by windows/scripts/AospResIds.java from android-all 14's com.android.internal.R:\n")
           .append("// the styleables AOSP's graphics classes read, with the ids of the platform table Win.apk ships.\n")
           .append("package com.android.internal;\n\npublic final class R {\n    private R() {}\n\n")
           .append("    public static final class styleable {\n        private styleable() {}\n");
        int privateAttrs = 0, missing = 0;
        for (String n : styleables) {
            int[] ids = (int[]) st.getField(n).get(null);
            out.append("        public static final int[] ").append(n).append(" = {");
            for (int i = 0; i < ids.length; i++) {
                int id = ids[i];
                String attr = aospAttr.get(id);
                int v = id;
                String note = null;
                if (!aospPublic.contains(id)) {
                    Integer s = attr == null ? null : shipped.get("attr/" + attr);
                    v = s == null ? 0 : s;
                    privateAttrs++;
                    if (s == null) missing++;
                    note = attr == null ? "?" : attr;
                }
                out.append(i == 0 ? "" : ",").append(String.format(" 0x%08x", v));
                if (note != null) out.append(" /* ").append(note).append(" */");
            }
            out.append(" };\n");
        }
        out.append("    }\n}\n");
        Files.writeString(Path.of(a[3]), out.toString());
        System.out.printf("[aosp-gfx]   com.android.internal.R: %d styleables (%d private attributes, %d not in the"
                + " table), %d private ids rewritten%n", styleables.size(), privateAttrs, missing, rewritten);
    }

    /** "type/name" -> id of package 0x01 in a resources.arsc (the apk's). */
    static Map<String, Integer> shippedIds(Path apk) throws IOException {
        byte[] arsc;
        try (ZipFile z = new ZipFile(apk.toFile()); InputStream in = z.getInputStream(z.getEntry("resources.arsc"))) {
            arsc = in.readAllBytes();
        }
        ByteBuffer b = ByteBuffer.wrap(arsc).order(ByteOrder.LITTLE_ENDIAN);
        Map<String, Integer> out = new HashMap<>();
        int off = b.getShort(2) & 0xffff;
        while (off < arsc.length) {
            int type = b.getShort(off) & 0xffff, size = b.getInt(off + 4);
            if (type == 0x0200) readPackage(b, off, out);
            off += size;
        }
        return out;
    }

    private static void readPackage(ByteBuffer b, int pkg, Map<String, Integer> out) {
        int pkgId = b.getInt(pkg + 8);
        int headerSize = b.getShort(pkg + 2) & 0xffff, size = b.getInt(pkg + 4);
        String[] types = strings(b, pkg + b.getInt(pkg + 8 + 4 + 256));
        String[] keys = strings(b, pkg + b.getInt(pkg + 8 + 4 + 256 + 8));
        int off = pkg + headerSize;
        while (off < pkg + size) {
            int t = b.getShort(off) & 0xffff, hs = b.getShort(off + 2) & 0xffff, sz = b.getInt(off + 4);
            if (t == 0x0201) {
                int typeId = b.get(off + 8) & 0xff, flags = b.get(off + 9) & 0xff;
                int count = b.getInt(off + 12), entriesStart = b.getInt(off + 16);
                for (int i = 0; i < count; i++) {
                    int index, entry;
                    if ((flags & 0x01) != 0) {           // FLAG_SPARSE: (index, offset / 4) pairs
                        index = b.getShort(off + hs + i * 4) & 0xffff;
                        entry = (b.getShort(off + hs + i * 4 + 2) & 0xffff) * 4;
                    } else if ((flags & 0x02) != 0) {    // FLAG_OFFSET16: offset / 4, 0xffff none
                        index = i;
                        int o = b.getShort(off + hs + i * 2) & 0xffff;
                        if (o == 0xffff) continue;
                        entry = o * 4;
                    } else {
                        index = i;
                        entry = b.getInt(off + hs + i * 4);
                        if (entry == -1) continue;
                    }
                    int e = off + entriesStart + entry;
                    int eflags = b.getShort(e + 2) & 0xffff;
                    int key = (eflags & 0x08) != 0 ? b.getShort(e) & 0xffff : b.getInt(e + 4);  // FLAG_COMPACT
                    out.putIfAbsent(types[typeId - 1] + "/" + keys[key], (pkgId << 24) | (typeId << 16) | index);
                }
            }
            off += sz;
        }
    }

    private static String[] strings(ByteBuffer b, int pool) {
        int count = b.getInt(pool + 8), flags = b.getInt(pool + 16), start = b.getInt(pool + 20);
        int hs = b.getShort(pool + 2) & 0xffff;
        boolean utf8 = (flags & 0x100) != 0;
        String[] out = new String[count];
        for (int i = 0; i < count; i++) {
            int s = pool + start + b.getInt(pool + hs + i * 4);
            if (utf8) {
                s += (b.get(s) & 0x80) != 0 ? 2 : 1;                   // UTF-16 length
                int len = b.get(s) & 0xff;
                if ((len & 0x80) != 0) { len = ((len & 0x7f) << 8) | (b.get(s + 1) & 0xff); s += 2; } else s += 1;
                out[i] = new String(b.array(), s, len, StandardCharsets.UTF_8);
            } else {
                int len = b.getShort(s) & 0xffff;
                if ((len & 0x8000) != 0) { len = ((len & 0x7fff) << 16) | (b.getShort(s + 2) & 0xffff); s += 4; } else s += 2;
                out[i] = new String(b.array(), s, len * 2, StandardCharsets.UTF_16LE);
            }
        }
        return out;
    }

    /** The two things read from a class file: its CONSTANT_Integer entries (offset of the value ->
     *  value) and the int[] fields of com.android.internal.R$styleable it names. */
    static final class ClassFile {
        final Map<Integer, Integer> intConstants = new TreeMap<>();
        final List<String> styleableFields = new ArrayList<>();

        ClassFile(byte[] data) throws IOException {
            ByteBuffer b = ByteBuffer.wrap(data);
            if (b.getInt(0) != 0xCAFEBABE) throw new IOException("not a class file");
            int n = b.getShort(8) & 0xffff, off = 10;
            Object[] cp = new Object[n];
            int[][] refs = new int[n][];
            for (int i = 1; i < n; i++) {
                int tag = data[off++] & 0xff;
                switch (tag) {
                    case 1: { int len = b.getShort(off) & 0xffff;
                              cp[i] = new String(data, off + 2, len, StandardCharsets.UTF_8); off += 2 + len; break; }
                    case 3: intConstants.put(off, b.getInt(off)); off += 4; break;
                    case 4: off += 4; break;
                    case 5: case 6: off += 8; i++; break;
                    case 7: case 8: case 16: case 19: case 20: refs[i] = new int[] {tag, b.getShort(off) & 0xffff}; off += 2; break;
                    case 9: case 10: case 11: case 12: case 17: case 18:
                        refs[i] = new int[] {tag, b.getShort(off) & 0xffff, b.getShort(off + 2) & 0xffff}; off += 4; break;
                    case 15: off += 3; break;
                    default: throw new IOException("constant pool tag " + tag);
                }
            }
            for (int i = 1; i < n; i++) {
                if (refs[i] == null || refs[i][0] != 9) continue;           // Fieldref
                String owner = (String) cp[refs[refs[i][1]][1]];
                int[] nt = refs[refs[i][2]];
                if (STYLEABLE.equals(owner) && "[I".equals(cp[nt[2]])) styleableFields.add((String) cp[nt[1]]);
            }
        }
    }
}
