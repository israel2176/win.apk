// AndroidPathsTest -- the java.base overlay's Android paths (the third change in
// WinNTFileSystem/File/WindowsPath, driven by jdk.internal.util.WinApkPaths): a
// '/'-rooted path ("/data/user/0/<pkg>/files") keeps that form through java.io
// and java.nio -- forward slashes, no drive, absolute -- while a plain Windows
// path is untouched. Run with the overlay patched into java.base and the feature
// on:
//
//   javac -d out windows/test/java/AndroidPathsTest.java
//   java --patch-module "java.base=<stage>/lib/java_base_patch" \
//        --add-exports java.base/jdk.internal.util=ALL-UNNAMED \
//        -Dwinapk.android.root="<tmp>" -cp out AndroidPathsTest
//
// Exits 0 when every check passes; prints and counts the failures otherwise.
// The host0 native that maps an Android path to a real host file is bound only
// in winapk-runtime; here an Android path's file is <winapk.android.root>/<path>,
// the table's last rule, so file I/O on names Windows forbids (':' '?' '*' '<'
// '>' '|' '"', a final dot or space) is checked too. Reflection on
// jdk.internal.util needs the --add-exports above.

import java.io.File;
import java.nio.file.Path;
import java.nio.file.Paths;

public final class AndroidPathsTest {

    static int failed = 0;

    static void eq(String what, Object got, Object want) {
        if (got == null ? want == null : got.equals(want)) return;
        failed++;
        System.err.println("FAIL " + what + ": got <" + got + "> want <" + want + ">");
    }

    static void tru(String what, boolean cond) {
        if (cond) return;
        failed++;
        System.err.println("FAIL " + what + ": expected true");
    }

    // jdk.internal.util.WinApkPaths.hostNames, reached by reflection (java.base does not export it).
    static String hostNames(String p) {
        try {
            java.lang.reflect.Method m = Class.forName("jdk.internal.util.WinApkPaths").getMethod("hostNames", String.class);
            return (String) m.invoke(null, p);
        } catch (ReflectiveOperationException e) {
            throw new AssertionError(e);
        }
    }

    public static void main(String[] args) throws Exception {
        final String D = "/data/user/0/com.example.app";

        // ---- java.io.File ----
        File f = new File(D + "/files");
        tru("File.isAbsolute", f.isAbsolute());
        eq("File.getPath", f.getPath(), D + "/files");
        eq("File.getAbsolutePath", f.getAbsolutePath(), D + "/files");
        eq("File.getName", f.getName(), "files");
        eq("File.getParent", f.getParent(), D);
        eq("File.toString no backslash", f.toString().indexOf('\\'), -1);

        // A child joins with '/', either separator folded.
        eq("File(parent, child)", new File(D, "sub/leaf").getPath(), D + "/sub/leaf");
        eq("File mixed separators fold", new File(D + "\\a\\b").getPath(), D + "/a/b");

        // "." and ".." fold out by text (canonicalize), staying Android form.
        eq("File.getCanonicalPath", new File(D + "/./a/../b").getCanonicalPath(), D + "/b");

        // A name with characters Windows forbids is an Android name like any other
        // (the host stores them as private-use characters, winapk-paths-test).
        File odd = new File(D + "/databases", "1||.entitystore");
        eq("File forbidden characters getPath", odd.getPath(), D + "/databases/1||.entitystore");
        eq("File forbidden characters getName", odd.getName(), "1||.entitystore");
        eq("File colon getName", new File(D + "/files/ab:c").getName(), "ab:c");

        // ---- java.nio.Path ----
        Path p = Paths.get(D + "/files");
        tru("Path.isAbsolute", p.isAbsolute());
        eq("Path.toString", p.toString(), D + "/files");
        eq("Path.toString no backslash", p.toString().indexOf('\\'), -1);
        eq("Path.getFileName", p.getFileName().toString(), "files");
        eq("Path.getParent", p.getParent().toString(), D);
        eq("Path.getRoot", p.getRoot().toString(), "/");
        eq("Path.getNameCount", p.getNameCount(), 5);
        eq("Path.toAbsolutePath", p.toAbsolutePath().toString(), D + "/files");
        eq("Path.resolve", Paths.get(D).resolve("sub").toString(), D + "/sub");
        eq("Path.normalize dotdot", Paths.get(D + "/a/../b").normalize().toString(), D + "/b");

        // ---- plain Windows paths are untouched ----
        File w = new File("C:\\x\\y");
        tru("Windows File.isAbsolute", w.isAbsolute());
        eq("Windows File.getPath", w.getPath(), "C:\\x\\y");
        eq("Windows File.getName", w.getName(), "y");
        Path wp = Paths.get("C:\\x\\y");
        tru("Windows Path.isAbsolute", wp.isAbsolute());
        eq("Windows Path.toString", wp.toString(), "C:\\x\\y");
        eq("Windows Path.getFileName", wp.getFileName().toString(), "y");

        // ---- names Windows forbids, through java.nio (Firebase's "frc_<app id>_..." preferences) ----
        String frc = D + "/shared_prefs/frc_1:1005457359603:android:fbc66c9d3f6296ca_firebase_settings.xml";
        Path fp = Paths.get(frc);
        eq("Path colon toString", fp.toString(), frc);
        eq("Path colon getFileName", fp.getFileName().toString(), "frc_1:1005457359603:android:fbc66c9d3f6296ca_firebase_settings.xml");
        eq("Path colon getParent", fp.getParent().toString(), D + "/shared_prefs");
        eq("File colon toPath", new File(frc).toPath().toString(), frc);
        eq("Path.resolve(String) colon", Paths.get(D).resolve("a:b").toString(), D + "/a:b");
        eq("Path.of(first, more) forbidden", Path.of(D, "x?", "y*").toString(), D + "/x?/y*");
        eq("Path.resolveSibling(String)", fp.resolveSibling("p|q").toString(), D + "/shared_prefs/p|q");
        tru("File colon not invalid", new File(frc).getAbsolutePath().equals(frc));

        // ---- the host names: each forbidden character is U+F000 + it (WSL's and Cygwin's mapping) ----
        eq("hostNames colon", hostNames("/a/b:c"), "/a/b\uF03Ac");
        eq("hostNames final dot and space", hostNames("/a./b /c"), "/a\uF02E/b\uF020/c");
        eq("hostNames links", hostNames("/a/./b/.."), "/a/./b/..");
        eq("hostNames plain", hostNames("/a/b.c"), "/a/b.c");

        // ---- real files: create, list, open, rename, delete, canonical path ----
        // Outside winapk-runtime the host file of an Android path is <winapk.android.root>/<path>,
        // the runtime table's last rule (WinApkPaths.hostPath), so this runs on any Windows JVM.
        String root = System.getProperty("winapk.android.root");
        String T = "/data/winapk-paths-test-" + ProcessHandle.current().pid();
        File dir = new File(T);
        tru("mkdirs " + T, dir.mkdirs() || dir.isDirectory());
        String[] names = { "plain.xml", "with space", "a:b", "q?", "s*", "lt<", "gt>", "p|q", "dq\"", "dot.", "space ", "ctl\u0001x", "frc_1:2:android:3.xml" };
        for (String n : names) {
            String shown = n.replace("\u0001", "\\u0001");
            File f1 = new File(dir, n);
            eq("File name " + shown, f1.getName(), n);
            // Created through java.nio: java.io's opens take the host file in winapk-runtime's own
            // natives (host/java_io_open.cpp), which a plain JVM does not have.
            try {
                java.nio.file.Files.write(f1.toPath(), n.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            } catch (java.io.IOException e) {
                failed++;
                System.err.println("FAIL create " + shown + ": " + e);
                continue;
            }
            tru("exists " + shown, f1.exists() && f1.isFile());
            eq("length " + shown, f1.length(), (long) n.getBytes(java.nio.charset.StandardCharsets.UTF_8).length);
            tru("File.list has " + shown, java.util.Arrays.asList(dir.list()).contains(n));
            boolean inListFiles = false;
            for (File c : dir.listFiles()) inListFiles |= c.getName().equals(n) && c.getPath().equals(T + "/" + n);
            tru("File.listFiles has " + shown, inListFiles);
            boolean inStream = false;
            try (java.nio.file.DirectoryStream<Path> ds = java.nio.file.Files.newDirectoryStream(dir.toPath())) {
                for (Path c : ds) inStream |= c.getFileName().toString().equals(n) && c.toString().equals(T + "/" + n);
            }
            tru("Files.newDirectoryStream has " + shown, inStream);
            boolean inWalk;
            try (java.util.stream.Stream<Path> walk = java.nio.file.Files.walk(dir.toPath())) {
                inWalk = walk.anyMatch(c -> c.getFileName().toString().equals(n));
            }
            tru("Files.walk has " + shown, inWalk);
            eq("read (nio) " + shown, new String(java.nio.file.Files.readAllBytes(f1.toPath()),
                    java.nio.charset.StandardCharsets.UTF_8), n);
            eq("getCanonicalPath " + shown, f1.getCanonicalPath(), T + "/" + n);
            eq("toRealPath " + shown, f1.toPath().toRealPath().toString(), T + "/" + n);
            // On the host it is the private-use spelling, so other tools (WSL, Cygwin) read the same name.
            if (root != null) {
                File host = new File(root + hostNames(T + "/" + n).replace('/', '\\'));
                tru("host spelling " + shown, host.isFile());
            }
            File f2 = new File(dir, "renamed " + n);
            tru("File.renameTo " + shown, f1.renameTo(f2) && f2.exists() && !f1.exists());
            Path p3 = dir.toPath().resolve("moved-" + n);
            java.nio.file.Files.move(f2.toPath(), p3);
            tru("Files.move " + shown, java.nio.file.Files.exists(p3) && !f2.exists());
            java.nio.file.Files.delete(p3);
            tru("Files.delete " + shown, !java.nio.file.Files.exists(p3));
        }
        File sub = new File(dir, "dir:x.");
        tru("mkdir forbidden name", sub.mkdir() && sub.isDirectory());
        tru("File.list dir name", java.util.Arrays.asList(dir.list()).contains("dir:x."));
        tru("delete forbidden dir", sub.delete() && !sub.exists());
        tru("delete test dir", dir.delete());

        if (failed == 0) System.err.println("AndroidPathsTest: all checks passed");

        // Microbenchmark: the Java-side per-op cost the overlay adds for a path
        // string (the isAndroid check plus, for an Android path, normalize).
        // host0 is unbound here, so this is the string-form overhead only, not
        // the native host mapping (that is winapk-paths-test's bench).
        final int N = 1_000_000;
        long sink = 0;
        // warm up
        for (int i = 0; i < 100_000; i++) sink += new File(D + "/files").getAbsolutePath().length();
        long t0 = System.nanoTime();
        for (int i = 0; i < N; i++) sink += new File(D + "/files").getAbsolutePath().length();
        long androidNs = (System.nanoTime() - t0) / N;
        t0 = System.nanoTime();
        for (int i = 0; i < N; i++) sink += new File("C:\\root\\x\\files").getAbsolutePath().length();
        long winNs = (System.nanoTime() - t0) / N;
        System.err.println("AndroidPathsTest bench (x" + N + "): File+getAbsolutePath Android=" + androidNs
                + " ns/op, Windows=" + winNs + " ns/op (sink=" + sink + ")");

        System.exit(failed);
    }
}
