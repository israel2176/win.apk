// AndroidPathsTest -- the java.base overlay's Android paths (the third change in
// WinNTFileSystem/File/WindowsPath, driven by jdk.internal.util.WinApkPaths): a
// '/'-rooted path ("/data/user/0/<pkg>/files") keeps that form through java.io
// and java.nio -- forward slashes, no drive, absolute -- while a plain Windows
// path is untouched. Run with the overlay patched into java.base and the feature
// on:
//
//   javac -d out windows/test/java/AndroidPathsTest.java
//   java --patch-module "java.base=<stage>/lib/java_base_patch" \
//        -Dwinapk.android.root="<tmp>" -cp out AndroidPathsTest
//
// Exits 0 when every check passes; prints and counts the failures otherwise.
// The host0 native that maps an Android path to a real host file is bound only
// in winapk-runtime, so this test checks the STRING form (what the app sees),
// not file I/O; the host mapping itself is winapk-paths-test's.

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

        if (failed == 0) System.err.println("AndroidPathsTest: all checks passed");
        System.exit(failed);
    }
}
