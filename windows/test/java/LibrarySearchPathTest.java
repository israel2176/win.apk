// LibrarySearchPathTest -- checks that a dalvik class loader created with a
// librarySearchPath finds libraries on it, as AOSP's DexPathList.findLibrary
// does (runtime/java/dalvik/system/NativeLibraryPath.java):
//
//   * a directory entry answers <dir>/lib<name>.so;
//   * a "<zip>!/<dir>" entry answers a copy of the zip's entry, made once,
//     with the libraries of the same zip directory its DT_NEEDED names;
//   * a changed zip gets a new copy, and the old directory goes;
//   * a name the path does not have is null here (no app loader above: with
//     one, BaseDexClassLoader.findLibrary asks it, as it always did);
//   * the path is split on ':', and on a Windows host a drive's colon is kept
//     and ';' separates too.
//
// Runs on any JVM, with no app: the loaders are the real ones, the libraries
// are only files. What the app's loader then does with the file found (the
// stub the JVM loads) is OnDemandNativeLoadTest's last section, on Windows.
//
//   javac -cp <runtime>/lib/android-framework.jar -d out windows/test/java/LibrarySearchPathTest.java
//   java  -cp "<runtime>/lib/android-framework.jar:<runtime>/lib/macapk-loader.jar:out" LibrarySearchPathTest
//
// Exits 0 when every check passes.

import dalvik.system.BaseDexClassLoader;
import dalvik.system.DelegateLastClassLoader;
import dalvik.system.DexClassLoader;
import dalvik.system.PathClassLoader;
import dalvik.system.VMRuntime;

import java.io.File;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public final class LibrarySearchPathTest {
    static int failures = 0;

    static void check(String what, Object got, Object want) {
        boolean ok = want == null ? got == null : want.equals(got);
        System.out.println((ok ? "ok   " : "FAIL ") + what + ": " + got + (ok ? "" : " (want " + want + ")"));
        if (!ok) failures++;
    }

    /** An x86_64 ELF shared object with nothing but its DT_NEEDED list. */
    static byte[] elf(String... needed) {
        StringBuilder strtab = new StringBuilder("\0");
        int[] at = new int[needed.length];
        for (int i = 0; i < needed.length; i++) {
            at[i] = strtab.length();
            strtab.append(needed[i]).append('\0');
        }
        int dynOff = 64 + 56;
        int dynSize = (needed.length + 3) * 16;
        int strOff = dynOff + dynSize;
        ByteBuffer b = ByteBuffer.allocate(strOff + strtab.length()).order(ByteOrder.LITTLE_ENDIAN);
        b.put(new byte[] {0x7f, 'E', 'L', 'F', 2, 1, 1});
        b.position(16);
        b.putShort((short) 3).putShort((short) 62).putInt(1);  // ET_DYN, EM_X86_64, EV_CURRENT
        b.putLong(0).putLong(64).putLong(0);                   // e_entry, e_phoff, e_shoff
        b.putInt(0).putShort((short) 64).putShort((short) 56).putShort((short) 1);
        b.position(64);
        b.putInt(2).putInt(4);                                 // PT_DYNAMIC, PF_R
        b.putLong(dynOff).putLong(dynOff).putLong(dynOff).putLong(dynSize).putLong(dynSize).putLong(8);
        for (int off : at) b.putLong(1).putLong(off);          // DT_NEEDED
        b.putLong(5).putLong(strOff);                          // DT_STRTAB
        b.putLong(10).putLong(strtab.length());                // DT_STRSZ
        b.putLong(0).putLong(0);                               // DT_NULL
        b.put(strtab.toString().getBytes(StandardCharsets.ISO_8859_1));
        return b.array();
    }

    static void zip(File out, String[] names, byte[][] contents) throws Exception {
        try (ZipOutputStream z = new ZipOutputStream(Files.newOutputStream(out.toPath()))) {
            for (int i = 0; i < names.length; i++) {
                z.putNextEntry(new ZipEntry(names[i]));
                z.write(contents[i]);
                z.closeEntry();
            }
        }
    }

    @SuppressWarnings("unchecked")
    static List<String> split(String path, boolean windowsHost) throws Exception {
        Method m = Class.forName("dalvik.system.NativeLibraryPath").getDeclaredMethod("split", String.class,
                                                                                      boolean.class);
        m.setAccessible(true);
        return (List<String>) m.invoke(null, path, windowsHost);
    }

    public static void main(String[] args) throws Exception {
        Path tmp = Files.createTempDirectory("libpath-test");
        File data = Files.createDirectories(tmp.resolve("data")).toFile();
        VMRuntime.setProcessDataDirectory(data.getPath());
        File cache = new File(data, "code_cache/macapk-native-libs");
        ClassLoader parent = LibrarySearchPathTest.class.getClassLoader();

        // ---- the separators ----
        check("Android path", split("/a/lib:/b/m.apk!/lib/x86_64", false),
              List.of("/a/lib", "/b/m.apk!/lib/x86_64"));
        check("Windows host: drive colons stay, ';' separates",
              split("C:\\a\\lib:/data/m.apk!/lib/x86_64;D:/b:c", true),
              List.of("C:\\a\\lib", "/data/m.apk!/lib/x86_64", "D:/b", "c"));
        check("empty entries are dropped", split("::/a:", false), List.of("/a"));
        check("no path", split(null, false), List.of());

        // ---- a directory entry ----
        File dir = Files.createDirectories(tmp.resolve("plugin/lib")).toFile();
        File foo = new File(dir, "libfoo.so");
        Files.write(foo.toPath(), elf());
        BaseDexClassLoader byDir = new DexClassLoader("", null, dir.getPath(), parent);
        check("directory entry", byDir.findLibrary("foo"), foo.getPath());
        check("a name it does not have", byDir.findLibrary("nope"), null);
        check("a name with a separator", byDir.findLibrary("../lib/foo"), null);
        check("no path, no answer", new PathClassLoader("", parent).findLibrary("foo"), null);

        // ---- a zip entry: copied out once, with what it needs ----
        File apk = tmp.resolve("module.apk").toFile();
        zip(apk, new String[] {"lib/x86_64/libmod.so", "lib/x86_64/libdep.so", "lib/x86_64/libdep2.so",
                               "lib/x86_64/libunused.so", "lib/arm64-v8a/libmod.so"},
            new byte[][] {elf("libdep.so", "libc.so", "../evil.so"), elf("libdep2.so"), elf("libmod.so"), elf(),
                          elf()});
        BaseDexClassLoader byZip =
            new DelegateLastClassLoader("", "/nowhere/lib:" + apk.getPath() + "!/lib/x86_64", parent);
        String mod = byZip.findLibrary("mod");
        check("zip entry: answered", mod != null && new File(mod).getName().equals("libmod.so"), true);
        File out = new File(mod).getParentFile();
        check("copied under the data dir's code_cache", out.getParentFile(), cache);
        check("the copy is the entry", java.util.Arrays.equals(Files.readAllBytes(new File(mod).toPath()),
                                                                 elf("libdep.so", "libc.so", "../evil.so")), true);
        check("with its DT_NEEDED closure of that zip directory, and nothing else",
              new java.util.TreeSet<>(List.of(out.list())).toString(), "[libdep.so, libdep2.so, libmod.so]");
        long stamp = new File(mod).lastModified();
        Thread.sleep(1100);
        check("asked again: the same file", byZip.findLibrary("mod"), mod);
        check("another loader of the same path: the same file",
              new DexClassLoader("", null, apk.getPath() + "!/lib/x86_64/", parent).findLibrary("mod"), mod);
        check("not written again", new File(mod).lastModified(), stamp);
        check("an entry the zip does not have", byZip.findLibrary("nope"), null);
        String unused = byZip.findLibrary("unused");
        check("a second library of the zip joins the first", unused, new File(out, "libunused.so").getPath());
        check("the directory comes before the zip",
              new PathClassLoader("", dir.getPath() + ":" + apk.getPath() + "!/lib/x86_64", parent)
                  .findLibrary("foo"), foo.getPath());

        // ---- the zip changes: a new copy, the old one removed ----
        zip(apk, new String[] {"lib/x86_64/libmod.so"}, new byte[][] {elf("libc.so", "libm.so")});
        String mod2 = byZip.findLibrary("mod");
        check("a changed zip is copied anew", mod2 != null && !mod2.equals(mod), true);
        check("the new copy is the new entry",
              java.util.Arrays.equals(Files.readAllBytes(new File(mod2).toPath()), elf("libc.so", "libm.so")), true);
        check("the old copy's directory is gone", out.exists(), false);
        check("no temporary file is left", new File(mod2).getParentFile().list().length, 1);

        // ---- a missing zip ----
        check("a zip that is not there",
              new DexClassLoader("", null, tmp.resolve("none.apk") + "!/lib/x86_64", parent).findLibrary("mod"),
              null);

        System.out.println(failures == 0 ? "LibrarySearchPathTest: OK"
                                         : "LibrarySearchPathTest: " + failures + " failure(s)");
        System.exit(failures == 0 ? 0 : 1);
    }
}
