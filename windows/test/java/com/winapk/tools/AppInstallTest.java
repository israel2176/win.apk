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
// .splits folder installs alone, as before. The package of the kept .apk reads as the bundle's.
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
        byte[] abi = zip(Map.of("AndroidManifest.xml", manifest(pkg), "lib/x86_64/libnative.so", bytes("ELF x86_64")));
        byte[] dpi = zip(Map.of("AndroidManifest.xml", manifest(pkg), "res/drawable-xxhdpi/icon.png", bytes("png")));
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

        AppInstall.deleteTree(root);
        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
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

    /** A binary AndroidManifest.xml whose root element is <manifest package="pkg">: a string pool (UTF-8)
     *  and one start element, which is all AppInstall.rootAttribute reads. */
    static byte[] manifest(String pkg) {
        List<String> strings = List.of("manifest", "package", pkg);
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
        int elemHeader = 16, elemSize = elemHeader + 20 + 20;
        ByteBuffer b = ByteBuffer.allocate(8 + poolSize + elemSize).order(ByteOrder.LITTLE_ENDIAN);
        b.putShort((short) 0x0003).putShort((short) 8).putInt(8 + poolSize + elemSize);
        b.putShort((short) 0x0001).putShort((short) poolHeader).putInt(poolSize).putInt(strings.size()).putInt(0)
                .putInt(0x100).putInt(poolHeader + 4 * strings.size()).putInt(0);
        for (int o : offsets) b.putInt(o);
        b.put(data.toByteArray());
        b.putShort((short) 0x0102).putShort((short) elemHeader).putInt(elemSize).putInt(1).putInt(-1);
        b.putInt(-1).putInt(0).putShort((short) 20).putShort((short) 20).putShort((short) 1).putShort((short) 0)
                .putShort((short) 0).putShort((short) 0);
        b.putInt(-1).putInt(1).putInt(2).putShort((short) 8).put((byte) 0).put((byte) 0x03).putInt(2);
        return b.array();
    }

}
