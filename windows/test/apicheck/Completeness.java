// Completeness: which methods of Win.apk's framework are missing, or present but empty, next to
// Android's own -- weighted by how much the libraries apps are built from call them.
//
//   java Completeness.java <win framework.jar> <android-all.jar> <out.tsv> [<library or app jar>...]
//
// <win framework.jar> is the runtime's lib/android-framework.jar. <android-all.jar> is Android's
// framework compiled for the JVM: Robolectric's org.robolectric:android-all (not the
// -instrumented one, whose bodies Robolectric rewrote), for example
// android-all-14-robolectric-10818077.jar from Maven Central.
//
// Every method of a public android.* class in Android's jar is looked up, by name and
// descriptor, in Win.apk's class of the same name (or up its superclasses). It is:
//   missing  no such method in Win.apk (a NoSuchMethodError when it is called)
//   constant Win.apk's body returns a value without reading any state or calling anything (a
//            constant, a parameter, `this`), or only throws or logs, while Android's does more:
//            a stub that answers 0, null or false -- the kind that misleads its caller
//   noop     a void method whose Win.apk body does nothing (or only logs) while Android's does
//            something: sometimes harmless (a pool's recycle), sometimes a lost effect
// A body that reads a field is state, not a stub, and is not listed; nor is a constructor unless
// Android's is big (40 bytes or more).
//   shallow  Win.apk's body is under a tenth of Android's (bytecode bytes) and Android's is big:
//            often a partial reimplementation, sometimes only a tighter one
// A method Android leaves abstract or native is skipped, and so are hidden members Android
// marks synthetic.
//
// The optional jars are what calls the framework: AndroidX and Material's classes.jar, or apps
// as the DEX converter left them. Each method reference in their constant pools counts one use
// of the framework method it names, so the list is ordered by what the code apps are built from
// actually reaches, then by the size of Android's body. Without them the order is by size only.
//
// Needs JDK 24 or later (java.lang.classfile).
import java.io.IOException;
import java.io.PrintWriter;
import java.lang.classfile.*;
import java.lang.classfile.constantpool.MemberRefEntry;
import java.lang.classfile.constantpool.PoolEntry;
import java.lang.classfile.attribute.CodeAttribute;
import java.lang.classfile.instruction.*;
import java.lang.reflect.AccessFlag;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class Completeness {
    record Body(int bytes, boolean empty) {}
    record Cls(String superName, List<String> ifaces, Map<String, Body> methods, boolean isPublic) {}

    static Map<String, Cls> read(Path jar, boolean androidOnly) throws IOException {
        Map<String, Cls> out = new HashMap<>();
        try (ZipFile z = new ZipFile(jar.toFile())) {
            for (ZipEntry e : Collections.list(z.entries())) {
                String n = e.getName();
                if (!n.endsWith(".class") || n.startsWith("META-INF/")) continue;
                if (androidOnly && !n.startsWith("android/")) continue;
                ClassModel cm;
                try (var in = z.getInputStream(e)) {
                    cm = ClassFile.of().parse(in.readAllBytes());
                } catch (Throwable t) {
                    continue;
                }
                Map<String, Body> ms = new HashMap<>();
                for (MethodModel m : cm.methods()) {
                    if (m.flags().has(AccessFlag.SYNTHETIC) || m.flags().has(AccessFlag.BRIDGE)) continue;
                    String key = m.methodName().stringValue() + m.methodType().stringValue();
                    Optional<CodeModel> code = m.code();
                    final boolean ctor = key.startsWith("<init>");
                    ms.put(key, code.map(c -> body(c, ctor)).orElse(null));
                }
                List<String> ifs = new ArrayList<>();
                cm.interfaces().forEach(i -> ifs.add(i.asInternalName()));
                out.put(cm.thisClass().asInternalName(), new Cls(
                    cm.superclass().map(s -> s.asInternalName()).orElse(null), ifs, ms,
                    cm.flags().has(AccessFlag.PUBLIC)));
            }
        }
        return out;
    }

    // A body that only loads constants, locals and fields, returns, throws, or calls a logger.
    static boolean throwable(String internalName) {
        return internalName.endsWith("Exception") || internalName.endsWith("Error");
    }

    static Body body(CodeModel code, boolean ctor) {
        int bytes = code instanceof CodeAttribute ca ? ca.codeLength() : 0;
        boolean empty = true;
        for (CodeElement el : code) {
            if (!(el instanceof Instruction ins)) continue;
            if (ins instanceof LoadInstruction || ins instanceof ConstantInstruction
                    || ins instanceof ReturnInstruction || ins instanceof ThrowInstruction
                    || ins instanceof StackInstruction || ins instanceof NopInstruction
                    || ins instanceof TypeCheckInstruction) {
                continue;
            }
            // new <Exception> before a throw; any other new object is work.
            if (ins instanceof NewObjectInstruction no && throwable(no.className().asInternalName())) continue;
            if (ins instanceof InvokeInstruction inv) {
                String owner = inv.owner().asInternalName();
                String name = inv.name().stringValue();
                // A constructor's call of its super's (`this`'s class is not known here: any
                // <init> through invokespecial on aload_0 would need the stack; a throwable's,
                // or the superclass call of a constructor, is what a stub body holds).
                if (owner.equals("com/macapk/loader/NativeLog") || owner.equals("android/util/Log")
                        || (name.equals("<init>") && (ctor || throwable(owner)))
                        || owner.startsWith("java/lang/StringBuilder")
                        || owner.equals("java/lang/String")) {
                    continue;
                }
            }
            empty = false;
            break;
        }
        return new Body(bytes, empty);
    }

    // Win.apk's body for a method, looking up the class's superclasses and interfaces.
    static Body find(Map<String, Cls> win, String cls, String key, Set<String> seen, boolean[] found) {
        for (String c = cls; c != null && seen.add(c); ) {
            Cls k = win.get(c);
            if (k == null) return null;
            if (k.methods().containsKey(key)) {
                found[0] = true;
                return k.methods().get(key);
            }
            for (String i : k.ifaces()) {
                Body b = find(win, i, key, seen, found);
                if (found[0]) return b;
            }
            c = k.superName();
        }
        return null;
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 3) {
            System.err.println("usage: java Completeness.java <win framework.jar> <android-all.jar> <out.tsv> [jars...]");
            System.exit(2);
        }
        Map<String, Cls> win = read(Path.of(args[0]), false);
        Map<String, Cls> aosp = read(Path.of(args[1]), true);

        // Uses: every method reference in the given jars, by "owner.name(desc)".
        Map<String, Integer> uses = new HashMap<>();
        for (int i = 3; i < args.length; i++) {
            try (ZipFile z = new ZipFile(args[i])) {
                for (ZipEntry e : Collections.list(z.entries())) {
                    if (!e.getName().endsWith(".class")) continue;
                    try (var in = z.getInputStream(e)) {
                        ClassModel cm = ClassFile.of().parse(in.readAllBytes());
                        for (PoolEntry pe : cm.constantPool()) {
                            if (pe instanceof MemberRefEntry mr && !(pe instanceof java.lang.classfile.constantpool.FieldRefEntry)) {
                                String o = mr.owner().asInternalName();
                                if (!o.startsWith("android/")) continue;
                                uses.merge(o + "." + mr.name().stringValue() + mr.type().stringValue(), 1, Integer::sum);
                            }
                        }
                    } catch (Throwable ignored) {}
                }
            }
        }

        record Row(String kind, String cls, String key, int winBytes, int aospBytes, int uses) {}
        List<Row> rows = new ArrayList<>();
        int checked = 0;
        for (var en : aosp.entrySet()) {
            String cls = en.getKey();
            Cls a = en.getValue();
            if (!a.isPublic() || cls.contains("$$")) continue;
            if (!win.containsKey(cls)) continue;  // whole missing classes: ApiCheck's list
            for (var m : a.methods().entrySet()) {
                String key = m.getKey();
                Body ab = m.getValue();
                if (ab == null || key.startsWith("<clinit>")) continue;  // abstract or native in Android
                checked++;
                boolean[] found = {false};
                Body wb = find(win, cls, key, new HashSet<>(), found);
                int u = uses.getOrDefault(cls + "." + key, 0);
                String kind = null;
                if (!found[0]) kind = "missing";
                else if (wb == null) continue;  // native or abstract in Win.apk: the host's
                else if (wb.empty() && !ab.empty()
                        && (!key.startsWith("<init>") || ab.bytes() >= 40)) {
                    kind = key.endsWith(")V") ? "noop" : "constant";
                }
                else if (ab.bytes() >= 200 && wb.bytes() * 10 < ab.bytes()) kind = "shallow";
                if (kind != null) rows.add(new Row(kind, cls, key, wb == null ? 0 : wb.bytes(), ab.bytes(), u));
            }
        }
        rows.sort(Comparator.comparingInt(Row::uses).reversed()
            .thenComparing(Comparator.comparingInt(Row::aospBytes).reversed()));
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(Path.of(args[2]), StandardCharsets.UTF_8))) {
            w.println("uses\tkind\tclass\tmethod\twin_bytes\tandroid_bytes");
            for (Row r : rows) {
                w.println(r.uses() + "\t" + r.kind() + "\t" + r.cls().replace('/', '.') + "\t" + r.key() + "\t"
                    + r.winBytes() + "\t" + r.aospBytes());
            }
        }
        Map<String, Integer> byKind = new TreeMap<>();
        Map<String, Integer> usedByKind = new TreeMap<>();
        for (Row r : rows) {
            byKind.merge(r.kind(), 1, Integer::sum);
            if (r.uses() > 0) usedByKind.merge(r.kind(), 1, Integer::sum);
        }
        System.out.println("[completeness] " + checked + " methods of classes both have; by kind: " + byKind
            + "; of those the given jars call: " + usedByKind + " -> " + args[2]);
    }
}
