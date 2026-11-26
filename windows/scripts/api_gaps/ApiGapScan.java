// ApiGapScan.java -- what of Android's public API the shipped runtime does not have, or has only as a stub
// (docs/research/api-gaps.md). An app is compiled against android.jar; at run time every class, method and
// field it names must resolve on Win.apk's class path (lib/aosp-app.jar, aosp-views, aosp-resources,
// aosp-graphics, android-framework.jar, macapk-loader.jar, bcprov; java.* from the JRE with the
// java.base overlay first), or HotSpot throws NoClassDefFoundError / NoSuchMethodError / NoSuchFieldError
// where the app calls it. LinkScan.java checks the runtime's own jars against each other; this checks
// them against the SDK: every public or protected class, method and field of android.jar (34, 35 and
// 36, each member with the level api-versions.xml says it was added), resolved as the JVM resolves it.
// A member that resolves is then judged by its body: one that throws UnsupportedOperationException,
// returns a constant (null, 0, false, an empty collection) or does nothing, where android-all 14's body
// (AOSP's real implementation, Robolectric's jar) does more, is a stub.
//
//   java -cp asm.jar ApiGapScan.java --out <dir> --versions <api-versions.xml> --api <android.jar>...
//        --jre <jre home> [--patch <dir>]... [--aosp <android-all.jar>] [--refs <refs.tsv>] <runtime jar>...
//
// The runtime jars in class path order (runtime_main.cpp jvm_options). --patch directories hold the JRE
// overlay's classes (lib/java_base_patch, lib/jdk_unsupported_patch), looked up before the JRE's own.
// --jre is the shipped jre/ (its lib/modules is read through jrt-fs with java.home pointed at it).
// Writes, tab-separated, one line each (level = the API level the item was added, from api-versions.xml):
//
//   classes.tsv   level  class                                     a public class nothing on the path has
//   members.tsv   level  kind  class  name  desc  problem            missing | static-mismatch | not-public |
//                                                                    abstract (abstract here, not in the SDK) |
//                                                                    missing-constant (a compile-time constant:
//                                                                    javac copies its value into the app, so only
//                                                                    reflection or a runtime caller misses it)
//   supers.tsv    level  class  supertype                          a supertype the SDK class has, Win.apk's not
//   stubs.tsv     level  kind  class  name  desc  stub  jar          throws | returns-const | empty | says-
//                                                                    unsupported; android-all's body does more
//   apprefs.tsv   problem  kind  class  name  desc  level|hidden  apps  callers
//                 with --refs (dex_refs.py's output): each class, method and field the apps' code uses that
//                 does not resolve (no-class, no-method, no-field; no-member for a reflective lookup by
//                 name) or resolves to a stub (stub-<kind>), SDK or hidden
//   summary.txt   counts per level and per top-level package
//
// Exit status 0; the numbers are for the document, not a gate.

import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassVisitor;
import org.objectweb.asm.FieldVisitor;
import org.objectweb.asm.MethodVisitor;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.tree.AbstractInsnNode;
import org.objectweb.asm.tree.ClassNode;
import org.objectweb.asm.tree.InsnList;
import org.objectweb.asm.tree.LdcInsnNode;
import org.objectweb.asm.tree.MethodInsnNode;
import org.objectweb.asm.tree.MethodNode;
import org.objectweb.asm.tree.TypeInsnNode;
import org.xml.sax.Attributes;
import org.xml.sax.helpers.DefaultHandler;

import javax.xml.parsers.SAXParserFactory;
import java.io.IOException;
import java.io.InputStream;
import java.io.PrintWriter;
import java.net.URI;
import java.nio.file.FileSystem;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class ApiGapScan {
    // ---- a class path: jars, directories, then the JRE's modules -------------------------------------
    static final class ClassPath {
        final List<ZipFile> jars = new ArrayList<>();
        final List<String> jarNames = new ArrayList<>();
        final List<Path> dirs = new ArrayList<>();
        FileSystem jrt;
        final Map<String, Path> jrtIndex = new HashMap<>();  // internal name -> path in the jrt image

        void jre(Path home) throws IOException {
            jrt = FileSystems.newFileSystem(URI.create("jrt:/"), Map.of("java.home", home.toString()));
            try (var mods = Files.list(jrt.getPath("/modules"))) {
                for (Path m : (Iterable<Path>) mods::iterator) {
                    try (var w = Files.walk(m)) {
                        w.filter(p -> p.toString().endsWith(".class")).forEach(p -> {
                            String rel = m.relativize(p).toString();
                            jrtIndex.putIfAbsent(rel.substring(0, rel.length() - 6), p);
                        });
                    }
                }
            }
        }

        /** The class file's bytes and where they came from, first entry first, as the JVM finds it. */
        Object[] find(String name) {
            String file = name + ".class";
            // The overlay's directories are --patch-module: they come before the JRE's own java.* classes,
            // and a JRE class (java.*, javax.*, org.w3c...) is never taken from a jar on the class path.
            try {
                for (Path d : dirs) {
                    Path p = d.resolve(file);
                    if (Files.isRegularFile(p)) return new Object[] {Files.readAllBytes(p), "patch:" + d.getFileName()};
                }
                Path j = jrtIndex.get(name);
                if (j != null) return new Object[] {Files.readAllBytes(j), "jre"};
                for (int i = 0; i < jars.size(); i++) {
                    ZipEntry e = jars.get(i).getEntry(file);
                    if (e == null) continue;
                    try (InputStream in = jars.get(i).getInputStream(e)) {
                        return new Object[] {in.readAllBytes(), jarNames.get(i)};
                    }
                }
            } catch (IOException ex) {
                throw new RuntimeException(ex);
            }
            return null;
        }
    }

    static final class Member {
        final String name, desc;
        final int access;
        boolean constant;  // a static final with a ConstantValue: javac copies it into the app, no link
        Member(String name, String desc, int access) { this.name = name; this.desc = desc; this.access = access; }
    }

    static final class Info {
        String name, superName, jar;
        String[] interfaces = new String[0];
        int access;
        final Map<String, Member> methods = new LinkedHashMap<>();  // name+desc
        final Map<String, Member> fields = new LinkedHashMap<>();   // name:desc
        ClassNode node;  // with code, for the stub test (runtime classes only)
    }

    static Info read(byte[] b, String jar, boolean code) {
        Info n = new Info();
        n.jar = jar;
        ClassNode cn = new ClassNode();
        new ClassReader(b).accept(cn, code ? ClassReader.SKIP_FRAMES : ClassReader.SKIP_CODE);
        n.name = cn.name;
        n.superName = cn.superName;
        n.interfaces = cn.interfaces.toArray(new String[0]);
        n.access = cn.access;
        for (MethodNode m : cn.methods) n.methods.put(m.name + m.desc, new Member(m.name, m.desc, m.access));
        for (var f : cn.fields) {
            Member m = new Member(f.name, f.desc, f.access);
            m.constant = f.value != null && (f.access & Opcodes.ACC_STATIC) != 0 && (f.access & Opcodes.ACC_FINAL) != 0;
            n.fields.put(f.name + ":" + f.desc, m);
        }
        if (code) n.node = cn;
        return n;
    }

    static final class Loader {
        final ClassPath cp;
        final boolean code;
        final Map<String, Info> cache = new HashMap<>();
        final Set<String> absent = new HashSet<>();
        Loader(ClassPath cp, boolean code) { this.cp = cp; this.code = code; }

        Info get(String name) {
            if (name == null) return null;
            if (name.startsWith("[")) name = "java/lang/Object";
            Info i = cache.get(name);
            if (i != null || absent.contains(name)) return i;
            Object[] f = cp.find(name);
            if (f == null) { absent.add(name); return null; }
            i = read((byte[]) f[0], (String) f[1], code);
            cache.put(name, i);
            return i;
        }

        /** JVMS 5.4.3.3: the class, its superclasses, then every superinterface. */
        Object[] method(String owner, String nd) {
            for (Info c = get(owner); c != null; c = get(c.superName)) {
                Member m = c.methods.get(nd);
                if (m != null) return new Object[] {c, m};
            }
            Deque<String> todo = new ArrayDeque<>();
            Set<String> seen = new HashSet<>();
            for (Info c = get(owner); c != null; c = get(c.superName)) todo.addAll(List.of(c.interfaces));
            while (!todo.isEmpty()) {
                Info c = get(todo.poll());
                if (c == null || !seen.add(c.name)) continue;
                Member m = c.methods.get(nd);
                if (m != null && (m.access & Opcodes.ACC_STATIC) == 0 && (m.access & Opcodes.ACC_PRIVATE) == 0)
                    return new Object[] {c, m};
                todo.addAll(List.of(c.interfaces));
            }
            return null;
        }

        /** JVMS 5.4.3.2: the class, its superinterfaces, then its superclass, recursively. */
        Object[] field(String owner, String nd) {
            Info c = get(owner);
            if (c == null) return null;
            Member m = c.fields.get(nd);
            if (m != null) return new Object[] {c, m};
            for (String i : c.interfaces) {
                Object[] r = field(i, nd);
                if (r != null) return r;
            }
            return c.superName == null ? null : field(c.superName, nd);
        }

        Set<String> supertypes(String name) {
            Set<String> out = new HashSet<>();
            Deque<String> todo = new ArrayDeque<>(List.of(name));
            while (!todo.isEmpty()) {
                Info c = get(todo.poll());
                if (c == null || !out.add(c.name)) continue;
                if (c.superName != null) todo.add(c.superName);
                todo.addAll(List.of(c.interfaces));
            }
            return out;
        }
    }

    // ---- api-versions.xml: the level each class and member was added ---------------------------------
    static final Map<String, Integer> since = new HashMap<>();  // "cls", "cls#name(desc)", "cls#name"
    static final Map<String, Integer> removed = new HashMap<>();

    static void readVersions(Path xml) throws Exception {
        SAXParserFactory.newInstance().newSAXParser().parse(xml.toFile(), new DefaultHandler() {
            String cls;
            int clsSince;
            @Override public void startElement(String uri, String local, String q, Attributes a) {
                String s = a.getValue("since"), r = a.getValue("removed");
                if (q.equals("class")) {
                    cls = a.getValue("name");
                    clsSince = s == null ? 1 : Integer.parseInt(s);
                    since.put(cls, clsSince);
                    if (r != null) removed.put(cls, Integer.parseInt(r));
                } else if ((q.equals("method") || q.equals("field")) && cls != null) {
                    String key = cls + "#" + a.getValue("name");
                    since.put(key, s == null ? clsSince : Integer.parseInt(s));
                    if (r != null) removed.put(key, Integer.parseInt(r));
                }
            }
        });
    }

    static int level(String cls, String member) {
        Integer l = member == null ? since.get(cls) : since.get(cls + "#" + member);
        if (l == null && member != null) l = since.get(cls);
        return l == null ? 0 : l;
    }

    // ---- the stub test --------------------------------------------------------------------------------
    static final Pattern UNSUPPORTED = Pattern.compile(
            "(?i)\\bstub\\b|not (yet )?(implemented|supported)|unsupported|no-op on windows|not on windows");

    static List<AbstractInsnNode> real(InsnList l) {
        List<AbstractInsnNode> out = new ArrayList<>();
        for (AbstractInsnNode n : l) if (n.getOpcode() >= 0) out.add(n);
        return out;
    }

    /** What a body amounts to, or null when it does real work. */
    static String stubKind(MethodNode m) {
        if ((m.access & (Opcodes.ACC_ABSTRACT | Opcodes.ACC_NATIVE)) != 0 || m.instructions == null) return null;
        List<AbstractInsnNode> in = real(m.instructions);
        if (in.isEmpty()) return null;
        for (AbstractInsnNode n : in) {
            if (n instanceof TypeInsnNode t && t.getOpcode() == Opcodes.NEW
                    && t.desc.equals("java/lang/UnsupportedOperationException") && in.size() <= 12) return "throws";
        }
        if (in.size() <= 12) {
            for (AbstractInsnNode n : in) {
                if (n instanceof LdcInsnNode c && c.cst instanceof String s && UNSUPPORTED.matcher(s).find())
                    return "says-unsupported";
            }
        }
        // A constant: [aconst_null|xconst|bipush|sipush|ldc] xreturn, or return alone, maybe after
        // loading `this`/arguments that nothing uses; or Collections.emptyX()/List.of() areturn.
        int i = 0;
        while (i < in.size() && in.get(i).getOpcode() >= Opcodes.ILOAD && in.get(i).getOpcode() <= Opcodes.ALOAD
                && i + 1 < in.size() && in.get(i + 1).getOpcode() == Opcodes.POP) i += 2;
        List<AbstractInsnNode> rest = in.subList(i, in.size());
        if (rest.size() == 1 && rest.get(0).getOpcode() == Opcodes.RETURN) return "empty";
        if (rest.size() == 2) {
            int op = rest.get(0).getOpcode(), ret = rest.get(1).getOpcode();
            boolean isConst = op == Opcodes.ACONST_NULL || (op >= Opcodes.ICONST_M1 && op <= Opcodes.DCONST_1)
                    || op == Opcodes.BIPUSH || op == Opcodes.SIPUSH || op == Opcodes.LDC;
            if (isConst && ret >= Opcodes.IRETURN && ret <= Opcodes.ARETURN) return "returns-const";
            if (rest.get(0) instanceof MethodInsnNode mi && ret == Opcodes.ARETURN && mi.getOpcode() == Opcodes.INVOKESTATIC
                    && ((mi.owner.equals("java/util/Collections") && mi.name.startsWith("empty"))
                        || (mi.name.equals("of") && mi.desc.startsWith("()"))))
                return "returns-const";
        }
        // new ArrayList<>() and nothing else: an empty collection (a new framework object is real work)
        if (rest.size() == 4 && rest.get(0) instanceof TypeInsnNode nt && nt.getOpcode() == Opcodes.NEW
                && nt.desc.startsWith("java/util/") && rest.get(1).getOpcode() == Opcodes.DUP
                && rest.get(2) instanceof MethodInsnNode c && c.name.equals("<init>") && c.desc.equals("()V")
                && rest.get(3).getOpcode() == Opcodes.ARETURN) return "returns-const";
        return null;
    }

    // ---- main ----------------------------------------------------------------------------------------
    public static void main(String[] a) throws Exception {
        Path out = null, versions = null, jre = null, aosp = null, refs = null;
        List<Path> api = new ArrayList<>();
        ClassPath rt = new ClassPath();
        for (int i = 0; i < a.length; i++) {
            switch (a[i]) {
                case "--out" -> out = Path.of(a[++i]);
                case "--versions" -> versions = Path.of(a[++i]);
                case "--api" -> api.add(Path.of(a[++i]));
                case "--jre" -> jre = Path.of(a[++i]);
                case "--patch" -> rt.dirs.add(Path.of(a[++i]));
                case "--aosp" -> aosp = Path.of(a[++i]);
                case "--refs" -> refs = Path.of(a[++i]);
                default -> {
                    rt.jars.add(new ZipFile(a[i]));
                    rt.jarNames.add(Path.of(a[i]).getFileName().toString());
                }
            }
        }
        if (out == null || versions == null || api.isEmpty() || jre == null)
            throw new IllegalArgumentException("usage: see the header");
        Files.createDirectories(out);
        readVersions(versions);
        rt.jre(jre);
        Loader run = new Loader(rt, true);
        Loader ref = null;
        if (aosp != null) {
            ClassPath r = new ClassPath();
            r.jars.add(new ZipFile(aosp.toFile()));
            r.jarNames.add("android-all");
            ref = new Loader(r, true);
        }

        // The SDK: the newest jar first; an older jar adds what a newer one dropped.
        Map<String, Info> sdk = new TreeMap<>();
        for (int k = api.size() - 1; k >= 0; k--) {
            try (ZipFile z = new ZipFile(api.get(k).toFile())) {
                for (ZipEntry e : java.util.Collections.list(z.entries())) {
                    if (!e.getName().endsWith(".class")) continue;
                    Info i;
                    try (InputStream in = z.getInputStream(e)) { i = read(in.readAllBytes(), "sdk", false); }
                    if ((i.access & (Opcodes.ACC_PUBLIC | Opcodes.ACC_PROTECTED)) == 0) continue;
                    Info have = sdk.get(i.name);
                    if (have == null) sdk.put(i.name, i);
                    else { i.methods.forEach(have.methods::putIfAbsent); i.fields.forEach(have.fields::putIfAbsent); }
                }
            }
        }

        Map<String, int[]> perPkg = new TreeMap<>();  // pkg -> classes, members, stubs, api classes
        Map<Integer, int[]> perLevel = new TreeMap<>();
        try (PrintWriter cls = new PrintWriter(Files.newBufferedWriter(out.resolve("classes.tsv")));
             PrintWriter mem = new PrintWriter(Files.newBufferedWriter(out.resolve("members.tsv")));
             PrintWriter sup = new PrintWriter(Files.newBufferedWriter(out.resolve("supers.tsv")));
             PrintWriter stb = new PrintWriter(Files.newBufferedWriter(out.resolve("stubs.tsv")))) {
            for (Info s : sdk.values()) {
                String pkg = s.name.contains("/") ? s.name.substring(0, s.name.lastIndexOf('/')) : "";
                int[] pc = perPkg.computeIfAbsent(pkg, k -> new int[4]);
                pc[3]++;
                int cl = level(s.name, null);
                Info r = run.get(s.name);
                if (r == null) {
                    cls.println(cl + "\t" + s.name);
                    pc[0]++;
                    perLevel.computeIfAbsent(cl, k -> new int[3])[0]++;
                    continue;
                }
                Set<String> have = run.supertypes(s.name);
                for (String t : sdkSupertypes(sdk, s.name)) {
                    if (!have.contains(t) && !t.equals("java/lang/Object")) sup.println(cl + "\t" + s.name + "\t" + t);
                }
                for (Member m : s.methods.values()) {
                    if ((m.access & (Opcodes.ACC_PUBLIC | Opcodes.ACC_PROTECTED)) == 0) continue;
                    if ((m.access & (Opcodes.ACC_SYNTHETIC | Opcodes.ACC_BRIDGE)) != 0) continue;
                    if (m.name.equals("<clinit>")) continue;
                    int ml = level(s.name, m.name + m.desc);
                    Object[] f = m.name.equals("<init>") ? ctor(run, s.name, m.desc) : run.method(s.name, m.name + m.desc);
                    String problem = null;
                    Member rm = f == null ? null : (Member) f[1];
                    if (rm == null) problem = "missing";
                    else if (((rm.access ^ m.access) & Opcodes.ACC_STATIC) != 0) problem = "static-mismatch";
                    else if ((rm.access & (Opcodes.ACC_PUBLIC | Opcodes.ACC_PROTECTED)) == 0) problem = "not-public";
                    else if ((rm.access & Opcodes.ACC_ABSTRACT) != 0 && (m.access & Opcodes.ACC_ABSTRACT) == 0
                            && (s.access & Opcodes.ACC_INTERFACE) == 0) problem = "abstract";
                    if (problem != null) {
                        mem.println(ml + "\tM\t" + s.name + "\t" + m.name + "\t" + m.desc + "\t" + problem);
                        pc[1]++;
                        perLevel.computeIfAbsent(ml, k -> new int[3])[1]++;
                        continue;
                    }
                    String[] st = stubOf(run, ref, f, s.name, m.name, m.desc);
                    if (st == null) continue;
                    stb.println(ml + "\tM\t" + s.name + "\t" + m.name + "\t" + m.desc + "\t" + st[0] + "\t" + st[1]);
                    pc[2]++;
                    perLevel.computeIfAbsent(ml, k -> new int[3])[2]++;
                }
                for (Member m : s.fields.values()) {
                    if ((m.access & (Opcodes.ACC_PUBLIC | Opcodes.ACC_PROTECTED)) == 0) continue;
                    if ((m.access & Opcodes.ACC_SYNTHETIC) != 0) continue;
                    int ml = level(s.name, m.name);
                    Object[] f = run.field(s.name, m.name + ":" + m.desc);
                    String problem = null;
                    if (f == null) problem = m.constant ? "missing-constant" : "missing";
                    else if (((((Member) f[1]).access ^ m.access) & Opcodes.ACC_STATIC) != 0) problem = "static-mismatch";
                    if (problem != null) {
                        mem.println(ml + "\tF\t" + s.name + "\t" + m.name + "\t" + m.desc + "\t" + problem);
                        if (problem.equals("missing-constant")) continue;
                        pc[1]++;
                        perLevel.computeIfAbsent(ml, k -> new int[3])[1]++;
                    }
                }
            }
        }
        if (refs != null) appRefs(refs, out, run, ref, sdk);
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out.resolve("summary.txt")))) {
            w.println("# level\tmissing classes\tmissing members\tstubs");
            perLevel.forEach((l, c) -> w.println(l + "\t" + c[0] + "\t" + c[1] + "\t" + c[2]));
            w.println("# package\tSDK classes\tmissing classes\tmissing members\tstubs");
            perPkg.forEach((p, c) -> {
                if (c[0] + c[1] + c[2] > 0) w.println(p + "\t" + c[3] + "\t" + c[0] + "\t" + c[1] + "\t" + c[2]);
            });
        }
    }

    /** {stub kind, where} when the resolved method `f` of cls.name(desc) is a stub: its body throws, returns a
     *  constant or does nothing, and android-all's body of the same method does more (or android-all has
     *  none: a member of 35 or 36 that Win.apk answers with a constant). Null when it does real work. */
    static String[] stubOf(Loader run, Loader ref, Object[] f, String cls, String name, String desc) {
        Info owner = (Info) f[0];
        // The JRE's bodies are OpenJDK's own (AbstractCollection.add throws on Android too): not Win.apk's.
        if (owner.node == null || owner.jar.equals("jre")) return null;
        MethodNode body = null;
        for (MethodNode x : owner.node.methods) if (x.name.equals(name) && x.desc.equals(desc)) body = x;
        String kind = body == null ? null : stubKind(body);
        if (kind == null) return null;
        if (ref != null) {
            Object[] g = name.equals("<init>") ? ctor(ref, cls, desc) : ref.method(cls, name + desc);
            if (g != null && ((Info) g[0]).node != null) {
                MethodNode rb = null;
                for (MethodNode x : ((Info) g[0]).node.methods) if (x.name.equals(name) && x.desc.equals(desc)) rb = x;
                if (rb == null || stubKind(rb) != null || (rb.access & Opcodes.ACC_ABSTRACT) != 0) return null;
                if (real(rb.instructions).size() <= 3) return null;  // AOSP's is just as small
            }
        }
        return new String[] {kind, owner.jar + (owner.name.equals(cls) ? "" : "\tvia " + owner.name)};
    }

    /** --refs: every framework class, method and field the apps' code uses (dex_refs.py), resolved on the
     *  runtime's class path, hidden ones too (the SDK is not the limit of what an app's code names).
     *  Writes apprefs.tsv: problem  kind  class  name  desc  level|hidden  apps  callers. */
    static void appRefs(Path refs, Path out, Loader run, Loader ref, Map<String, Info> sdk) throws IOException {
        Map<String, String[]> rows = new TreeMap<>();  // key -> {problem, kind, cls, name, desc, level}
        Map<String, Set<String>> apps = new HashMap<>(), callers = new HashMap<>();
        for (String line : Files.readAllLines(refs)) {
            String[] t = line.split("\t", -1);
            String k = t[0];
            if (k.equals("APP")) continue;
            String cls = t[1], name = "", desc = "";
            int at;
            if (k.equals("C")) at = 2;
            else if (k.equals("R")) { name = t[2]; at = 3; }
            else { name = t[2]; desc = t[3]; at = 4; }
            String app = t[at], who = t.length > at + 1 ? t[at + 1] : "";
            String key = k + " " + cls + " " + name + " " + desc;
            String[] row = rows.get(key);
            if (row == null) {
                row = resolveRef(run, ref, sdk, k, cls, name, desc);
                rows.put(key, row);
            }
            apps.computeIfAbsent(key, x -> new java.util.TreeSet<>()).add(app);
            for (String c : who.split(",")) if (!c.isEmpty()) callers.computeIfAbsent(key, x -> new java.util.TreeSet<>()).add(c);
        }
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out.resolve("apprefs.tsv")))) {
            rows.forEach((key, r) -> {
                if (r == null) return;
                w.println(String.join("\t", r) + "\t" + String.join(",", apps.get(key)) + "\t"
                        + String.join(",", callers.getOrDefault(key, Set.of())));
            });
        }
    }

    /** {problem, kind, class, name, desc, level or "hidden"} for a reference that fails or reaches a stub,
     *  else null. */
    static String[] resolveRef(Loader run, Loader ref, Map<String, Info> sdk, String k, String cls, String name, String desc) {
        Info s = sdk.get(cls);
        String member = k.equals("M") ? name + desc : k.equals("F") ? name : null;
        boolean inSdk = s != null && (member == null || k.equals("R")
                || (k.equals("M") ? s.methods.containsKey(name + desc) : s.fields.containsKey(name + ":" + desc)));
        String lvl = inSdk ? String.valueOf(level(cls, member)) : "hidden";
        if (k.equals("F") && s != null && !s.fields.containsKey(name + ":" + desc)) {
            // Inherited from an SDK supertype: still in the SDK.
            for (String t : sdkSupertypes(sdk, cls)) {
                Info i = sdk.get(t);
                if (i != null && i.fields.containsKey(name + ":" + desc)) { lvl = String.valueOf(level(t, name)); break; }
            }
        }
        if (k.equals("M") && s != null && !s.methods.containsKey(name + desc)) {
            for (String t : sdkSupertypes(sdk, cls)) {
                Info i = sdk.get(t);
                if (i != null && i.methods.containsKey(name + desc)) { lvl = String.valueOf(level(t, name + desc)); break; }
            }
        }
        Info c = run.get(cls);
        if (c == null) return new String[] {"no-class", k, cls, name, desc, s == null ? "hidden" : String.valueOf(level(cls, null))};
        switch (k) {
            case "C": return null;
            case "R": {
                if (name.equals("-")) return null;
                for (Info x = c; x != null; x = run.get(x.superName)) {
                    for (Member m : x.methods.values()) if (m.name.equals(name)) return null;
                    for (Member m : x.fields.values()) if (m.name.equals(name)) return null;
                }
                return new String[] {"no-member", k, cls, name, desc, lvl};
            }
            case "F": {
                Object[] f = run.field(cls, name + ":" + desc);
                return f == null ? new String[] {"no-field", k, cls, name, desc, lvl} : null;
            }
            default: {
                Object[] f = name.equals("<init>") ? ctor(run, cls, desc) : run.method(cls, name + desc);
                if (f == null) return new String[] {"no-method", k, cls, name, desc, lvl};
                String[] st = stubOf(run, ref, f, cls, name, desc);
                return st == null ? null : new String[] {"stub-" + st[0], k, cls, name, desc, lvl};
            }
        }
    }

    /** A constructor resolves in its own class only (invokespecial <init>). */
    static Object[] ctor(Loader l, String owner, String desc) {
        Info c = l.get(owner);
        if (c == null) return null;
        Member m = c.methods.get("<init>" + desc);
        return m == null ? null : new Object[] {c, m};
    }

    /** The SDK class's supertypes, as android.jar declares them (JDK supertypes stop the walk). */
    static Set<String> sdkSupertypes(Map<String, Info> sdk, String name) {
        Set<String> out = new HashSet<>();
        Deque<String> todo = new ArrayDeque<>();
        Info s = sdk.get(name);
        if (s.superName != null) todo.add(s.superName);
        todo.addAll(List.of(s.interfaces));
        while (!todo.isEmpty()) {
            String t = todo.poll();
            if (!out.add(t)) continue;
            Info i = sdk.get(t);
            if (i == null) continue;
            if (i.superName != null) todo.add(i.superName);
            todo.addAll(List.of(i.interfaces));
        }
        return out;
    }
}
