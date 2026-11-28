// AppInstall -- installs an APK or a split bundle the way a device's installer
// leaves it, for the Windows host.
//
//     java -cp winapk-tools.jar com.winapk.tools.AppInstall
//          <app.apk|.apkm|.xapk|.apks> <android root> <abi>[,<abi>...] <record file>
//
// Upstream's runtime reads an installed app in the layout `pm install` leaves
// on a device, and only recognizes it there (runtime/java/com/macapk/loader/
// InstalledSplits.java):
//
//     <android root>/data/app/~~<nonce>/<package>-<nonce>/base.apk
//                                                        /split_<name>.apk ...
//                                                        /lib/<isa>/lib*.so
//                                                        /.installed
//
// On the Mac its C++ unpacker writes that layout (apk_unpacker.cpp, with the
// same two fixed nonces as here). The Windows host has no zip reader of its
// own, so it runs this instead:
//
//  - A plain APK becomes base.apk as a hard link (a copy across volumes),
//    as the Mac's APFS clone does without using space. So do the splits in
//    a <name>.splits folder beside a <name>.apk: the launcher's library
//    keeps an app that came as a bundle that way once it is installed, hard
//    links to the APKs installed here (launcher/library_files.h), so that an
//    install again (a stale record, WINAPK_ABIS) has the whole app.
//  - A bundle (.apkm from APKMirror, .xapk from APKPure, .apks from
//    bundletool) is unpacked; which APKs make up the app is decided exactly
//    as upstream's SelectBundleApks decides it, and each split gets the name
//    the device installer gives it.
//  - The native libraries of one ABI, from the base and every split, go to
//    lib/<isa>/ (Android's own extraction; libraries for other ABIs are left
//    in the APKs). The ABI is the first of the given list the app has
//    libraries for, as a device picks from its SUPPORTED_ABIS: the host
//    passes x86_64,arm64-v8a, so an app's x86_64 build is used when it has
//    one and its arm64 build (run through the interpreter) otherwise.
//  - Where a later ABI of the list has libraries the chosen one lacks (an
//    SDK that ships arm64 only: Mercantile's x86_64 split has no
//    libScanovateImagingCheque2.so, its cheque scanner), those go to that
//    ABI's lib/<isa>/ too, with the libraries of that ABI they need, as
//    their DT_NEEDED entries name them. A device would fail to load them;
//    Win.apk runs arm64 code, so the runtime loads them from there
//    (InstalledSplits.fallbackLibDirs). Only those: the background
//    translation translates whatever is in lib/arm64.
//
//  - The APKs are one install session, checked as PackageInstallerSession
//    checks one (validateApkInstallLocked, AOSP 14) before anything installed
//    is touched: one base (an APK whose manifest names no `split`; "Full
//    install must include a base package"), each split name once, every APK
//    of the base's package and version code, and a base that declares
//    android:isSplitRequired has its splits (with the types its
//    android:requiredSplitTypes lists): else INSTALL_FAILED_MISSING_SPLIT. A
//    refused set exits 3 with Android's error on stderr, and the app's install
//    from before stays as it was. Then each APK is named as the device names
//    a staged one, by its manifest: base.apk, split_<split name>.apk.
//  - Names are held to the device installer's rules before they name a file:
//    the manifest's package to PackageParser.validateName (it names the
//    installed directory, the data directory and the library's files), and
//    each library's and each bundle APK's file name to NativeLibraryHelper's
//    isFilenameSafe (on Windows a '\' or ':' in one names a file elsewhere).
//
// The installed base.apk's path is written to the record file, which the host
// reads back and also uses to tell an install is current.
//
// Exit codes: 0 installed, 2 usage, 3 nothing installable (or refused as Android refuses it), 1 error.
//
//     java -cp winapk-tools.jar com.winapk.tools.AppInstall --set <out file> <app.apk>...
//
// tells the launcher which files of a batch are parts of one app, so it installs
// them as one session (adb install-multiple): a line per file, "<package>\t<split
// name, empty for a base>\t<file>", the package empty for a file with no manifest.

package com.winapk.tools;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public final class AppInstall {
    private AppInstall() {}

    // Upstream's fixed nonces (apk_unpacker.cpp kInstallDirNonce /
    // kInstallNameNonce); InstalledSplits only checks the "~~" shape.
    static final String DIR_NONCE = "~~m4c4pk0Zx9AbC1dEfGhIjw==";
    static final String NAME_NONCE = "Qm4c4pk0Zx9AbC1dEfGh8Q==";

    public static void main(String[] args) {
        // AppInstall --package <app> <out file>: only the package name, for the
        // launcher's install helper, which must know which app an install
        // replaces before it asks the user and installs it.
        if (args.length == 3 && args[0].equals("--package")) {
            try {
                String pkg = packageOf(Paths.get(args[1]));
                if (pkg == null) {
                    System.err.println("[install] no package name in " + args[1]);
                    System.exit(3);
                }
                Files.writeString(Paths.get(args[2]), pkg, StandardCharsets.UTF_8);
                System.exit(0);
            } catch (IOException e) {
                System.err.println("[install] " + e);
                System.exit(1);
            }
        }
        if (args.length >= 2 && args[0].equals("--set")) {
            System.exit(writeSet(Paths.get(args[1]), List.of(args).subList(2, args.length)));
        }
        if (args.length != 4) {
            System.err.println("usage: AppInstall <app> <android root> <abi>[,<abi>...] <record file>");
            System.exit(2);
        }
        try {
            System.exit(install(Paths.get(args[0]), Paths.get(args[1]), args[2], Paths.get(args[3])));
        } catch (IOException e) {
            System.err.println("[install] " + e);
            System.exit(1);
        }
    }

    static int install(Path source, Path androidRoot, String abis, Path record) throws IOException {
        // A fixed name: only one install runs at a time, and a random one
        // would cost a SecureRandom seed for nothing.
        Path staging = androidRoot.resolve(".installing");
        deleteTree(staging);
        Files.createDirectories(staging);
        try {
            List<Path> members = new ArrayList<>();  // base first
            if (isApk(source)) {
                Path base = staging.resolve("base.apk");
                link(source, base);
                members.add(base);
                for (Path split : keptSplits(source)) {
                    Path target = staging.resolve(split.getFileName().toString());
                    link(split, target);
                    members.add(target);
                }
            } else if (!unpackBundle(source, staging, members)) {
                return 3;
            }

            // The session's checks, then each APK by the name the device gives it (base first).
            String refused = validateSession(members);
            if (refused != null) {
                System.err.println("[install] " + refused);
                return 3;
            }
            members = nameAsInstalled(members);
            String pkg = packageName(members.get(0));
            if (pkg == null) {
                System.err.println("[install] no package name in " + source);
                return 3;
            }
            String bad = validatePackageName(pkg);
            if (bad != null) {
                System.err.println("[install] invalid package name \"" + pkg + "\" in " + source + ": " + bad);
                return 3;
            }
            AppCompat.Result compat = AppCompat.evaluate(members);
            if (!compat.isInstallAllowed()) {
                System.err.println("[install] INSTALL_FAILED_UNSUPPORTED: " + pkg + " (" + compat.reasonCode
                        + ": " + compat.reasonDetail + ")");
                return 3;
            }
            // PackageManagerService refuses an app whose native code is for none of the device's ABIs
            // (NativeLibraryHelper.copyNativeBinariesForSupportedAbi: INSTALL_FAILED_NO_MATCHING_ABIS),
            // before anything is installed: an armeabi-v7a-only app on an x86_64 device with no ARM
            // translation. Installed, it would start and stop at its first System.loadLibrary.
            String[] supported = abis.split(",");
            Set<String> present = new TreeSet<>();
            for (Path m : members) present.addAll(libAbis(m));
            boolean matches = present.isEmpty();
            for (String a : supported) matches |= present.contains(a);
            if (!matches) {
                System.err.println("[install] INSTALL_FAILED_NO_MATCHING_ABIS: " + pkg + " has native code only for "
                        + String.join(",", present) + "; this PC runs " + abis);
                return 3;
            }

            Path dir = androidRoot.resolve("data").resolve("app").resolve(DIR_NONCE)
                    .resolve(pkg + "-" + NAME_NONCE);
            Files.createDirectories(dir.getParent());
            deleteTree(dir);
            Files.move(staging, dir);
            staging = null;

            String[] preference = abis.split(",");
            String abi = preference[0];
            choose:
            for (String candidate : preference) {
                for (Path m : members) {
                    if (hasLibs(dir.resolve(m.getFileName()), candidate)) {
                        abi = candidate;
                        break choose;
                    }
                }
            }
            int libs = 0;
            Path libDir = dir.resolve("lib").resolve(isaOf(abi));
            for (Path m : members) libs += extractLibs(dir.resolve(m.getFileName()), abi, libDir);
            List<Path> installed = new ArrayList<>();
            for (Path m : members) installed.add(dir.resolve(m.getFileName()));
            for (String other : preference) {
                if (other.equals(abi)) continue;
                installOnlyIn(installed, abi, other, dir.resolve("lib").resolve(isaOf(other)));
            }

            Files.writeString(dir.resolve(".installed"), members.size() + " " + Files.size(source) + ":"
                    + Files.getLastModifiedTime(source).toMillis() + "\n");
            Path base = dir.resolve("base.apk");
            Files.writeString(record, base.toString(), StandardCharsets.UTF_8);
            System.err.println("[install] " + pkg + ": " + members.size() + " APK(s), " + libs + " "
                    + abi + " libraries -> " + dir);
            return 0;
        } finally {
            if (staging != null) deleteTree(staging);
        }
    }

    // ---- one install session, as PackageInstallerSession checks it ------------------------

    /** --set: a line per file, "<package>\t<split name>\t<file>", into `out`; 0, or 1 when it cannot be
     *  written. A file that is no APK has an empty package: the launcher installs it alone, and it fails
     *  there as it would. */
    static int writeSet(Path out, List<String> files) {
        StringBuilder text = new StringBuilder();
        for (String f : files) {
            ApkLite a = null;
            try {
                a = apkLite(Paths.get(f));
            } catch (IOException | RuntimeException notAnApk) {
                // no package: see above
            }
            text.append(a == null || a.packageName == null ? "" : a.packageName).append('\t')
                    .append(a == null || a.splitName == null ? "" : a.splitName).append('\t')
                    .append(f).append('\n');
        }
        try {
            Files.writeString(out, text, StandardCharsets.UTF_8);
            return 0;
        } catch (IOException e) {
            System.err.println("[install] " + e);
            return 1;
        }
    }

    /** What the installer reads of an APK before it installs it (ApkLiteParseUtils.parseApkLite):
     *  the manifest's package, split, version code and split types, and <application>'s
     *  isSplitRequired. */
    static final class ApkLite {
        String packageName;
        String splitName;  // null: a base
        long versionCode;
        boolean splitRequired;
        Set<String> requiredSplitTypes = new HashSet<>();
        Set<String> splitTypes = new HashSet<>();
    }

    /** The APK's ApkLite, or null when it has no AndroidManifest.xml. */
    static ApkLite apkLite(Path apk) throws IOException {
        byte[] axml = manifestOf(apk);
        if (axml == null) return null;
        Map<String, Object> root = elementAttributes(axml, null);
        Map<String, Object> app = elementAttributes(axml, "application");
        ApkLite a = new ApkLite();
        a.packageName = root.get("package") instanceof String p ? p : null;
        String split = root.get("split") instanceof String sp ? sp : null;
        a.splitName = split == null || split.isEmpty() ? null : split;
        // PackageInfo.composeLongVersionCode(versionCodeMajor, versionCode).
        long code = root.get("versionCode") instanceof Integer c ? c & 0xffffffffL : 0;
        long major = root.get("versionCodeMajor") instanceof Integer m ? m : 0;
        a.versionCode = (major << 32) | code;
        a.splitRequired = Boolean.TRUE.equals(app.get("isSplitRequired"));
        a.requiredSplitTypes = namesOf(root.get("requiredSplitTypes"));
        a.splitTypes = namesOf(root.get("splitTypes"));
        return a;
    }

    /** ApkLiteParseUtils.separateNamesToSet: a comma-separated attribute as a set. */
    static Set<String> namesOf(Object value) {
        Set<String> out = new HashSet<>();
        if (!(value instanceof String s)) return out;
        for (String n : s.split(",")) if (!n.isEmpty()) out.add(n);
        return out;
    }

    /** PackageInstallerSession.validateApkInstallLocked's checks of a full install's APKs (AOSP 14), with
     *  its errors: null when the APKs install as one app, else the device's error code and message. */
    static String validateSession(List<Path> apks) throws IOException {
        String pkg = null;
        long version = 0;
        ApkLite base = null;
        Set<String> splits = new HashSet<>();  // "" stands for the base, which no split name can be
        Set<String> stagedSplitTypes = new HashSet<>();
        for (Path file : apks) {
            ApkLite a;
            try {
                a = apkLite(file);
            } catch (RuntimeException malformed) {
                a = null;
            }
            String name = file.getFileName().toString();
            if (a == null || a.packageName == null)
                return "INSTALL_PARSE_FAILED_NOT_APK: Failed to parse " + name;
            if (a.splitName != null && !isFilenameSafe("split_" + a.splitName + ".apk"))
                return "INSTALL_PARSE_FAILED_BAD_MANIFEST: Invalid split name " + a.splitName + " in " + name;
            if (!splits.add(a.splitName == null ? "" : a.splitName))
                return "INSTALL_FAILED_INVALID_APK: Split " + a.splitName + " was defined multiple times";
            if (pkg == null) {
                pkg = a.packageName;
                version = a.versionCode;
            } else if (!pkg.equals(a.packageName)) {
                return "INSTALL_FAILED_INVALID_APK: Inconsistent package " + a.packageName + " in " + name
                        + "; expected " + pkg;
            } else if (version != a.versionCode) {
                return "INSTALL_FAILED_INVALID_APK: Inconsistent version " + a.versionCode + " in " + name
                        + "; expected " + version;
            }
            if (a.splitName == null) base = a;
            stagedSplitTypes.addAll(a.splitTypes);
        }
        if (base == null) return "INSTALL_FAILED_INVALID_APK: Full install must include a base package";
        if (base.splitRequired && (splits.size() <= 1 || !stagedSplitTypes.containsAll(base.requiredSplitTypes)))
            return "INSTALL_FAILED_MISSING_SPLIT: Missing split for " + pkg;
        return null;
    }

    /** The session's APKs renamed in place as the device names staged ones, by their manifests
     *  (PackageInstallerSession: "base.apk", "split_" + splitName + ".apk"), the base first. Run after
     *  validateSession, so the names are safe and distinct. */
    static List<Path> nameAsInstalled(List<Path> apks) throws IOException {
        List<Path> moved = new ArrayList<>();
        List<String> targets = new ArrayList<>();
        for (int i = 0; i < apks.size(); i++) {
            ApkLite a = apkLite(apks.get(i));
            targets.add(a.splitName == null ? "base.apk" : "split_" + a.splitName + ".apk");
            // Through a name no APK has, so that two APKs may swap names.
            Path tmp = apks.get(i).resolveSibling(".staged-" + i);
            Files.move(apks.get(i), tmp);
            moved.add(tmp);
        }
        List<Path> out = new ArrayList<>();
        for (int i = 0; i < moved.size(); i++) {
            Path target = moved.get(i).resolveSibling(targets.get(i));
            Files.move(moved.get(i), target);
            if (targets.get(i).equals("base.apk")) out.add(0, target);
            else out.add(target);
        }
        return out;
    }

    /** The splits the library keeps beside an app that came as a bundle: <name>.splits/split_*.apk next to
     *  <name>.apk, hard links to the APKs an earlier install of the bundle installed (the launcher's
     *  library_files.h). Sorted by name; empty for an APK with no such folder. */
    static List<Path> keptSplits(Path apk) {
        String file = apk.getFileName().toString();
        Path dir = apk.resolveSibling(file.substring(0, file.length() - ".apk".length()) + ".splits");
        List<Path> out = new ArrayList<>();
        if (!Files.isDirectory(dir)) return out;
        try (DirectoryStream<Path> kids = Files.newDirectoryStream(dir, "split_*.apk")) {
            for (Path k : kids) if (Files.isRegularFile(k)) out.add(k);
        } catch (IOException e) {
            System.err.println("[install] cannot list " + dir + ": " + e);
        }
        out.sort(Comparator.comparing(p -> p.getFileName().toString()));
        return out;
    }

    /** A hard link where the volume allows one, else a copy. */
    static void link(Path source, Path target) throws IOException {
        try {
            Files.createLink(target, source.toAbsolutePath());
        } catch (IOException | UnsupportedOperationException e) {
            Files.copy(source, target, StandardCopyOption.REPLACE_EXISTING);
        }
    }

    /** An APK, by its name or, for a file named as a bundle (.xapk, .apkm, .apks), by what it holds: its
     *  own AndroidManifest.xml at the top, as Android's installer reads an APK whatever it is called
     *  (a store's "XAPK" of an app with no splits is sometimes the APK itself). */
    static boolean isApk(Path source) {
        if (source.getFileName().toString().toLowerCase().endsWith(".apk")) return true;
        try (ZipFile zip = new ZipFile(source.toFile())) {
            return zip.getEntry("AndroidManifest.xml") != null;
        } catch (IOException e) {
            return false;
        }
    }

    static boolean unpackBundle(Path bundle, Path dir, List<Path> members) throws IOException {
        try (ZipFile zip = new ZipFile(bundle.toFile())) {
            List<ZipEntry> apks = new ArrayList<>();
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                ZipEntry z = e.nextElement();
                if (!z.isDirectory() && z.getName().endsWith(".apk") && isFilenameSafe(baseName(z))) apks.add(z);
            }
            if (apks.isEmpty()) {
                System.err.println("[install] no APK inside " + bundle);
                return false;
            }
            for (Map.Entry<ZipEntry, String> m : select(apks, xapkBase(zip), packageGuess(bundle)).entrySet()) {
                Path target = dir.resolve(m.getValue());
                try (InputStream in = zip.getInputStream(m.getKey())) {
                    Files.copy(in, target, StandardCopyOption.REPLACE_EXISTING);
                }
                members.add(target);
                System.err.println("[install] " + m.getKey().getName() + " -> " + m.getValue());
            }
        }
        return true;
    }

    /** The libraries of `other` that `abi` lacks, with the libraries of
     *  `other` they need (DT_NEEDED, transitively), into `libDir`. */
    static void installOnlyIn(List<Path> apks, String abi, String other, Path libDir) throws IOException {
        Set<String> have = libNames(apks, abi);
        Set<String> theirs = libNames(apks, other);
        if (have.isEmpty() || theirs.isEmpty()) return;
        Set<String> missing = new TreeSet<>(theirs);
        missing.removeAll(have);
        if (missing.isEmpty()) return;
        Set<String> take = new LinkedHashSet<>();
        ArrayDeque<String> queue = new ArrayDeque<>(missing);
        while (!queue.isEmpty()) {
            String name = queue.poll();
            if (!take.add(name)) continue;
            byte[] elf = libBytes(apks, other, name);
            if (elf == null) continue;
            for (String dep : neededOf(elf)) {
                if (theirs.contains(dep) && !take.contains(dep)) queue.add(dep);
            }
        }
        int n = 0;
        Files.createDirectories(libDir);
        for (String name : take) {
            byte[] elf = libBytes(apks, other, name);
            if (elf == null) continue;
            Files.write(libDir.resolve(name), elf);
            n++;
        }
        System.err.println("[install] " + other + " has " + missing.size() + " librar"
                + (missing.size() == 1 ? "y" : "ies") + " " + abi + " lacks " + missing + ": " + n + " " + other
                + " libraries (with what they need) -> " + libDir);
    }

    /** The file names of lib/<abi>/*.so across the APKs. */
    static Set<String> libNames(List<Path> apks, String abi) throws IOException {
        Set<String> names = new TreeSet<>();
        String prefix = "lib/" + abi + "/";
        for (Path apk : apks) {
            try (ZipFile zip = new ZipFile(apk.toFile())) {
                for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                    String name = e.nextElement().getName();
                    if (!name.startsWith(prefix) || !name.endsWith(".so")) continue;
                    String file = name.substring(prefix.length());
                    if (isFilenameSafe(file)) names.add(file);
                }
            }
        }
        return names;
    }

    /** lib/<abi>/<name> from the first APK that has it, or null. */
    static byte[] libBytes(List<Path> apks, String abi, String name) throws IOException {
        for (Path apk : apks) {
            try (ZipFile zip = new ZipFile(apk.toFile())) {
                ZipEntry z = zip.getEntry("lib/" + abi + "/" + name);
                if (z == null) continue;
                try (InputStream in = zip.getInputStream(z)) {
                    return in.readAllBytes();
                }
            }
        }
        return null;
    }

    /** DT_NEEDED of an ELF64 little-endian shared library: the dynamic
     *  section's string table offsets, read through the PT_LOAD that maps the
     *  table. Empty for anything else. */
    static List<String> neededOf(byte[] elf) {
        List<String> out = new ArrayList<>();
        ByteBuffer b = ByteBuffer.wrap(elf).order(ByteOrder.LITTLE_ENDIAN);
        try {
            if (elf.length < 64 || b.getInt(0) != 0x464c457f || elf[4] != 2 || elf[5] != 1) return out;
            long phoff = b.getLong(32);
            int phentsize = b.getShort(54) & 0xffff, phnum = b.getShort(56) & 0xffff;
            long dynOff = -1, dynSize = 0;
            List<long[]> loads = new ArrayList<>();  // {vaddr, offset, filesz}
            for (int i = 0; i < phnum; i++) {
                int p = (int) (phoff + (long) i * phentsize);
                int type = b.getInt(p);
                if (type == 1) loads.add(new long[] {b.getLong(p + 16), b.getLong(p + 8), b.getLong(p + 32)});
                if (type == 2) {
                    dynOff = b.getLong(p + 8);
                    dynSize = b.getLong(p + 32);
                }
            }
            if (dynOff < 0) return out;
            long strtab = -1;
            List<Long> needed = new ArrayList<>();
            for (long q = dynOff; q + 16 <= dynOff + dynSize; q += 16) {
                long tag = b.getLong((int) q), val = b.getLong((int) q + 8);
                if (tag == 0) break;
                if (tag == 1) needed.add(val);
                if (tag == 5) strtab = val;
            }
            long strOff = -1;
            for (long[] l : loads) {
                if (strtab >= l[0] && strtab < l[0] + l[2]) strOff = strtab - l[0] + l[1];
            }
            if (strOff < 0) return out;
            for (long n : needed) {
                int start = (int) (strOff + n), end = start;
                while (end < elf.length && elf[end] != 0) end++;
                out.add(new String(elf, start, end - start, StandardCharsets.UTF_8));
            }
        } catch (RuntimeException malformed) {
            // Not an ELF this reader understands: no dependencies known.
        }
        return out;
    }

    /** The ABIs one APK has lib/<abi>/*.so for. */
    static Set<String> libAbis(Path apk) throws IOException {
        Set<String> out = new TreeSet<>();
        try (ZipFile zip = new ZipFile(apk.toFile())) {
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                String name = e.nextElement().getName();
                if (!name.startsWith("lib/") || !name.endsWith(".so")) continue;
                int slash = name.indexOf('/', 4);
                if (slash > 4 && name.indexOf('/', slash + 1) < 0) out.add(name.substring(4, slash));
            }
        }
        return out;
    }

    /** Whether one APK has lib/<abi>/*.so. */
    static boolean hasLibs(Path apk, String abi) throws IOException {
        String prefix = "lib/" + abi + "/";
        try (ZipFile zip = new ZipFile(apk.toFile())) {
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                String name = e.nextElement().getName();
                if (name.startsWith(prefix) && name.endsWith(".so")) return true;
            }
        }
        return false;
    }

    /** lib/<abi>/*.so of one APK into libDir; returns how many. */
    static int extractLibs(Path apk, String abi, Path libDir) throws IOException {
        int n = 0;
        String prefix = "lib/" + abi + "/";
        try (ZipFile zip = new ZipFile(apk.toFile())) {
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                ZipEntry z = e.nextElement();
                String name = z.getName();
                if (z.isDirectory() || !name.startsWith(prefix) || !name.endsWith(".so")) continue;
                String file = name.substring(prefix.length());
                if (!isFilenameSafe(file)) continue;
                Files.createDirectories(libDir);
                try (InputStream in = zip.getInputStream(z)) {
                    Files.copy(in, libDir.resolve(file), StandardCopyOption.REPLACE_EXISTING);
                }
                n++;
            }
        }
        return n;
    }

    /** PackageParser.validateName(name, requireSeparator, requireFilename) for a manifest's package: segments of
     *  a letter then letters, digits or '_', at least one '.', and a valid file name (FileUtils.isValidExtFilename:
     *  not "." or ".."). Null when valid, else why not. The package names the installed directory, the data
     *  directory and the library's files, so a name with a path in it must never get that far. */
    static String validatePackageName(String name) {
        boolean hasSep = false, front = true;
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                front = false;
                continue;
            }
            if (!front && ((c >= '0' && c <= '9') || c == '_')) continue;
            if (c == '.') {
                hasSep = true;
                front = true;
                continue;
            }
            return "bad character '" + c + "'";
        }
        if (name.isEmpty() || name.equals(".") || name.equals("..")) return "Invalid filename";
        return hasSep ? null : "must have at least one '.' separator";
    }

    /** NativeLibraryHelper's isFilenameSafe (android.os.FileUtils.isFilenameSafe) for one file name inside an
     *  APK: letters, digits and + , - . = _ only. A library or split named otherwise is skipped, as a device's
     *  installer skips it; on Windows a '\\' or ':' in it would otherwise name a file outside the app. */
    static boolean isFilenameSafe(String name) {
        if (name.isEmpty()) return false;
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            boolean ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+'
                    || c == ',' || c == '-' || c == '.' || c == '=' || c == '_';
            if (!ok) return false;
        }
        return true;
    }

    /** VMRuntime.getInstructionSet: the directory name an ABI installs under. */
    static String isaOf(String abi) {
        switch (abi) {
            case "arm64-v8a": return "arm64";
            case "armeabi-v7a": case "armeabi": return "arm";
            default: return abi;  // x86_64, x86, riscv64
        }
    }

    // ---- the package name, from the binary AndroidManifest.xml ------------------

    /** The package of an APK or a bundle (whose base APK is unpacked to a
     *  scratch folder next to it and removed again). */
    static String packageOf(Path source) throws IOException {
        if (isApk(source)) return packageName(source);
        Path scratch = Files.createTempDirectory(source.toAbsolutePath().getParent(), ".package-");
        try {
            List<Path> members = new ArrayList<>();
            return unpackBundle(source, scratch, members) && !members.isEmpty() ? packageName(members.get(0)) : null;
        } finally {
            deleteTree(scratch);
        }
    }

    /** The `package` attribute of the manifest's root element. */
    static String packageName(Path apk) throws IOException {
        byte[] axml = manifestOf(apk);
        return axml == null ? null : rootAttribute(axml, "package");
    }

    /** The APK's binary AndroidManifest.xml, or null when it has none. */
    static byte[] manifestOf(Path apk) throws IOException {
        try (ZipFile zip = new ZipFile(apk.toFile())) {
            ZipEntry m = zip.getEntry("AndroidManifest.xml");
            if (m == null) return null;
            try (InputStream in = zip.getInputStream(m)) {
                return in.readAllBytes();
            }
        }
    }

    /** A string attribute of the first element of a binary XML document. */
    static String rootAttribute(byte[] axml, String attr) {
        return elementAttributes(axml, null).get(attr) instanceof String s ? s : null;
    }

    /** The attributes of the first element named `element` (null: the first element of all) of a binary XML
     *  document, by name: a String, an Integer (decimal or hex) or a Boolean each. Empty when there is no such
     *  element or the document is not one this reader understands. */
    static Map<String, Object> elementAttributes(byte[] axml, String element) {
        Map<String, Object> out = new LinkedHashMap<>();
        try {
            ByteBuffer b = ByteBuffer.wrap(axml).order(ByteOrder.LITTLE_ENDIAN);
            List<String> strings = null;
            int pos = b.getShort(2) & 0xffff;  // the document header's size
            while (pos + 8 <= axml.length) {
                int type = b.getShort(pos) & 0xffff;
                int headerSize = b.getShort(pos + 2) & 0xffff;
                int size = b.getInt(pos + 4);
                if (size <= 0) break;
                if (type == 0x0001) {
                    strings = stringPool(b, pos);
                } else if (type == 0x0102 && strings != null) {
                    int tag = b.getInt(pos + headerSize + 4);
                    String tagName = tag >= 0 && tag < strings.size() ? strings.get(tag) : null;
                    if (element == null || element.equals(tagName)) {
                        int attrStart = b.getShort(pos + headerSize + 8) & 0xffff;
                        int attrSize = b.getShort(pos + headerSize + 10) & 0xffff;
                        int count = b.getShort(pos + headerSize + 12) & 0xffff;
                        for (int i = 0; i < count; i++) {
                            int a = pos + headerSize + attrStart + i * attrSize;
                            int name = b.getInt(a + 4);
                            if (name < 0 || name >= strings.size()) continue;
                            int raw = b.getInt(a + 8);
                            int dataType = b.get(a + 15) & 0xff, data = b.getInt(a + 16);
                            Object value = null;
                            if (raw >= 0 && raw < strings.size()) value = strings.get(raw);
                            else if (dataType == 0x03 && data >= 0 && data < strings.size()) value = strings.get(data);
                            else if (dataType == 0x10 || dataType == 0x11) value = data;
                            else if (dataType == 0x12) value = data != 0;
                            if (value != null) out.putIfAbsent(strings.get(name), value);
                        }
                        return out;
                    }
                }
                pos += size;
            }
        } catch (RuntimeException malformed) {
            out.clear();
        }
        return out;
    }

    static List<String> stringPool(ByteBuffer b, int pos) {
        int headerSize = b.getShort(pos + 2) & 0xffff;
        int count = b.getInt(pos + 8);
        boolean utf8 = (b.getInt(pos + 16) & 0x100) != 0;
        int stringsStart = pos + b.getInt(pos + 20);
        List<String> out = new ArrayList<>(count);
        for (int i = 0; i < count; i++) {
            int s = stringsStart + b.getInt(pos + headerSize + i * 4);
            if (utf8) {
                s += (b.get(s) & 0x80) != 0 ? 2 : 1;  // length in characters
                int len = b.get(s) & 0xff;
                if ((len & 0x80) != 0) {
                    len = ((len & 0x7f) << 8) | (b.get(s + 1) & 0xff);
                    s += 2;
                } else {
                    s += 1;
                }
                out.add(new String(b.array(), s, len, StandardCharsets.UTF_8));
            } else {
                int len = b.getShort(s) & 0xffff;
                if ((len & 0x8000) != 0) {
                    len = ((len & 0x7fff) << 16) | (b.getShort(s + 2) & 0xffff);
                    s += 4;
                } else {
                    s += 2;
                }
                out.add(new String(b.array(), s, len * 2, StandardCharsets.UTF_16LE));
            }
        }
        return out;
    }

    // ---- which APKs of a bundle make up the app -----------------------------------

    /** Upstream's SelectBundleApks, returning each chosen entry with the file
     *  name it is installed under, the base first. */
    static Map<ZipEntry, String> select(List<ZipEntry> apks, String manifestBase, String pkg) {
        Map<ZipEntry, String> plan = new LinkedHashMap<>();

        // bundletool .apks: a split set under splits/ (and system/,
        // asset-slices/), named <module>-<config>.apk.
        List<ZipEntry> set = new ArrayList<>();
        set.addAll(under(apks, "system/"));
        set.addAll(under(apks, "splits/"));
        set.addAll(under(apks, "asset-slices/"));
        if (!under(apks, "splits/").isEmpty() || !under(apks, "system/").isEmpty()) {
            set.sort(Comparator.comparingInt(AppInstall::bundletoolRank).thenComparing(ZipEntry::getName));
            plan.put(set.get(0), "base.apk");
            for (ZipEntry z : set.subList(1, set.size())) plan.put(z, bundletoolSplitFile(baseName(z)));
            return plan;
        }
        for (ZipEntry z : apks) {
            if (z.getName().equals("universal.apk")) {
                plan.put(z, "base.apk");
                return plan;
            }
        }
        // Only whole-app alternatives: exactly one is the app, the largest.
        List<ZipEntry> whole = under(apks, "standalones/");
        if (whole.isEmpty()) whole = under(apks, "instant/");
        if (whole.isEmpty()) whole = under(apks, "archive/");
        if (!whole.isEmpty()) {
            plan.put(largest(whole), "base.apk");
            return plan;
        }

        // Root layout (.apkm, .xapk): the manifest.json's base, base.apk,
        // <package>.apk, or else the largest APK, which carries the DEX.
        ZipEntry base = null;
        if (manifestBase != null) base = named(apks, manifestBase);
        if (base == null) base = named(apks, "base.apk");
        if (base == null && pkg != null) base = named(apks, pkg + ".apk");
        if (base == null) base = largest(apks);
        plan.put(base, "base.apk");
        List<ZipEntry> rest = new ArrayList<>(apks);
        rest.remove(base);
        rest.sort(Comparator.comparing(ZipEntry::getName));
        for (ZipEntry z : rest) plan.put(z, rootSplitFile(baseName(z)));
        return plan;
    }

    /** system/ first, then the base module's master, then its config splits. */
    static int bundletoolRank(ZipEntry z) {
        if (z.getName().startsWith("system/")) return 0;
        String bn = baseName(z);
        if (bn.equals("base-master.apk")) return 1;
        if (bn.startsWith("base-")) return 2;
        return 3;
    }

    /** bundletool names a split <module>-<config>.apk ("base-xxhdpi.apk",
     *  "feature-master.apk"); the installed split name is the one the split's
     *  own manifest declares: config.<q> for the base module's config splits,
     *  <module> and <module>.config.<q> for a feature module's. */
    static String bundletoolSplitFile(String bn) {
        String stem = bn.substring(0, bn.length() - 4);
        int dash = stem.indexOf('-');
        if (dash < 0) return "split_" + stem + ".apk";
        String module = stem.substring(0, dash), config = stem.substring(dash + 1);
        if (module.equals("base")) return "split_config." + config + ".apk";
        if (config.equals("master")) return "split_" + module + ".apk";
        return "split_" + module + ".config." + config + ".apk";
    }

    /** APKMirror already names splits split_config.<q>.apk; APKPure names
     *  them config.<q>.apk; anything else keeps its own name. */
    static String rootSplitFile(String bn) {
        return bn.startsWith("split_") ? bn : "split_" + bn;
    }

    /** APKPure's manifest.json names the base: {"file": "x.apk", "id": "base"}. */
    static String xapkBase(ZipFile zip) throws IOException {
        ZipEntry m = zip.getEntry("manifest.json");
        if (m == null) return null;
        String json;
        try (InputStream in = zip.getInputStream(m)) {
            json = new String(in.readAllBytes(), StandardCharsets.UTF_8);
        }
        Matcher obj = Pattern.compile("\\{[^{}]*\\}").matcher(json);
        while (obj.find()) {
            String o = obj.group();
            if (!o.matches("(?s).*\"id\"\\s*:\\s*\"base\".*")) continue;
            Matcher file = Pattern.compile("\"file\"\\s*:\\s*\"([^\"]+)\"").matcher(o);
            if (file.find()) return file.group(1);
        }
        return null;
    }

    /** "com.example.app_1.2.3.xapk" -> "com.example.app": a hint only. */
    static String packageGuess(Path bundle) {
        String n = bundle.getFileName().toString();
        int dot = n.lastIndexOf('.');
        if (dot > 0) n = n.substring(0, dot);
        int us = n.indexOf('_');
        return us > 0 ? n.substring(0, us) : n;
    }

    static List<ZipEntry> under(List<ZipEntry> apks, String dir) {
        List<ZipEntry> out = new ArrayList<>();
        for (ZipEntry z : apks) if (z.getName().startsWith(dir)) out.add(z);
        return out;
    }

    static ZipEntry named(List<ZipEntry> apks, String name) {
        for (ZipEntry z : apks) if (z.getName().equals(name)) return z;
        return null;
    }

    static ZipEntry largest(List<ZipEntry> apks) {
        ZipEntry best = apks.get(0);
        for (ZipEntry z : apks) if (z.getSize() > best.getSize()) best = z;
        return best;
    }

    static String baseName(ZipEntry z) {
        String n = z.getName();
        return n.substring(n.lastIndexOf('/') + 1);
    }

    static void deleteTree(Path p) throws IOException {
        if (!Files.exists(p, java.nio.file.LinkOption.NOFOLLOW_LINKS)) return;
        if (Files.isDirectory(p, java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
            try (DirectoryStream<Path> kids = Files.newDirectoryStream(p)) {
                for (Path k : kids) deleteTree(k);
            }
        }
        Files.delete(p);
    }
}
