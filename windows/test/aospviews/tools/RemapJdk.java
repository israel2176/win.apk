// RemapJdk -- finds, and where a bridge exists redirects, the calls AOSP's framework makes to members of
// java.* / javax.* that only ART's libcore has (System.logW, ...): HotSpot's JDK 21 lacks them and the
// call ends in NoSuchMethodError at run time. The second half of the build-time rewrite.
//
//   RemapJdk <android-all.jar> <bridge classes dir> <bridge class> <out dir> <class prefix>...
//
// For every class of the jar under the prefixes (a class already in <out dir>, from RewriteNatives,
// is taken from there), every method and field reference to java.* / javax.* is checked against the
// running JDK. A missing method whose bridge (<bridge class>, a static method of the same name taking
// the receiver first for an instance call) exists is redirected there; the class is written to
// <out dir>. Prints every missing member with how many classes name it, the redirected ones marked,
// then a REMAP line with the totals.
import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassVisitor;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.MethodVisitor;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.Type;

import java.io.File;
import java.io.InputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public final class RemapJdk {
    static final Map<String, Boolean> exists = new HashMap<>();
    static final Map<String, Set<String>> missing = new TreeMap<>();  // member -> classes naming it

    public static void main(String[] a) throws Exception {
        String bridge = a[2].replace('.', '/');
        Set<String> have = RewriteNatives.bridgeMethods(new File(a[1], bridge + ".class"));
        int scanned = 0, written = 0, redirected = 0;
        try (ZipFile jar = new ZipFile(a[0])) {
            for (Enumeration<? extends ZipEntry> en = jar.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement();
                String n = e.getName();
                if (!n.endsWith(".class") || !under(n, a)) continue;
                scanned++;
                File out = new File(a[3], n);
                byte[] in;
                if (out.isFile()) {
                    in = Files.readAllBytes(out.toPath());
                } else {
                    try (InputStream s = jar.getInputStream(e)) { in = s.readAllBytes(); }
                }
                String cls = n.substring(0, n.length() - 6);
                int[] changed = new int[1];
                ClassReader cr = new ClassReader(in);
                ClassWriter cw = new ClassWriter(cr, 0);
                cr.accept(new ClassVisitor(Opcodes.ASM9, cw) {
                    @Override
                    public MethodVisitor visitMethod(int acc, String name, String desc, String sig, String[] exc) {
                        return new MethodVisitor(Opcodes.ASM9, super.visitMethod(acc, name, desc, sig, exc)) {
                            @Override
                            public void visitMethodInsn(int op, String owner, String mn, String md, boolean itf) {
                                if (jdk(owner) && !polymorphic(owner, mn) && !methodExists(owner, mn, md)) {
                                    String bdesc = op == Opcodes.INVOKESTATIC ? md : "(L" + owner + ";" + md.substring(1);
                                    String key = owner.replace('/', '.') + "." + mn + md;
                                    boolean bridged = op != Opcodes.INVOKESPECIAL && have.contains(mn + bdesc);
                                    missing.computeIfAbsent(key + (bridged ? "  [bridged]" : ""), k -> new TreeSet<>()).add(cls);
                                    if (bridged) {
                                        super.visitMethodInsn(Opcodes.INVOKESTATIC, bridge, mn, bdesc, false);
                                        changed[0]++;
                                        return;
                                    }
                                }
                                super.visitMethodInsn(op, owner, mn, md, itf);
                            }

                            @Override
                            public void visitFieldInsn(int op, String owner, String fn, String fd) {
                                if (jdk(owner) && !fieldExists(owner, fn)) {
                                    missing.computeIfAbsent(owner.replace('/', '.') + "." + fn + " (field)", k -> new TreeSet<>()).add(cls);
                                }
                                super.visitFieldInsn(op, owner, fn, fd);
                            }
                        };
                    }
                }, 0);
                if (changed[0] > 0) {
                    out.getParentFile().mkdirs();
                    Files.write(out.toPath(), cw.toByteArray());
                    written++;
                    redirected += changed[0];
                }
            }
        }
        for (Map.Entry<String, Set<String>> m : missing.entrySet()) {
            System.out.printf("  missing in JDK %d: %s  (%d classes, e.g. %s)%n", Runtime.version().feature(),
                    m.getKey(), m.getValue().size(), m.getValue().iterator().next().replace('/', '.'));
        }
        System.out.printf("REMAP classes scanned %d, ART-only java.* members named %d, call sites redirected %d in %d classes%n",
                scanned, missing.size(), redirected, written);
    }

    static boolean under(String n, String[] a) {
        for (int i = 4; i < a.length; i++) if (n.startsWith(a[i].replace('.', '/'))) return true;
        return false;
    }

    /** A reference the JDK must answer: java.* always (no class path may define java.*), javax.* when
     *  the JDK has the class (javax.microedition.khronos, for one, is android-all's own). */
    static boolean jdk(String owner) {
        return owner.startsWith("java/") || owner.startsWith("javax/") && load(owner) != null;
    }

    /** MethodHandle.invoke / invokeExact and VarHandle's accessors take any descriptor. */
    static boolean polymorphic(String owner, String name) {
        return owner.equals("java/lang/invoke/MethodHandle") && (name.equals("invoke") || name.equals("invokeExact"))
                || owner.equals("java/lang/invoke/VarHandle");
    }

    static Class<?> load(String owner) {
        try {
            return Class.forName(owner.replace('/', '.'), false, ClassLoader.getPlatformClassLoader());
        } catch (Throwable t) {
            return null;
        }
    }

    static boolean methodExists(String owner, String name, String desc) {
        return exists.computeIfAbsent(owner + "." + name + desc, k -> {
            Class<?> c = load(owner);
            if (c == null) return false;
            if (name.equals("<init>")) {
                for (Constructor<?> k2 : c.getDeclaredConstructors()) {
                    if (Type.getConstructorDescriptor(k2).equals(desc)) return true;
                }
                return false;
            }
            return findMethod(c, name, desc, new java.util.HashSet<>());
        });
    }

    static boolean findMethod(Class<?> c, String name, String desc, Set<Class<?>> seen) {
        if (c == null || !seen.add(c)) return false;
        for (Method m : c.getDeclaredMethods()) {
            if (m.getName().equals(name) && Type.getMethodDescriptor(m).equals(desc)) return true;
        }
        if (findMethod(c.getSuperclass(), name, desc, seen)) return true;
        for (Class<?> i : c.getInterfaces()) if (findMethod(i, name, desc, seen)) return true;
        return c.isInterface() && findMethod(Object.class, name, desc, seen);
    }

    static boolean fieldExists(String owner, String name) {
        return exists.computeIfAbsent(owner + "#" + name, k -> {
            for (Class<?> c = load(owner); c != null; c = c.getSuperclass()) {
                for (Field f : c.getDeclaredFields()) if (f.getName().equals(name)) return true;
                for (Class<?> i : c.getInterfaces()) {
                    for (Field f : i.getFields()) if (f.getName().equals(name)) return true;
                }
            }
            return false;
        });
    }
}
