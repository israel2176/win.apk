// AospAddMethods.java -- gives AOSP's graphics classes (build_aosp_gfx.sh's) methods they lack: those of
// a later Android, which the runtime's SDK level makes apps call, and those Win.apk's own framework
// calls on a class AOSP's replaces.
//
//   java AospAddMethods.java <classes dir> <helper class>...
//
// Every public static method of a helper class (a class file under <classes dir>, "a/b/C") whose first
// parameter is a class under <classes dir> becomes an instance method of that class: public, the same
// name, the other parameters and the return type, its body a call to the helper with `this` first:
//
//   Api35.setUseBoundsForWidth(StaticLayout$Builder, boolean)   adds
//   StaticLayout$Builder.setUseBoundsForWidth(boolean) { return Api35.setUseBoundsForWidth(this, b); }
//
// The body has no branch, so it needs no StackMapTable. New constant pool entries go at the end and the
// method after the class's last one; nothing else in the file moves. A method the class already has
// stops the build: the helper is stale (android-all moved on), or it would hide AOSP's own.

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class AospAddMethods {
    /** A class file's constant pool and where its parts are. */
    static final class ClassFile {
        final byte[] data;
        final int count, poolEnd, methodsCount, methodsEnd;
        final String[] utf8;
        final String name;
        final List<String[]> methods = new ArrayList<>();  // {access, name, descriptor}

        ClassFile(byte[] data, Path file) throws IOException {
            this.data = data;
            ByteBuffer b = ByteBuffer.wrap(data);
            if (b.getInt(0) != 0xCAFEBABE) throw new IOException(file + ": not a class file");
            count = b.getShort(8) & 0xffff;
            utf8 = new String[count];
            int[] classNames = new int[count];
            int off = 10;
            for (int i = 1; i < count; i++) {
                int tag = data[off++] & 0xff;
                switch (tag) {
                    case 1: { int len = b.getShort(off) & 0xffff;
                              utf8[i] = new String(data, off + 2, len, StandardCharsets.UTF_8); off += 2 + len; break; }
                    case 7: classNames[i] = b.getShort(off) & 0xffff; off += 2; break;
                    case 8: case 16: case 19: case 20: off += 2; break;
                    case 3: case 4: case 9: case 10: case 11: case 12: case 17: case 18: off += 4; break;
                    case 5: case 6: off += 8; i++; break;
                    case 15: off += 3; break;
                    default: throw new IOException(file + ": constant pool tag " + tag);
                }
            }
            poolEnd = off;
            name = utf8[classNames[b.getShort(off + 2) & 0xffff]];
            off += 6;
            off += 2 + 2 * (b.getShort(off) & 0xffff);  // interfaces
            off = skipMembers(b, off, null);             // fields
            methodsCount = b.getShort(off) & 0xffff;
            methodsEnd = skipMembers(b, off, methods);
        }

        private int skipMembers(ByteBuffer b, int off, List<String[]> out) {
            int n = b.getShort(off) & 0xffff;
            off += 2;
            for (int i = 0; i < n; i++) {
                if (out != null) {
                    out.add(new String[] {Integer.toString(b.getShort(off) & 0xffff),
                        utf8[b.getShort(off + 2) & 0xffff], utf8[b.getShort(off + 4) & 0xffff]});
                }
                int attrs = b.getShort(off + 6) & 0xffff;
                off += 8;
                for (int a = 0; a < attrs; a++) off += 6 + b.getInt(off + 2);
            }
            return off;
        }

        boolean has(String n, String d) {
            for (String[] m : methods) if (m[1].equals(n) && m[2].equals(d)) return true;
            return false;
        }
    }

    /** The parameter types of a method descriptor, one descriptor each, and its return type (last). */
    static List<String> types(String desc) {
        List<String> out = new ArrayList<>();
        int i = 1;
        while (desc.charAt(i) != ')') {
            int s = i;
            while (desc.charAt(i) == '[') i++;
            i = desc.charAt(i) == 'L' ? desc.indexOf(';', i) + 1 : i + 1;
            out.add(desc.substring(s, i));
        }
        out.add(desc.substring(i + 1));
        return out;
    }

    static int slots(String t) { return t.equals("J") || t.equals("D") ? 2 : t.equals("V") ? 0 : 1; }

    static int loadOp(String t) {
        switch (t.charAt(0)) {
            case 'J': return 0x16;  // lload
            case 'F': return 0x17;  // fload
            case 'D': return 0x18;  // dload
            case 'L': case '[': return 0x19;  // aload
            default: return 0x15;   // iload: Z B C S I
        }
    }

    static int returnOp(String t) {
        switch (t.charAt(0)) {
            case 'V': return 0xb1;
            case 'J': return 0xad;
            case 'F': return 0xae;
            case 'D': return 0xaf;
            case 'L': case '[': return 0xb0;
            default: return 0xac;
        }
    }

    /** Appends constant pool entries after a class's own, by index. */
    static final class Pool {
        final ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        int next;

        Pool(int count) { next = count; }

        int utf8(String s) {
            byte[] b = s.getBytes(StandardCharsets.UTF_8);
            bytes.write(1);
            u2(b.length);
            bytes.writeBytes(b);
            return next++;
        }

        int ref(int tag, int a, int b) {
            bytes.write(tag);
            u2(a);
            if (tag != 7) u2(b);
            return next++;
        }

        void u2(int v) { bytes.write(v >> 8); bytes.write(v); }
    }

    public static void main(String[] a) throws Exception {
        if (a.length < 2) throw new IllegalArgumentException("usage: AospAddMethods <classes dir> <helper class>...");
        Path dir = Path.of(a[0]);
        // Target class -> {name, descriptor, helper, helper descriptor} to add.
        Map<String, List<String[]>> adds = new LinkedHashMap<>();
        for (int h = 1; h < a.length; h++) {
            Path hf = dir.resolve(a[h] + ".class");
            ClassFile helper = new ClassFile(Files.readAllBytes(hf), hf);
            for (String[] m : helper.methods) {
                int acc = Integer.parseInt(m[0]);
                if ((acc & 0x0009) != 0x0009 || m[1].startsWith("<")) continue;  // public static only
                List<String> t = types(m[2]);
                String first = t.get(0);
                if (t.size() < 2 || !first.startsWith("L")) continue;
                String target = first.substring(1, first.length() - 1);
                if (!Files.isRegularFile(dir.resolve(target + ".class"))) continue;
                StringBuilder d = new StringBuilder("(");
                for (int i = 1; i < t.size() - 1; i++) d.append(t.get(i));
                d.append(')').append(t.get(t.size() - 1));
                adds.computeIfAbsent(target, k -> new ArrayList<>())
                    .add(new String[] {m[1], d.toString(), a[h], m[2]});
            }
        }
        for (Map.Entry<String, List<String[]>> e : adds.entrySet()) {
            Path file = dir.resolve(e.getKey() + ".class");
            ClassFile cf = new ClassFile(Files.readAllBytes(file), file);
            Pool pool = new Pool(cf.count);
            ByteArrayOutputStream methods = new ByteArrayOutputStream();
            int code = pool.utf8("Code");
            Map<String, Integer> helperClasses = new LinkedHashMap<>();
            for (String[] m : e.getValue()) {
                if (cf.has(m[0], m[1])) {
                    throw new IllegalStateException(cf.name + " already has " + m[0] + m[1] + " (" + m[2] + ")");
                }
                int helperClass = helperClasses.computeIfAbsent(m[2], n -> pool.ref(7, pool.utf8(n), 0));
                int nat = pool.ref(12, pool.utf8(m[0]), pool.utf8(m[3]));
                int mref = pool.ref(10, helperClass, nat);
                int nameIdx = pool.utf8(m[0]), descIdx = pool.utf8(m[1]);
                // aload_0, each parameter by its type, invokestatic, return by type.
                ByteArrayOutputStream body = new ByteArrayOutputStream();
                body.write(0x2a);
                List<String> t = types(m[1]);
                int slot = 1;
                for (int i = 0; i < t.size() - 1; i++) {
                    body.write(loadOp(t.get(i)));
                    body.write(slot);
                    slot += slots(t.get(i));
                }
                if (slot > 255) throw new IllegalStateException(m[0] + m[1] + ": too many parameters");
                body.write(0xb8);
                body.write(mref >> 8);
                body.write(mref);
                String ret = t.get(t.size() - 1);
                body.write(returnOp(ret));
                int maxStack = Math.max(slot, slots(ret));
                byte[] c = body.toByteArray();
                DataOut out = new DataOut(methods);
                out.u2(0x0001);  // ACC_PUBLIC
                out.u2(nameIdx);
                out.u2(descIdx);
                out.u2(1);       // one attribute: Code
                out.u2(code);
                out.u4(12 + c.length);
                out.u2(maxStack);
                out.u2(slot);
                out.u4(c.length);
                methods.writeBytes(c);
                out.u2(0);       // no exception table
                out.u2(0);       // no attributes
                System.out.println("[aosp-gfx]   " + cf.name + "." + m[0] + m[1] + " -> " + m[2]);
            }
            if (pool.next > 0xffff) throw new IllegalStateException(cf.name + ": constant pool full");
            byte[] d = cf.data;
            ByteArrayOutputStream out = new ByteArrayOutputStream(d.length + pool.bytes.size() + methods.size());
            out.write(d, 0, 8);
            out.write(pool.next >> 8);
            out.write(pool.next);
            out.write(d, 10, cf.poolEnd - 10);
            pool.bytes.writeTo(out);
            // Up to the methods count, the count plus ours, the class's methods, ours, the rest.
            int countAt = methodsCountOffset(cf);
            out.write(d, cf.poolEnd, countAt - cf.poolEnd);
            int n = cf.methodsCount + e.getValue().size();
            out.write(n >> 8);
            out.write(n);
            out.write(d, countAt + 2, cf.methodsEnd - countAt - 2);
            methods.writeTo(out);
            out.write(d, cf.methodsEnd, d.length - cf.methodsEnd);
            Files.write(file, out.toByteArray());
        }
    }

    /** Where the methods_count of the class file is: after the fields. */
    static int methodsCountOffset(ClassFile cf) {
        ByteBuffer b = ByteBuffer.wrap(cf.data);
        int off = cf.poolEnd + 6;
        off += 2 + 2 * (b.getShort(off) & 0xffff);
        int n = b.getShort(off) & 0xffff;
        off += 2;
        for (int i = 0; i < n; i++) {
            int attrs = b.getShort(off + 6) & 0xffff;
            off += 8;
            for (int at = 0; at < attrs; at++) off += 6 + b.getInt(off + 2);
        }
        return off;
    }

    static final class DataOut {
        final ByteArrayOutputStream out;

        DataOut(ByteArrayOutputStream out) { this.out = out; }

        void u2(int v) { out.write(v >> 8); out.write(v); }

        void u4(int v) { u2(v >>> 16); u2(v & 0xffff); }
    }
}
