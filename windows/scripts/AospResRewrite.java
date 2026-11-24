// AospResRewrite.java -- the build-time rewrite that lets AOSP 14's android.content.res run on HotSpot
// beside Win.apk's framework (build_aosp_res.sh; docs/research/aosp-views.md, step R1). It edits the
// class files under <classes dir> in place, with ASM:
//
//   * Natives. Each native method of AssetManager, ApkAssets, XmlBlock and StringBlock (libandroidfw's,
//     through JNI, on Android) becomes a plain method that calls the static method of the same name
//     and descriptor on com.winapk.aospres.Native<Class> (an instance native passes `this` first),
//     which runs it on Robolectric's Java port of libandroidfw. A native the bridge lacks throws
//     UnsupportedOperationException naming it, so it shows up by name, never as a silent 0.
//   * Attribute sets. AOSP's resources code casts every AttributeSet it is handed to XmlBlock.Parser
//     (it reads the element's compiled attributes from the parser's native state). Win.apk's own
//     inflater hands its views a set over a DOM element; each `checkcast XmlBlock$Parser` becomes
//     a call to com.winapk.aospres.AttrSets.parser(Object), which gives back the parser itself, or
//     one standing on the same element of the same compiled file (AttrSets says how).
//   * What ART and AOSP's other classes have and Win.apk's do not (found by windows/test's member
//     scan against android-framework.jar): their calls and fields go to com.winapk.aospres.Compat,
//     and DisplayMetrics' noncompat* fields to the plain ones (Win.apk never scales for
//     compatibility, so the two are always equal).
//   * Assets. AOSP's decoders hand an AssetInputStream's native Asset* to the native runtime; here
//     an asset is the port's Java object. In the graphics classes build_aosp_res.sh copies in, the
//     `instanceof AssetManager$AssetInputStream` tests become false and ImageDecoder.createFromAsset
//     reads the asset as the InputStream it also is.
//   * Guava. Robolectric's port names 13 Guava types; they are mapped to the small stand-ins in
//     com.winapk.aospres.guava, so an app that ships its own Guava never meets a partial one first
//     on the class path.
//
//   java -cp asm.jar:asm-commons.jar AospResRewrite.java <classes dir>
//
// Prints what it changed, per kind.

import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassVisitor;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.MethodVisitor;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.Type;
import org.objectweb.asm.commons.ClassRemapper;
import org.objectweb.asm.commons.Remapper;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.stream.Stream;

public class AospResRewrite {
    static final String BRIDGE_PKG = "com/winapk/aospres/";
    static final Set<String> NATIVE_CLASSES = Set.of("android/content/res/AssetManager",
            "android/content/res/ApkAssets", "android/content/res/XmlBlock", "android/content/res/StringBlock");
    static final String COMPAT = BRIDGE_PKG + "Compat";
    static final String PARSER = "android/content/res/XmlBlock$Parser";
    static final String ASSET_STREAM = "android/content/res/AssetManager$AssetInputStream";

    /** owner.name+desc of a static call -> the class that has it, same name and descriptor. */
    static final Map<String, String> STATIC_CALLS = Map.of(
            "android/content/pm/ActivityInfo.activityInfoConfigNativeToJava(I)I", COMPAT,
            "android/os/StrictMode.noteResourceMismatch(Ljava/lang/Object;)V", COMPAT,
            "android/text/TextUtils.emptyIfNull(Ljava/lang/String;)Ljava/lang/String;", COMPAT,
            "android/text/TextUtils.formatSimple(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;", COMPAT,
            "android/app/ActivityThread.isSystem()Z", COMPAT);
    /** owner.name+desc of an instance call -> a static on Compat taking the receiver first. */
    static final Map<String, String> INSTANCE_CALLS = Map.of(
            "dalvik/system/VMRuntime.addressOf(Ljava/lang/Object;)J", "addressOf");
    static final Map<String, String> STATIC_FIELDS = Map.of(
            "android/os/Build$VERSION.RESOURCES_SDK_INT", COMPAT);
    static final Map<String, String> RENAMED_FIELDS = Map.of(
            "noncompatWidthPixels", "widthPixels", "noncompatHeightPixels", "heightPixels",
            "noncompatDensity", "density", "noncompatScaledDensity", "scaledDensity",
            "noncompatXdpi", "xdpi", "noncompatYdpi", "ydpi", "noncompatDensityDpi", "densityDpi");

    static final Map<String, Integer> counts = new TreeMap<>();
    static void count(String what) { counts.merge(what, 1, Integer::sum); }

    public static void main(String[] a) throws IOException {
        if (a.length != 1) throw new IllegalArgumentException("usage: AospResRewrite <classes dir>");
        Path root = Path.of(a[0]);
        List<Path> files;
        try (Stream<Path> s = Files.walk(root)) {
            files = s.filter(p -> p.toString().endsWith(".class")).toList();
        }
        Remapper guava = new Remapper() {
            @Override public String map(String name) {
                if (name.startsWith("com/google/common/")) {
                    count("Guava type names mapped to the stand-ins");
                    return BRIDGE_PKG + "guava/" + name.substring("com/google/common/".length());
                }
                return name;
            }
        };
        for (Path f : files) {
            String name = root.relativize(f).toString().replace('\\', '/');
            name = name.substring(0, name.length() - ".class".length());
            if (name.startsWith(BRIDGE_PKG)) continue;  // our own classes are written for HotSpot
            byte[] in = Files.readAllBytes(f);
            ClassReader cr = new ClassReader(in);
            ClassWriter cw = new ClassWriter(cr, ClassWriter.COMPUTE_MAXS);
            ClassVisitor cv = new ClassRemapper(cw, guava);
            if (name.startsWith("android/") || name.startsWith("com/android/")) {
                Set<String> bridge = NATIVE_CLASSES.contains(name) ? bridgeMethods(root, name) : null;
                cv = new Rewriter(cv, name, bridge);
            }
            if (name.equals(PORT_THEME)) cv = themeSetToResets(cv);
            if (name.equals(PORT_PACKAGE)) cv = collectConfigurationsFixed(cv);
            cr.accept(cv, 0);
            byte[] out = cw.toByteArray();
            if (!java.util.Arrays.equals(in, out)) Files.write(f, out);
        }
        counts.forEach((k, v) -> System.out.println("[aosp-res]   " + v + " " + k));
    }

    static final String PORT_PACKAGE = "org/robolectric/res/android/LoadedArsc$LoadedPackage";

    /** The port's LoadedPackage.CollectConfigurations walks its type map by the indexes 0..size-1,
     *  not by its keys (type ids, from 1), and dereferences the missing ones: every call threw, and
     *  AOSP's ActivityThread asks for the size configurations at each activity launch. Its body
     *  becomes windows/java/aosp_res's PortFixes.collectConfigurations, the C++'s loop. */
    static ClassVisitor collectConfigurationsFixed(ClassVisitor next) {
        return new ClassVisitor(Opcodes.ASM9, next) {
            @Override public MethodVisitor visitMethod(int acc, String name, String desc, String sig, String[] exc) {
                MethodVisitor mv = super.visitMethod(acc, name, desc, sig, exc);
                if (!name.equals("CollectConfigurations") || !desc.equals("(ZLjava/util/Set;)V")) return mv;
                mv.visitCode();
                mv.visitVarInsn(Opcodes.ALOAD, 0);
                mv.visitVarInsn(Opcodes.ILOAD, 1);
                mv.visitVarInsn(Opcodes.ALOAD, 2);
                mv.visitMethodInsn(Opcodes.INVOKESTATIC, "org/robolectric/res/android/PortFixes",
                        "collectConfigurations", "(L" + PORT_PACKAGE + ";ZLjava/util/Set;)V", false);
                mv.visitInsn(Opcodes.RETURN);
                mv.visitMaxs(0, 0);
                mv.visitEnd();
                count("LoadedPackage.CollectConfigurations made the C++'s loop");
                return null;
            }
        };
    }

    static final String PORT_THEME = "org/robolectric/res/android/CppAssetManager2$Theme";
    static final String PORT_THEME_PACKAGE = PORT_THEME + "$ThemePackage";

    /** Robolectric's port of AssetManager2's Theme::SetTo: where the C++ resets a package slot the
     *  source theme does not have (`packages_[p].reset()`, empty), the port puts a new, empty
     *  ThemePackage in it, with its 256-slot type array: 254 of them per copy, 2 KB each, half a
     *  megabyte for every theme copied (a context's, a DecorContext's...), most of the theme memory
     *  an app's heap held. The first `new ThemePackage()` of SetTo, that branch's, becomes null,
     *  as the C++ leaves it; every reader of a slot tests it for null (GetAttribute, ApplyStyle). */
    static ClassVisitor themeSetToResets(ClassVisitor next) {
        return new ClassVisitor(Opcodes.ASM9, next) {
            @Override public MethodVisitor visitMethod(int acc, String name, String desc, String sig, String[] exc) {
                MethodVisitor mv = super.visitMethod(acc, name, desc, sig, exc);
                if (!name.equals("SetTo") || !desc.equals("(L" + PORT_THEME + ";)Z")) return mv;
                return new MethodVisitor(Opcodes.ASM9, mv) {
                    int seen;      // the `new ThemePackage` instructions met
                    int skipping;  // its DUP, ACONST_NULL (the synthetic accessor's argument), <init>
                    @Override public void visitTypeInsn(int op, String type) {
                        if (op == Opcodes.NEW && type.equals(PORT_THEME_PACKAGE) && seen++ == 0) {
                            super.visitInsn(Opcodes.ACONST_NULL);
                            skipping = 3;
                            count("empty theme packages Theme.SetTo made left null, as AssetManager2 does");
                            return;
                        }
                        super.visitTypeInsn(op, type);
                    }
                    @Override public void visitInsn(int op) {
                        if (skipping > 1 && (op == Opcodes.DUP || op == Opcodes.ACONST_NULL)) {
                            skipping--;
                            return;
                        }
                        super.visitInsn(op);
                    }
                    @Override public void visitMethodInsn(int op, String owner, String n, String d, boolean itf) {
                        if (skipping == 1 && op == Opcodes.INVOKESPECIAL && owner.equals(PORT_THEME_PACKAGE)) {
                            skipping = 0;
                            return;
                        }
                        super.visitMethodInsn(op, owner, n, d, itf);
                    }
                };
            }
        };
    }

    static String bridgeOf(String cls) {
        return BRIDGE_PKG + "Native" + cls.substring(cls.lastIndexOf('/') + 1);
    }

    static Set<String> bridgeMethods(Path root, String cls) throws IOException {
        Path f = root.resolve(bridgeOf(cls) + ".class");
        Set<String> have = new HashSet<>();
        if (!Files.exists(f)) return have;
        new ClassReader(Files.readAllBytes(f)).accept(new ClassVisitor(Opcodes.ASM9) {
            @Override public MethodVisitor visitMethod(int acc, String n, String d, String s, String[] e) {
                if ((acc & Opcodes.ACC_STATIC) != 0 && (acc & Opcodes.ACC_PUBLIC) != 0) have.add(n + d);
                return null;
            }
        }, ClassReader.SKIP_CODE);
        return have;
    }

    static final class Rewriter extends ClassVisitor {
        final String cls;
        final Set<String> bridge;

        Rewriter(ClassVisitor cv, String cls, Set<String> bridge) {
            super(Opcodes.ASM9, cv);
            this.cls = cls;
            this.bridge = bridge;
        }

        @Override
        public MethodVisitor visitMethod(int acc, String name, String desc, String sig, String[] exc) {
            if (bridge != null && (acc & Opcodes.ACC_NATIVE) != 0) {
                boolean isStatic = (acc & Opcodes.ACC_STATIC) != 0;
                MethodVisitor mv = super.visitMethod(acc & ~Opcodes.ACC_NATIVE, name, desc, sig, exc);
                mv.visitCode();
                String bdesc = isStatic ? desc : "(L" + cls + ";" + desc.substring(1);
                if (bridge.contains(name + bdesc)) {
                    int slot = 0;
                    if (!isStatic) mv.visitVarInsn(Opcodes.ALOAD, slot++);
                    for (Type t : Type.getArgumentTypes(desc)) {
                        mv.visitVarInsn(t.getOpcode(Opcodes.ILOAD), slot);
                        slot += t.getSize();
                    }
                    mv.visitMethodInsn(Opcodes.INVOKESTATIC, bridgeOf(cls), name, bdesc, false);
                    mv.visitInsn(Type.getReturnType(desc).getOpcode(Opcodes.IRETURN));
                    count("natives given Java bodies");
                } else {
                    mv.visitTypeInsn(Opcodes.NEW, "java/lang/UnsupportedOperationException");
                    mv.visitInsn(Opcodes.DUP);
                    mv.visitLdcInsn("not implemented on HotSpot: " + cls.replace('/', '.') + "." + name + desc);
                    mv.visitMethodInsn(Opcodes.INVOKESPECIAL, "java/lang/UnsupportedOperationException", "<init>",
                            "(Ljava/lang/String;)V", false);
                    mv.visitInsn(Opcodes.ATHROW);
                    System.out.println("[aosp-res]   no body for " + cls + "." + name + desc);
                }
                mv.visitMaxs(0, 0);
                mv.visitEnd();
                return null;
            }
            if (cls.equals("android/graphics/ImageDecoder") && name.equals("createFromAsset")
                    && desc.equals("(L" + ASSET_STREAM + ";ZLandroid/graphics/ImageDecoder$Source;)Landroid/graphics/ImageDecoder;")) {
                // An asset is read as the stream it also is: createFromStream(ais, true, preferAnimation,
                // source), which owns and closes it, as createFromAsset's decoder would.
                MethodVisitor mv = super.visitMethod(acc, name, desc, sig, exc);
                mv.visitCode();
                mv.visitVarInsn(Opcodes.ALOAD, 0);
                mv.visitInsn(Opcodes.ICONST_1);
                mv.visitVarInsn(Opcodes.ILOAD, 1);
                mv.visitVarInsn(Opcodes.ALOAD, 2);
                mv.visitMethodInsn(Opcodes.INVOKESTATIC, cls, "createFromStream",
                        "(Ljava/io/InputStream;ZZLandroid/graphics/ImageDecoder$Source;)Landroid/graphics/ImageDecoder;", false);
                mv.visitInsn(Opcodes.ARETURN);
                mv.visitMaxs(0, 0);
                mv.visitEnd();
                count("ImageDecoder.createFromAsset read as a stream");
                return null;
            }
            final boolean graphics = cls.startsWith("android/graphics/");
            return new MethodVisitor(Opcodes.ASM9, super.visitMethod(acc, name, desc, sig, exc)) {
                @Override public void visitTypeInsn(int op, String type) {
                    if (graphics && op == Opcodes.INSTANCEOF && type.equals(ASSET_STREAM)) {
                        super.visitInsn(Opcodes.POP);
                        super.visitInsn(Opcodes.ICONST_0);
                        count("graphics tests for a native asset made false");
                        return;
                    }
                    if (op == Opcodes.CHECKCAST && type.equals(PARSER)) {
                        super.visitMethodInsn(Opcodes.INVOKESTATIC, BRIDGE_PKG + "AttrSets", "parser",
                                "(Ljava/lang/Object;)L" + PARSER + ";", false);
                        count("casts to XmlBlock.Parser through AttrSets.parser");
                        return;
                    }
                    super.visitTypeInsn(op, type);
                }

                @Override public void visitMethodInsn(int op, String owner, String n, String d, boolean itf) {
                    if (owner.equals("android/app/ActivityThread") && n.equals("getSystemContext")
                            && d.equals("()Landroid/app/ContextImpl;")) {
                        // An AIDL Stub's default constructor (IPlatformCompat's): Win.apk has no
                        // ContextImpl, so the class did not link; a Context instead.
                        super.visitMethodInsn(Opcodes.INVOKESTATIC, COMPAT, "systemContext",
                                "(Landroid/app/ActivityThread;)Landroid/content/Context;", false);
                        count("calls to ActivityThread.getSystemContext answered with a Context");
                        return;
                    }
                    String key = owner + "." + n + d;
                    String to = STATIC_CALLS.get(key);
                    if (op == Opcodes.INVOKESTATIC && to != null) {
                        super.visitMethodInsn(op, to, n, d, false);
                        count("calls to " + owner.substring(owner.lastIndexOf('/') + 1) + "." + n + " redirected");
                        return;
                    }
                    String inst = INSTANCE_CALLS.get(key);
                    if (op == Opcodes.INVOKEVIRTUAL && inst != null) {
                        super.visitMethodInsn(Opcodes.INVOKESTATIC, COMPAT, inst, "(L" + owner + ";" + d.substring(1), false);
                        count("calls to " + owner.substring(owner.lastIndexOf('/') + 1) + "." + n + " redirected");
                        return;
                    }
                    super.visitMethodInsn(op, owner, n, d, itf);
                }

                @Override public void visitFieldInsn(int op, String owner, String n, String d) {
                    String to = STATIC_FIELDS.get(owner + "." + n);
                    if (op == Opcodes.GETSTATIC && to != null) {
                        super.visitFieldInsn(op, to, n, d);
                        count("reads of " + owner.substring(owner.lastIndexOf('/') + 1) + "." + n + " redirected");
                        return;
                    }
                    if (owner.equals("android/util/DisplayMetrics")) {
                        String plain = RENAMED_FIELDS.get(n);
                        if (plain != null) {
                            super.visitFieldInsn(op, owner, plain, d);
                            count("DisplayMetrics.noncompat* accesses to the plain fields");
                            return;
                        }
                        if (n.equals("fontScaleConverter")) {
                            if (op == Opcodes.GETFIELD) {
                                super.visitMethodInsn(Opcodes.INVOKESTATIC, COMPAT, "fontScaleConverter",
                                        "(Landroid/util/DisplayMetrics;)" + d, false);
                            } else {
                                super.visitMethodInsn(Opcodes.INVOKESTATIC, COMPAT, "setFontScaleConverter",
                                        "(Landroid/util/DisplayMetrics;" + d + ")V", false);
                            }
                            count("DisplayMetrics.fontScaleConverter accesses through Compat");
                            return;
                        }
                    }
                    super.visitFieldInsn(op, owner, n, d);
                }
            };
        }
    }
}
