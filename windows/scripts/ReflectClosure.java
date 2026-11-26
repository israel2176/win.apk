// ReflectClosure.java -- what reflection on AOSP's classes resolves, taken too (build_aosp_app.sh;
// checked on the stage by ReflectScan.java, CI's runtime tests).
//
// Apps look members of framework classes up by reflection (getMethod, getDeclaredField...): HotSpot then
// builds every declared method, field and constructor of the class, so each type a member's descriptor
// names (parameters, return type, declared exceptions, a field's type), and the class's supertypes, must
// load, or the app's call throws NoClassDefFoundError. On a device every framework class exists. The
// jdeps rounds of the build scripts take what the code names from a bounded set of packages; this takes,
// from android-all 14, every class such a descriptor names that nothing on the class path has, from any
// package, until nothing more is missing. Those classes are only loaded, never run, unless an app's code
// goes further than reflection.
//
//   java -cp asm.jar ReflectClosure.java <classes dir> <android-all.jar> --aosp <jar|dir>... --own <jar|dir>...
//
// The classes dir and the --aosp jars (aosp-views, aosp-resources, aosp-graphics) are scanned, and their
// nested classes may be taken beside them; --own: Win.apk's (the framework as compiled, the loader, the
// JDK patch folders), whose nested classes are never taken from AOSP (an inner class of AOSP's beside
// Win.apk's outer one would mix two implementations): such a type is added to Win.apk's own class,
// following AOSP 14's declaration.
// Prints the entries of android-all to take, one per line; on stderr, the types it cannot take: a
// nested class of one of Win.apk's own classes, or a class android-all lacks too.

import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassVisitor;
import org.objectweb.asm.FieldVisitor;
import org.objectweb.asm.MethodVisitor;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.Type;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Deque;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.stream.Stream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class ReflectClosure {
    /** A jar or a folder of classes. */
    record Source(ZipFile zip, Path dir) {
        boolean has(String cls) {
            String n = cls + ".class";
            return zip != null ? zip.getEntry(n) != null : Files.isRegularFile(dir.resolve(n));
        }

        static Source of(String p) throws IOException {
            File f = new File(p);
            return f.isDirectory() ? new Source(null, f.toPath()) : new Source(new ZipFile(f), null);
        }
    }

    static final List<Source> aosp = new ArrayList<>();
    static final List<Source> own = new ArrayList<>();

    public static void main(String[] args) throws IOException {
        Path classes = Path.of(args[0]);
        ZipFile all = new ZipFile(args[1]);
        List<Source> into = null;
        for (int i = 2; i < args.length; i++) {
            if (args[i].equals("--aosp")) into = aosp;
            else if (args[i].equals("--own")) into = own;
            else into.add(Source.of(args[i]));
        }
        aosp.add(0, new Source(null, classes));

        Set<String> taken = new TreeSet<>();
        Set<String> seen = new HashSet<>();
        Map<String, Set<String>> cannot = new TreeMap<>();
        Deque<byte[]> work = new ArrayDeque<>();
        try (Stream<Path> s = Files.walk(classes)) {
            for (Path p : (Iterable<Path>) s.filter(p -> p.toString().endsWith(".class"))::iterator) {
                work.add(Files.readAllBytes(p));
            }
        }
        for (Source a : aosp) {
            if (a.zip == null) continue;
            for (ZipEntry e : Collections.list(a.zip.entries())) {
                String n = e.getName();
                if (!n.endsWith(".class") || n.startsWith("META-INF/") || n.endsWith("module-info.class")) continue;
                try (InputStream in = a.zip.getInputStream(e)) {
                    work.add(in.readAllBytes());
                }
            }
        }
        while (!work.isEmpty()) {
            ClassReader r = new ClassReader(work.poll());
            String self = r.getClassName();
            for (String t : types(r)) {
                if (!seen.add(t) || exists(t)) continue;
                ZipEntry e = all.getEntry(t + ".class");
                int dollar = t.indexOf('$');
                String outer = dollar < 0 ? null : t.substring(0, dollar);
                if (e == null) {
                    cannot.computeIfAbsent(t + ": android-all 14 has no such class", k -> new TreeSet<>()).add(self);
                } else if (outer != null && !in(aosp, outer) && !taken.contains(outer) && in(own, outer)) {
                    cannot.computeIfAbsent(t + ": a nested class of Win.apk's own " + outer.replace('/', '.'),
                            k -> new TreeSet<>()).add(self);
                } else {
                    taken.add(t);
                    try (InputStream in = all.getInputStream(e)) {
                        work.add(in.readAllBytes());
                    }
                }
            }
        }
        for (String t : taken) System.out.println(t + ".class");
        for (Map.Entry<String, Set<String>> c : cannot.entrySet()) {
            System.err.println("[reflect-closure] cannot take " + c.getKey() + " (named by "
                    + String.join(", ", c.getValue()) + ")");
        }
    }

    static boolean in(List<Source> l, String cls) {
        for (Source s : l) if (s.has(cls)) return true;
        return false;
    }

    static boolean exists(String cls) {
        if (in(aosp, cls) || in(own, cls)) return true;
        // The JDK's own (jrt), as the JVM would find them.
        return ClassLoader.getPlatformClassLoader().getResource(cls + ".class") != null
                || ClassLoader.getSystemClassLoader().getResource(cls + ".class") != null;
    }

    /** The class types reflection on this class resolves: supertypes, and each declared member's descriptor
     *  and declared exceptions. */
    static Set<String> types(ClassReader r) {
        Set<String> out = new TreeSet<>();
        r.accept(new ClassVisitor(Opcodes.ASM9) {
            @Override
            public void visit(int v, int acc, String name, String sig, String sup, String[] itfs) {
                if (sup != null) out.add(sup);
                if (itfs != null) for (String i : itfs) out.add(i);
            }

            @Override
            public FieldVisitor visitField(int acc, String name, String desc, String sig, Object value) {
                add(out, Type.getType(desc));
                return null;
            }

            @Override
            public MethodVisitor visitMethod(int acc, String name, String desc, String sig, String[] exc) {
                Type m = Type.getMethodType(desc);
                add(out, m.getReturnType());
                for (Type a : m.getArgumentTypes()) add(out, a);
                if (exc != null) for (String x : exc) out.add(x);
                return null;
            }
        }, ClassReader.SKIP_CODE | ClassReader.SKIP_DEBUG | ClassReader.SKIP_FRAMES);
        return out;
    }

    static void add(Set<String> out, Type t) {
        if (t.getSort() == Type.ARRAY) t = t.getElementType();
        if (t.getSort() == Type.OBJECT) out.add(t.getInternalName());
    }
}
