// LinkScan.java -- which member references would not link on a class path of mixed jars
// (build_aosp_views.sh; docs/research/aosp-views.md). AOSP's classes and Win.apk's share package and
// class names; whichever jar comes first wins a name, and every call across the two must find its
// member (name and descriptor) in the class that won, its superclasses or its interfaces, or HotSpot
// throws NoSuchMethodError / NoSuchFieldError when it runs. This resolves every method and field
// reference of the scanned jars' classes against the class path, first jar first, as the JVM would,
// and prints what does not resolve:
//
//   java -cp asm.jar LinkScan.java [--only <prefix>]... <scanned jar,...> <class path jar>...
//
// <scanned jar,...> are the jars whose classes are checked (comma-separated; they are on the class
// path too, in the order given among the others); a class of a scanned jar that an earlier jar
// shadows is never loaded and is skipped. --only keeps the referencing classes whose internal name
// starts with a prefix. JDK classes come from the running JDK (jrt). Output, one line each:
//
//   M owner.name(desc) <- referrer [more...]     a method nothing on the path declares
//   F owner.name:desc <- referrer ...            a field
//   C owner <- referrer ...                      a class nothing on the path has
//
// sorted by owner. Exit status 0; the caller decides what is fatal.

import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassVisitor;
import org.objectweb.asm.FieldVisitor;
import org.objectweb.asm.MethodVisitor;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.Type;

import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class LinkScan {
    static final class Info {
        String superName;
        String[] interfaces = new String[0];
        final Set<String> methods = new HashSet<>();  // name+desc
        final Set<String> fields = new HashSet<>();   // name:desc and name (fields resolve by name+type)
        boolean polymorphic;  // MethodHandle/VarHandle: signature-polymorphic natives
    }

    static final List<ZipFile> path = new ArrayList<>();
    static final Map<String, Info> cache = new HashMap<>();
    static final Set<String> absent = new HashSet<>();

    static Info info(String name) {
        if (name.startsWith("[")) return info("java/lang/Object");
        Info i = cache.get(name);
        if (i != null || absent.contains(name)) return i;
        byte[] b = bytes(name);
        if (b == null) {
            absent.add(name);
            return null;
        }
        Info n = new Info();
        new ClassReader(b).accept(new ClassVisitor(Opcodes.ASM9) {
            @Override public void visit(int v, int acc, String nm, String sig, String sup, String[] itf) {
                n.superName = sup;
                n.interfaces = itf == null ? new String[0] : itf;
            }
            @Override public MethodVisitor visitMethod(int acc, String nm, String d, String s, String[] e) {
                n.methods.add(nm + d);
                if ((acc & Opcodes.ACC_NATIVE) != 0 && (acc & Opcodes.ACC_VARARGS) != 0) n.polymorphic = true;
                return null;
            }
            @Override public FieldVisitor visitField(int acc, String nm, String d, String s, Object v) {
                n.fields.add(nm + ":" + d);
                return null;
            }
        }, ClassReader.SKIP_CODE);
        cache.put(name, n);
        return n;
    }

    static byte[] bytes(String name) {
        try {
            for (ZipFile z : path) {
                ZipEntry e = z.getEntry(name + ".class");
                if (e != null) try (InputStream in = z.getInputStream(e)) { return in.readAllBytes(); }
            }
            if (name.startsWith("java/") || name.startsWith("javax/") || name.startsWith("jdk/")
                    || name.startsWith("sun/") || name.startsWith("org/w3c/") || name.startsWith("org/xml/")
                    || name.startsWith("com/sun/")) {
                try (InputStream in = ClassLoader.getSystemResourceAsStream(name + ".class")) {
                    if (in != null) return in.readAllBytes();
                }
            }
        } catch (IOException e) {
            throw new RuntimeException(e);
        }
        return null;
    }

    /** JVMS 5.4.3.3/4: the class, its superclasses, then every superinterface. */
    static boolean hasMethod(String owner, String key, Set<String> seen) {
        if (owner == null || !seen.add(owner)) return false;
        Info i = info(owner);
        if (i == null) return true;  // reported as a missing class instead
        if (i.methods.contains(key) || i.polymorphic && key.startsWith("invoke")) return true;
        if (hasMethod(i.superName, key, seen)) return true;
        for (String s : i.interfaces) if (hasMethod(s, key, seen)) return true;
        return false;
    }

    static boolean hasField(String owner, String key, Set<String> seen) {
        if (owner == null || !seen.add(owner)) return false;
        Info i = info(owner);
        if (i == null) return true;
        if (i.fields.contains(key)) return true;
        for (String s : i.interfaces) if (hasField(s, key, seen)) return true;
        return hasField(i.superName, key, seen);
    }

    public static void main(String[] a) throws IOException {
        List<String> only = new ArrayList<>();
        int k = 0;
        while (k < a.length && a[k].equals("--only")) { only.add(a[k + 1]); k += 2; }
        Set<String> scanned = new HashSet<>(List.of(a[k].split(",")));
        List<String> jars = new ArrayList<>();
        for (String s : a[k].split(",")) if (!jars.contains(s)) jars.add(s);
        for (int j = k + 1; j < a.length; j++) if (!jars.contains(a[j])) jars.add(a[j]);
        // The scanned jars take their place among the others as given on the command line: re-order
        // by the order of the class path arguments when they appear there too.
        List<String> order = new ArrayList<>();
        for (int j = k + 1; j < a.length; j++) order.add(a[j]);
        for (String s : jars) if (!order.contains(s)) order.add(0, s);
        for (String s : order) path.add(new ZipFile(s));

        Map<String, Set<String>> missing = new TreeMap<>();
        for (int zi = 0; zi < path.size(); zi++) {
            if (!scanned.contains(order.get(zi))) continue;
            ZipFile z = path.get(zi);
            for (ZipEntry e : java.util.Collections.list(z.entries())) {
                String n = e.getName();
                if (!n.endsWith(".class") || n.startsWith("META-INF/") || n.equals("module-info.class")) continue;
                String cls = n.substring(0, n.length() - 6);
                if (!only.isEmpty() && only.stream().noneMatch(cls::startsWith)) continue;
                // Shadowed by an earlier jar: never loaded.
                boolean shadowed = false;
                for (int p = 0; p < zi; p++) if (path.get(p).getEntry(n) != null) { shadowed = true; break; }
                if (shadowed) continue;
                byte[] b;
                try (InputStream in = z.getInputStream(e)) { b = in.readAllBytes(); }
                scan(cls, b, missing);
            }
        }
        for (Map.Entry<String, Set<String>> m : missing.entrySet()) {
            StringBuilder sb = new StringBuilder(m.getKey()).append(" <-");
            int c = 0;
            for (String r : m.getValue()) {
                if (c++ == 4) { sb.append(" (+").append(m.getValue().size() - 4).append(")"); break; }
                sb.append(' ').append(r);
            }
            System.out.println(sb);
        }
    }

    static void scan(String cls, byte[] b, Map<String, Set<String>> missing) {
        new ClassReader(b).accept(new ClassVisitor(Opcodes.ASM9) {
            @Override public MethodVisitor visitMethod(int acc, String nm, String d, String s, String[] e) {
                return new MethodVisitor(Opcodes.ASM9) {
                    @Override public void visitMethodInsn(int op, String owner, String n, String desc, boolean itf) {
                        if (owner.startsWith("[")) return;
                        if (info(owner) == null) { note("C " + owner, cls, missing); return; }
                        if (!hasMethod(owner, n + desc, new HashSet<>())) {
                            note("M " + owner + "." + n + desc, cls, missing);
                        }
                    }
                    @Override public void visitFieldInsn(int op, String owner, String n, String desc) {
                        if (info(owner) == null) { note("C " + owner, cls, missing); return; }
                        if (!hasField(owner, n + ":" + desc, new HashSet<>())) {
                            note("F " + owner + "." + n + ":" + desc, cls, missing);
                        }
                    }
                    @Override public void visitTypeInsn(int op, String type) {
                        String t = type.startsWith("[") ? Type.getType(type).getElementType().getInternalName() : type;
                        if (t.length() > 1 && info(t) == null && Type.getObjectType(t).getSort() == Type.OBJECT) {
                            note("C " + t, cls, missing);
                        }
                    }
                };
            }
            @Override public void visit(int v, int acc, String nm, String sig, String sup, String[] itf) {
                if (sup != null && info(sup) == null) note("C " + sup, cls, missing);
                if (itf != null) for (String i : itf) if (info(i) == null) note("C " + i, cls, missing);
            }
        }, ClassReader.SKIP_DEBUG);
    }

    static void note(String what, String cls, Map<String, Set<String>> missing) {
        missing.computeIfAbsent(what, x -> new TreeSet<>()).add(cls);
    }
}
