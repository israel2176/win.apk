// AppInstallTest -- com.winapk.tools.AppInstall on bundles and on what the library keeps of them.
//
//   javac -d <classes> windows/java/com/winapk/tools/AppInstall.java \
//       windows/test/java/com/winapk/tools/AppInstallTest.java
//   java -cp <classes> com.winapk.tools.AppInstallTest <scratch dir>
//
// An .apkm (base.apk, an x86_64 config split with a library, a density split) installs as a device leaves
// it: base.apk, the splits by their installed names, the x86_64 library under lib/x86_64, the .installed
// stamp ("<APKs> <bytes>:<mtime ms>") and the record. The launcher then keeps the app as <name>.apk and
// <name>.splits/ (launcher/library_files.h), hard links to those APKs, and drops the bundle: installing
// again from that .apk (a stale record) links base.apk and every split back, extracts the split's library
// again, and leaves the library's files the very files installed (one copy on disk). A plain APK with no
// .splits folder installs alone, as before. The package of the kept .apk reads as the bundle's. Hostile names
// go nowhere: a package that fails Android's validateName is not installed, and a library or a bundle's APK
// whose file name is not safe (a backslash, a drive) is skipped.
// Prints PASS/FAIL per check and "ALL PASSED" at the end; exits 1 on a failure.
package com.winapk.tools;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public class AppInstallTest {
    static int failures;

    static void check(String name, boolean ok) {
        System.out.println((ok ? "PASS " : "FAIL ") + name);
        if (!ok) failures++;
    }

    public static void main(String[] args) throws Exception {
        Path root = Paths.get(args[0]).toAbsolutePath();
        AppInstall.deleteTree(root);
        Files.createDirectories(root);
        Path library = Files.createDirectories(root.resolve("library"));
        Path android = Files.createDirectories(root.resolve("android"));
        String pkg = "com.example.bundled";

        byte[] base = zip(Map.of("AndroidManifest.xml", manifest(pkg), "classes.dex", bytes("dex\n035")));
        byte[] abi = zip(Map.of("AndroidManifest.xml", splitManifest(pkg, "config.x86_64"), "lib/x86_64/libnative.so", bytes("ELF x86_64")));
        byte[] dpi = zip(Map.of("AndroidManifest.xml", splitManifest(pkg, "config.xxhdpi"), "res/drawable-xxhdpi/icon.png", bytes("png")));
        Map<String, byte[]> members = new LinkedHashMap<>();
        members.put("base.apk", base);
        members.put("split_config.x86_64.apk", abi);
        members.put("split_config.xxhdpi.apk", dpi);
        members.put("info.json", bytes("{}"));
        Path bundle = library.resolve("Bundled.apkm");
        Files.write(bundle, zip(members));

        // The bundle installs.
        Path record = android.resolve("data/app/.Bundled.installed");
        Files.createDirectories(record.getParent());
        int rc = AppInstall.install(bundle, android, "x86_64,arm64-v8a", record);
        check("bundle installs", rc == 0);
        Path dir = android.resolve("data/app").resolve(AppInstall.DIR_NONCE).resolve(pkg + "-" + AppInstall.NAME_NONCE);
        check("record names the installed base.apk",
                Files.readString(record).equals(dir.resolve("base.apk").toString()));
        check("installed: base.apk and both splits", Files.isRegularFile(dir.resolve("base.apk"))
                && Files.isRegularFile(dir.resolve("split_config.x86_64.apk"))
                && Files.isRegularFile(dir.resolve("split_config.xxhdpi.apk")));
        check("installed: the split's x86_64 library", Files.isRegularFile(dir.resolve("lib/x86_64/libnative.so")));
        String stamp = Files.readString(dir.resolve(".installed"));
        check("stamp: 3 APKs, the bundle's size and time", stamp.equals("3 " + Files.size(bundle) + ":"
                + Files.getLastModifiedTime(bundle).toMillis() + "\n"));

        // What the launcher keeps (library_files.h keep_installed): links to the installed APKs, no bundle.
        Path apk = library.resolve("Bundled.apk");
        Path splits = Files.createDirectories(library.resolve("Bundled.splits"));
        Files.createLink(apk, dir.resolve("base.apk"));
        Files.createLink(splits.resolve("split_config.x86_64.apk"), dir.resolve("split_config.x86_64.apk"));
        Files.createLink(splits.resolve("split_config.xxhdpi.apk"), dir.resolve("split_config.xxhdpi.apk"));
        Files.delete(bundle);
        check("kept .apk reads as the bundle's package", pkg.equals(AppInstall.packageOf(apk)));
        check("kept splits listed in order", AppInstall.keptSplits(apk).size() == 2
                && AppInstall.keptSplits(apk).get(0).getFileName().toString().equals("split_config.x86_64.apk"));

        // Installed again from the kept files, as the runtime does with a stale record.
        Files.delete(record);
        rc = AppInstall.install(apk, android, "x86_64,arm64-v8a", record);
        check("kept .apk + .splits install again", rc == 0);
        check("again: base.apk and both splits", Files.isRegularFile(dir.resolve("base.apk"))
                && Files.isRegularFile(dir.resolve("split_config.x86_64.apk"))
                && Files.isRegularFile(dir.resolve("split_config.xxhdpi.apk")));
        check("again: the split's x86_64 library", Files.isRegularFile(dir.resolve("lib/x86_64/libnative.so")));
        check("again: the installed APKs are the library's files (one copy)",
                Files.isSameFile(apk, dir.resolve("base.apk"))
                        && Files.isSameFile(splits.resolve("split_config.x86_64.apk"),
                                dir.resolve("split_config.x86_64.apk"))
                        && Files.isSameFile(splits.resolve("split_config.xxhdpi.apk"),
                                dir.resolve("split_config.xxhdpi.apk")));
        check("again: the content is the bundle's", java.util.Arrays.equals(Files.readAllBytes(apk), base)
                && java.util.Arrays.equals(Files.readAllBytes(dir.resolve("split_config.x86_64.apk")), abi));
        check("again: stamp counts 3 APKs", Files.readString(dir.resolve(".installed")).startsWith("3 "));

        // A plain APK with no .splits folder: base.apk alone.
        Path plain = library.resolve("Plain.apk");
        Files.write(plain, zip(Map.of("AndroidManifest.xml", manifest("com.example.plain"), "classes.dex",
                bytes("dex\n035"))));
        Path plainRecord = android.resolve("data/app/.Plain.installed");
        rc = AppInstall.install(plain, android, "x86_64,arm64-v8a", plainRecord);
        Path plainDir = android.resolve("data/app").resolve(AppInstall.DIR_NONCE)
                .resolve("com.example.plain-" + AppInstall.NAME_NONCE);
        long apks;
        try (var s = Files.list(plainDir)) {
            apks = s.filter(p -> p.getFileName().toString().endsWith(".apk")).count();
        }
        check("plain APK installs alone",
                rc == 0 && apks == 1 && Files.isSameFile(plain, plainDir.resolve("base.apk")));
        check("plain APK has no kept splits", AppInstall.keptSplits(plain).isEmpty());

        // Hostile names, refused or skipped as a device's installer refuses or skips them: a package with a
        // path in it would name directories outside the app (and the data folder a removal deletes), a
        // library or split named with a backslash or ':' a file outside it on Windows.
        String up = "..\\..\\..\\..\\";
        check("package names: Android's rule", AppInstall.validatePackageName("com.example.app") == null
                && AppInstall.validatePackageName("a.b_2.C3") == null
                && AppInstall.validatePackageName("noseparator") != null
                && AppInstall.validatePackageName("..") != null
                && AppInstall.validatePackageName("com.1digit") != null
                && AppInstall.validatePackageName("com.example\\" + up + "escaped") != null
                && AppInstall.validatePackageName("com.example/../escaped") != null
                && AppInstall.validatePackageName("C:.example") != null
                && AppInstall.validatePackageName("") != null);
        Path hostile = library.resolve("Hostile.apk");
        Files.write(hostile, zip(Map.of("AndroidManifest.xml", manifest("com.example/../../../../escaped"),
                "classes.dex", bytes("dex\n035"))));
        rc = AppInstall.install(hostile, android, "x86_64,arm64-v8a", android.resolve("data/app/.Hostile.installed"));
        check("a package with a path in it is not installed", rc == 3 && !anyNamed(root, "escaped")
                && !Files.exists(android.resolve("data/app/.Hostile.installed")));

        Path libs = library.resolve("Libs.apk");
        Files.write(libs, zip(Map.of("AndroidManifest.xml", manifest("com.example.libs"),
                "lib/x86_64/libgood.so", bytes("ELF good"),
                "lib/x86_64/" + up + "libevil.so", bytes("ELF evil"),
                "lib/x86_64/C:libdrive.so", bytes("ELF drive"))));
        rc = AppInstall.install(libs, android, "x86_64,arm64-v8a", android.resolve("data/app/.Libs.installed"));
        Path libsDir = android.resolve("data/app").resolve(AppInstall.DIR_NONCE)
                .resolve("com.example.libs-" + AppInstall.NAME_NONCE).resolve("lib/x86_64");
        List<String> extracted;
        try (var s = Files.list(libsDir)) {
            extracted = s.map(p -> p.getFileName().toString()).toList();
        }
        check("only safely named libraries are extracted", rc == 0 && extracted.equals(List.of("libgood.so"))
                && !anyNamed(root, "libevil") && !anyNamed(root, "libdrive"));

        Map<String, byte[]> hostileBundle = new LinkedHashMap<>();
        hostileBundle.put("base.apk", zip(Map.of("AndroidManifest.xml", manifest("com.example.hb"))));
        hostileBundle.put(up + "splitevil.apk", dpi);
        Path hb = library.resolve("Hb.apkm");
        Files.write(hb, zip(hostileBundle));
        rc = AppInstall.install(hb, android, "x86_64,arm64-v8a", android.resolve("data/app/.Hb.installed"));
        Path hbDir = android.resolve("data/app").resolve(AppInstall.DIR_NONCE)
                .resolve("com.example.hb-" + AppInstall.NAME_NONCE);
        check("a bundle's unsafely named APK is skipped", rc == 0 && Files.isRegularFile(hbDir.resolve("base.apk"))
                && !anyNamed(root, "splitevil"));

        // INSTALL_FAILED_NO_MATCHING_ABIS: native code for none of the PC's ABIs is refused before install,
        // native code for one of them (or none at all) installs.
        Path arm32 = library.resolve("Arm32.apk");
        Files.write(arm32, zip(Map.of("AndroidManifest.xml", manifest("com.example.arm32"),
                "lib/armeabi-v7a/libreact.so", bytes("ELF arm32"))));
        rc = AppInstall.install(arm32, android, "x86_64,arm64-v8a", android.resolve("data/app/.Arm32.installed"));
        check("an armeabi-v7a-only app is refused", rc == 3 && !Files.exists(android.resolve("data/app")
                .resolve(AppInstall.DIR_NONCE).resolve("com.example.arm32-" + AppInstall.NAME_NONCE)));
        Path both = library.resolve("Both.apk");
        Files.write(both, zip(Map.of("AndroidManifest.xml", manifest("com.example.both"),
                "lib/armeabi-v7a/libx.so", bytes("ELF arm32"), "lib/arm64-v8a/libx.so", bytes("ELF arm64"))));
        rc = AppInstall.install(both, android, "x86_64,arm64-v8a", android.resolve("data/app/.Both.installed"));
        check("an app with arm64-v8a beside armeabi-v7a installs", rc == 0);
        check("libAbis lists the ABIs", AppInstall.libAbis(both).equals(new java.util.TreeSet<>(
                List.of("arm64-v8a", "armeabi-v7a"))));
        sessionChecks(library, android);

        AppInstall.deleteTree(root);
        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }

    /** The checks PackageInstallerSession makes of a full install's APKs, each refused as Android refuses it
     *  and leaving the app's install from before as it was; and the APKs named by their manifests. */
    static void sessionChecks(Path library, Path android) throws IOException {
        String pkg = "com.example.parts";
        Path dir = android.resolve("data/app").resolve(AppInstall.DIR_NONCE).resolve(pkg + "-" + AppInstall.NAME_NONCE);
        byte[] base = zip(Map.of("AndroidManifest.xml", manifest(pkg, null, 7, true, null, null),
                "classes.dex", bytes("dex\n035")));
        byte[] abi = zip(Map.of("AndroidManifest.xml", manifest(pkg, "config.arm64_v8a", 7, false, null, null),
                "lib/arm64-v8a/libreactnative.so", bytes("ELF arm64")));

        // The app as a bundle whose files are named by a store, not as installed: installs, its split by its name.
        Map<String, byte[]> m = new LinkedHashMap<>();
        m.put(pkg + ".apk", base);
        m.put("libs-for-arm.apk", abi);
        Path xapk = library.resolve("Parts.xapk");
        Files.write(xapk, zip(m));
        Path record = android.resolve("data/app/.Parts.installed");
        int rc = AppInstall.install(xapk, android, "x86_64,arm64-v8a", record);
        check("bundle: installed, the split by its manifest's name", rc == 0
                && Files.isRegularFile(dir.resolve("base.apk")) && Files.isRegularFile(dir.resolve("split_config.arm64_v8a.apk"))
                && !Files.exists(dir.resolve("split_libs-for-arm.apk"))
                && Files.isRegularFile(dir.resolve("lib/arm64/libreactnative.so")));

        // A split alone: "Full install must include a base package"; the installed app is untouched.
        Path lone = library.resolve("com.example.parts.config.arm64_v8a.apk");
        Files.write(lone, abi);
        rc = AppInstall.install(lone, android, "x86_64,arm64-v8a", android.resolve("data/app/.Lone.installed"));
        check("a split alone is refused, the app as it was", rc == 3
                && AppInstall.validateSession(List.of(lone)).equals(
                        "INSTALL_FAILED_INVALID_APK: Full install must include a base package")
                && Files.isRegularFile(dir.resolve("lib/arm64/libreactnative.so")));

        // A base that requires its splits, alone: INSTALL_FAILED_MISSING_SPLIT; the installed app is untouched.
        Path baseOnly = library.resolve("PartsBase.apk");
        Files.write(baseOnly, base);
        rc = AppInstall.install(baseOnly, android, "x86_64,arm64-v8a", android.resolve("data/app/.PartsBase.installed"));
        check("a base that requires splits, alone, is refused", rc == 3
                && AppInstall.validateSession(List.of(baseOnly)).equals(
                        "INSTALL_FAILED_MISSING_SPLIT: Missing split for " + pkg)
                && Files.isRegularFile(dir.resolve("split_config.arm64_v8a.apk")));

        // The same base with its split kept beside it (the launcher's <name>.splits): installs.
        Path splits = Files.createDirectories(library.resolve("PartsBase.splits"));
        Files.write(splits.resolve("split_whatever.apk"), abi);
        rc = AppInstall.install(baseOnly, android, "x86_64,arm64-v8a", android.resolve("data/app/.PartsBase.installed"));
        check("the base with its split installs", rc == 0 && Files.isRegularFile(dir.resolve("split_config.arm64_v8a.apk"))
                && Files.isRegularFile(dir.resolve("lib/arm64/libreactnative.so")));

        // Required split types: a split of each listed type, or the install is refused.
        Path typed = Files.write(library.resolve("typed-base.apk"), zip(Map.of("AndroidManifest.xml",
                manifest(pkg, null, 7, true, "abi,density", null))));
        Path abiTyped = Files.write(library.resolve("typed-abi.apk"), zip(Map.of("AndroidManifest.xml",
                manifest(pkg, "config.arm64_v8a", 7, false, null, "abi"))));
        Path dpiTyped = Files.write(library.resolve("typed-dpi.apk"), zip(Map.of("AndroidManifest.xml",
                manifest(pkg, "config.xxhdpi", 7, false, null, "density"))));
        check("required split types: one missing is refused",
                String.valueOf(AppInstall.validateSession(List.of(typed, abiTyped))).startsWith("INSTALL_FAILED_MISSING_SPLIT"));
        check("required split types: all present installs", AppInstall.validateSession(List.of(typed, abiTyped, dpiTyped)) == null);

        // A Play bundle's base names requiredSplitTypes with no isSplitRequired (Mizrahi Tefahot, issue #223:
        // base__abi,base__density): ApkLite counts that as requiring its splits, so the base alone is refused.
        Path playBase = Files.write(library.resolve("play-base.apk"), zip(Map.of("AndroidManifest.xml",
                manifest(pkg, null, 7, false, "abi,density", null))));
        check("required split types without isSplitRequired: the base alone is refused",
                String.valueOf(AppInstall.validateSession(List.of(playBase))).equals(
                        "INSTALL_FAILED_MISSING_SPLIT: Missing split for " + pkg));
        check("required split types without isSplitRequired: one type missing is refused",
                String.valueOf(AppInstall.validateSession(List.of(playBase, abiTyped))).startsWith("INSTALL_FAILED_MISSING_SPLIT"));
        check("required split types without isSplitRequired: all present installs",
                AppInstall.validateSession(List.of(playBase, abiTyped, dpiTyped)) == null);

        // Parts of other apps or versions, or a split twice: refused with Android's messages.
        Path other = Files.write(library.resolve("other.apk"), zip(Map.of("AndroidManifest.xml",
                manifest("com.example.other", "config.xxhdpi", 7, false, null, null))));
        Path older = Files.write(library.resolve("older.apk"), zip(Map.of("AndroidManifest.xml",
                manifest(pkg, "config.xxhdpi", 6, false, null, null))));
        Path again = Files.write(library.resolve("again.apk"), abi);
        Path abiFile = Files.write(library.resolve("abi.apk"), abi);
        check("another app's split is refused", AppInstall.validateSession(List.of(baseOnly, other)).equals(
                "INSTALL_FAILED_INVALID_APK: Inconsistent package com.example.other in other.apk; expected " + pkg));
        check("another version's split is refused", AppInstall.validateSession(List.of(baseOnly, older)).equals(
                "INSTALL_FAILED_INVALID_APK: Inconsistent version 6 in older.apk; expected 7"));
        check("a split twice is refused", AppInstall.validateSession(List.of(baseOnly, abiFile, again)).equals(
                "INSTALL_FAILED_INVALID_APK: Split config.arm64_v8a was defined multiple times"));

        // The launcher's question: which files are parts of which app.
        Path out = library.resolve("set.txt");
        check("--set writes", AppInstall.writeSet(out, List.of(baseOnly.toString(), lone.toString())) == 0);
        check("--set: package and split per file", Files.readString(out).equals(
                pkg + "\t\t" + baseOnly + "\n" + pkg + "\tconfig.arm64_v8a\t" + lone + "\n"));
    }

    /** Whether any file or folder under (or the volume's root above) `root` has a name containing `part`. */
    static boolean anyNamed(Path root, String part) throws IOException {
        for (Path top : new Path[] {root, root.getRoot()}) {
            try (var s = Files.walk(top, top == root ? Integer.MAX_VALUE : 1)) {
                if (s.anyMatch(p -> p.getFileName() != null && p.getFileName().toString().contains(part))) return true;
            }
        }
        return false;
    }

    static byte[] bytes(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }

    static byte[] zip(Map<String, byte[]> entries) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(out)) {
            for (Map.Entry<String, byte[]> e : new java.util.TreeMap<>(entries).entrySet()) {
                z.putNextEntry(new ZipEntry(e.getKey()));
                z.write(e.getValue());
                z.closeEntry();
            }
        }
        return out.toByteArray();
    }

    /** A base's manifest: <manifest package="pkg" versionCode="1">. */
    static byte[] manifest(String pkg) {
        return manifest(pkg, null, 1, false, null, null);
    }

    /** A split's manifest: <manifest package="pkg" split="split" versionCode="1">. */
    static byte[] splitManifest(String pkg, String split) {
        return manifest(pkg, split, 1, false, null, null);
    }

    /** A binary AndroidManifest.xml: a string pool (UTF-8), the start element <manifest package split
     *  versionCode requiredSplitTypes splitTypes> (split and the types only when given) and, for a base that
     *  requires its splits, <application isSplitRequired="true">: what AppInstall's reader looks at. */
    static byte[] manifest(String pkg, String split, int versionCode, boolean splitRequired,
            String requiredSplitTypes, String splitTypes) {
        List<String> strings = new java.util.ArrayList<>();
        java.util.function.Function<String, Integer> idx = t -> {
            int i = strings.indexOf(t);
            if (i < 0) { strings.add(t); i = strings.size() - 1; }
            return i;
        };
        // {name, raw string or -1, type, data}
        List<int[]> rootAttrs = new java.util.ArrayList<>();
        rootAttrs.add(new int[] {idx.apply("package"), idx.apply(pkg), 0x03, idx.apply(pkg)});
        if (split != null) rootAttrs.add(new int[] {idx.apply("split"), idx.apply(split), 0x03, idx.apply(split)});
        rootAttrs.add(new int[] {idx.apply("versionCode"), -1, 0x10, versionCode});
        if (requiredSplitTypes != null) rootAttrs.add(new int[] {idx.apply("requiredSplitTypes"),
                idx.apply(requiredSplitTypes), 0x03, idx.apply(requiredSplitTypes)});
        if (splitTypes != null) rootAttrs.add(new int[] {idx.apply("splitTypes"), idx.apply(splitTypes), 0x03,
                idx.apply(splitTypes)});
        int manifestTag = idx.apply("manifest");
        List<int[]> appAttrs = new java.util.ArrayList<>();
        if (splitRequired) appAttrs.add(new int[] {idx.apply("isSplitRequired"), -1, 0x12, -1});
        int applicationTag = idx.apply("application");

        ByteArrayOutputStream data = new ByteArrayOutputStream();
        int[] offsets = new int[strings.size()];
        for (int i = 0; i < strings.size(); i++) {
            offsets[i] = data.size();
            byte[] s = strings.get(i).getBytes(StandardCharsets.UTF_8);
            data.write(strings.get(i).length());
            data.write(s.length);
            data.write(s, 0, s.length);
            data.write(0);
        }
        while (data.size() % 4 != 0) data.write(0);
        int poolHeader = 28, poolSize = poolHeader + 4 * strings.size() + data.size();
        int elemHeader = 16;
        int rootSize = elemHeader + 20 + 20 * rootAttrs.size(), appSize = elemHeader + 20 + 20 * appAttrs.size();
        int total = 8 + poolSize + rootSize + appSize;
        ByteBuffer b = ByteBuffer.allocate(total).order(ByteOrder.LITTLE_ENDIAN);
        b.putShort((short) 0x0003).putShort((short) 8).putInt(total);
        b.putShort((short) 0x0001).putShort((short) poolHeader).putInt(poolSize).putInt(strings.size()).putInt(0)
                .putInt(0x100).putInt(poolHeader + 4 * strings.size()).putInt(0);
        for (int o : offsets) b.putInt(o);
        b.put(data.toByteArray());
        element(b, manifestTag, rootAttrs);
        element(b, applicationTag, appAttrs);
        return b.array();
    }

    static void element(ByteBuffer b, int tag, List<int[]> attrs) {
        int size = 16 + 20 + 20 * attrs.size();
        b.putShort((short) 0x0102).putShort((short) 16).putInt(size).putInt(1).putInt(-1);
        b.putInt(-1).putInt(tag).putShort((short) 20).putShort((short) 20).putShort((short) attrs.size())
                .putShort((short) 0).putShort((short) 0).putShort((short) 0);
        for (int[] a : attrs)
            b.putInt(-1).putInt(a[0]).putInt(a[1]).putShort((short) 8).put((byte) 0).put((byte) a[2]).putInt(a[3]);
    }

}
