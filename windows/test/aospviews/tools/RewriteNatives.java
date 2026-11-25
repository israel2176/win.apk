// RewriteNatives -- the build-time step that gives AOSP's classes Java bodies for their natives.
//
// AOSP's android.content.res.AssetManager, ApkAssets, XmlBlock and StringBlock are thin Java over
// libandroidfw (AssetManager2, LoadedArsc, ResXMLTree, AttributeResolution) reached through JNI.
// Robolectric's native runtime does not carry libandroidfw; Robolectric runs those natives on its
// Java port of it (org.robolectric.res.android, the `resources` jar), reached through bytecode
// instrumentation of every class. This does the one small part of that instrumentation Win.apk
// would need, once, at build time: each native method of the named classes becomes a plain method
// whose body calls a static method with the same name and descriptor on a bridge class
// (`<bridge package>/Bridge<SimpleName>`; an instance native gets `this` first). A native the bridge
// lacks gets a body that throws UnsupportedOperationException naming it, so what is missing shows
// up by name the first time something calls it, never as a silent 0.
//
//   RewriteNatives <android-all.jar> <bridge classes dir> <bridge package> <out dir> <class>[:<package>]...
//
// A class given as <class>:<package> takes its bridge from that package instead of the default.
//
// Prints one line per class (bridged / unbridged counts) and every unbridged native.
import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassVisitor;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.MethodVisitor;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.Type;

import java.io.File;
import java.io.InputStream;
import java.nio.file.Files;
import java.util.HashSet;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public final class RewriteNatives {
    public static void main(String[] a) throws Exception {
        if (a.length < 5) {
            System.err.println("usage: RewriteNatives <android-all.jar> <bridge dir> <bridge pkg> <out dir> <class>...");
            System.exit(2);
        }
        int bridgedAll = 0, missingAll = 0;
        try (ZipFile jar = new ZipFile(a[0])) {
            for (int i = 4; i < a.length; i++) {
                String[] spec = a[i].split(":");
                String internal = spec[0].replace('.', '/');
                ZipEntry e = jar.getEntry(internal + ".class");
                if (e == null) throw new IllegalArgumentException("no class " + spec[0] + " in " + a[0]);
                byte[] in;
                try (InputStream s = jar.getInputStream(e)) { in = s.readAllBytes(); }
                String simple = internal.substring(internal.lastIndexOf('/') + 1).replace('$', '_');
                String bridge = (spec.length > 1 ? spec[1] : a[2]).replace('.', '/') + "/Bridge" + simple;
                Set<String> have = bridgeMethods(new File(a[1], bridge + ".class"));
                int[] counts = new int[2];
                ClassReader cr = new ClassReader(in);
                ClassWriter cw = new ClassWriter(cr, ClassWriter.COMPUTE_MAXS);
                cr.accept(new ClassVisitor(Opcodes.ASM9, cw) {
                    @Override
                    public MethodVisitor visitMethod(int acc, String name, String desc, String sig, String[] exc) {
                        if ((acc & Opcodes.ACC_NATIVE) == 0) return super.visitMethod(acc, name, desc, sig, exc);
                        boolean isStatic = (acc & Opcodes.ACC_STATIC) != 0;
                        MethodVisitor mv = super.visitMethod(acc & ~Opcodes.ACC_NATIVE, name, desc, sig, exc);
                        String bdesc = isStatic ? desc : "(L" + internal + ";" + desc.substring(1);
                        mv.visitCode();
                        if (have.contains(name + bdesc)) {
                            int slot = 0;
                            if (!isStatic) mv.visitVarInsn(Opcodes.ALOAD, slot++);
                            for (Type t : Type.getArgumentTypes(desc)) {
                                mv.visitVarInsn(t.getOpcode(Opcodes.ILOAD), slot);
                                slot += t.getSize();
                            }
                            mv.visitMethodInsn(Opcodes.INVOKESTATIC, bridge, name, bdesc, false);
                            mv.visitInsn(Type.getReturnType(desc).getOpcode(Opcodes.IRETURN));
                            counts[0]++;
                        } else {
                            mv.visitTypeInsn(Opcodes.NEW, "java/lang/UnsupportedOperationException");
                            mv.visitInsn(Opcodes.DUP);
                            mv.visitLdcInsn("native " + internal.replace('/', '.') + "." + name + desc
                                    + " has no Java body (no " + bridge.replace('/', '.') + "." + name + ")");
                            mv.visitMethodInsn(Opcodes.INVOKESPECIAL, "java/lang/UnsupportedOperationException", "<init>",
                                    "(Ljava/lang/String;)V", false);
                            mv.visitInsn(Opcodes.ATHROW);
                            counts[1]++;
                            System.out.println("  unbridged: " + internal.replace('/', '.') + "." + name + desc);
                        }
                        mv.visitMaxs(0, 0);
                        mv.visitEnd();
                        return null;
                    }
                }, 0);
                File out = new File(a[3], internal + ".class");
                out.getParentFile().mkdirs();
                Files.write(out.toPath(), cw.toByteArray());
                System.out.printf("%-48s natives bridged %3d, unbridged %3d%n", spec[0], counts[0], counts[1]);
                bridgedAll += counts[0];
                missingAll += counts[1];
            }
        }
        System.out.printf("REWRITE natives bridged %d, unbridged %d%n", bridgedAll, missingAll);
    }

    /** Every static method of the bridge class, as name + descriptor (none when it does not exist). */
    static Set<String> bridgeMethods(File f) throws Exception {
        Set<String> s = new HashSet<>();
        if (!f.isFile()) return s;
        new ClassReader(Files.readAllBytes(f.toPath())).accept(new ClassVisitor(Opcodes.ASM9) {
            @Override
            public MethodVisitor visitMethod(int acc, String name, String desc, String sig, String[] exc) {
                if ((acc & Opcodes.ACC_STATIC) != 0) s.add(name + desc);
                return null;
            }
        }, ClassReader.SKIP_CODE);
        return s;
    }
}
