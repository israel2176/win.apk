package com.winapk.tools;

import com.macapk.loader.Manifest;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Enumeration;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.TreeSet;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Unified pre-install and library compatibility evaluator (R3).
 * Evaluates S1-S7 signals deterministically before copying into library/ or launching.
 */
public final class AppCompat {
    public static final int SCHEMA_VERSION = 1;
    public static final int ENGINE_VERSION = 1;
    public static final int MAX_SUPPORTED_SDK = 36;
    public static final int MAX_SUPPORTED_GLES = 0x00030002;
    public static final Set<String> SUPPORTED_ABIS = Set.of("x86_64", "arm64-v8a");

    private static final Set<String> HARD_UNSUPPORTED_FEATURES = Set.of(
        "android.hardware.type.watch",
        "android.hardware.type.automotive",
        "android.hardware.type.vr.headtracking",
        "android.hardware.vr.high_performance",
        "android.hardware.nfc",
        "android.hardware.nfc.hce",
        "android.hardware.nfc.hcef",
        "android.hardware.nfc.uicc",
        "android.hardware.nfc.beam",
        "android.hardware.fingerprint",
        "android.hardware.biometrics.face",
        "android.hardware.biometrics.iris",
        "android.hardware.strongbox_keystore",
        "android.hardware.consumerir",
        "android.hardware.uwb",
        "android.hardware.usb.host",
        "android.hardware.usb.accessory",
        "android.hardware.sensor.accelerometer",
        "android.hardware.sensor.gyroscope",
        "android.hardware.sensor.compass",
        "android.hardware.sensor.barometer",
        "android.hardware.sensor.heartrate",
        "android.hardware.sensor.stepcounter",
        "android.hardware.sensor.proximity",
        "android.hardware.sensor.light",
        "android.hardware.telephony",
        "android.hardware.telephony.gsm",
        "android.hardware.telephony.cdma",
        "android.hardware.telephony.ims",
        "android.hardware.telephony.euicc",
        "android.hardware.vulkan.level",
        "android.hardware.vulkan.version",
        "android.hardware.vulkan.compute"
    );

    private static final Set<String> SUPPORTED_LIBRARIES = Set.of(
        "org.apache.http.legacy",
        "androidx.window.sidecar",
        "com.google.android.maps"
    );

    public static final class Result {
        public String pkg = "";
        public long versionCode = 0;
        public String apkStamp = "";
        public String verdict = "supported"; // supported | unopenable | unsupported
        public String reasonCode = "OK";
        public String reasonDetail = "";
        public final Set<String> abis = new TreeSet<>();
        public int minSdk = 1;
        public int targetSdk = 0;
        public int glEsVersion = 0x00020000;
        public boolean hasLauncher = false;
        public int startableCount = 0;
        public final List<String> missingFeatures = new ArrayList<>();
        public final List<String> missingLibraries = new ArrayList<>();
        public final List<String> engineSignatures = new ArrayList<>();

        public boolean isInstallAllowed() {
            return !"unsupported".equals(verdict);
        }

        public String toJson() {
            StringBuilder sb = new StringBuilder();
            sb.append("{\n");
            sb.append("  \"schema\": ").append(SCHEMA_VERSION).append(",\n");
            sb.append("  \"engineVersion\": ").append(ENGINE_VERSION).append(",\n");
            sb.append("  \"apkStamp\": ").append(jsonStr(apkStamp)).append(",\n");
            sb.append("  \"package\": ").append(jsonStr(pkg)).append(",\n");
            sb.append("  \"versionCode\": ").append(versionCode).append(",\n");
            sb.append("  \"verdict\": ").append(jsonStr(verdict)).append(",\n");
            sb.append("  \"reasonCode\": ").append(jsonStr(reasonCode)).append(",\n");
            sb.append("  \"reasonDetail\": ").append(jsonStr(reasonDetail)).append(",\n");
            sb.append("  \"signals\": {\n");
            sb.append("    \"abis\": ").append(jsonArr(abis)).append(",\n");
            sb.append("    \"minSdk\": ").append(minSdk).append(",\n");
            sb.append("    \"targetSdk\": ").append(targetSdk).append(",\n");
            sb.append("    \"glEsVersion\": ").append(glEsVersion).append(",\n");
            sb.append("    \"hasLauncher\": ").append(hasLauncher).append(",\n");
            sb.append("    \"startableCount\": ").append(startableCount).append(",\n");
            sb.append("    \"missingFeatures\": ").append(jsonArr(missingFeatures)).append(",\n");
            sb.append("    \"missingLibraries\": ").append(jsonArr(missingLibraries)).append(",\n");
            sb.append("    \"engineSignatures\": ").append(jsonArr(engineSignatures)).append("\n");
            sb.append("  },\n");
            sb.append("  \"runtime\": {\n");
            sb.append("    \"consecutiveStartupFailures\": 0,\n");
            sb.append("    \"lastExitCode\": 0,\n");
            sb.append("    \"lastWasReady\": false,\n");
            sb.append("    \"lastFatalSignature\": \"\"\n");
            sb.append("  },\n");
            sb.append("  \"userOverride\": false\n");
            sb.append("}\n");
            return sb.toString();
        }
    }

    private AppCompat() {}

    public static Result evaluate(List<Path> members) {
        Result r = new Result();
        if (members == null || members.isEmpty()) {
            r.verdict = "unsupported";
            r.reasonCode = "INVALID_APK";
            r.reasonDetail = "no APK members";
            return r;
        }
        Path base = members.get(0);
        try {
            r.apkStamp = Files.size(base) + ":" + Files.getLastModifiedTime(base).toMillis();
        } catch (IOException ignored) {}

        Manifest mf;
        try {
            mf = Manifest.parse(new File(""), base.toAbsolutePath().toString());
        } catch (Throwable t) {
            r.verdict = "unsupported";
            r.reasonCode = "INVALID_MANIFEST";
            r.reasonDetail = String.valueOf(t.getMessage());
            return r;
        }
        r.pkg = mf.packageName != null ? mf.packageName : "";
        r.versionCode = mf.versionCode;
        r.minSdk = mf.minSdk;
        r.targetSdk = mf.target;
        r.hasLauncher = mf.launchableActivity != null && !mf.launchableActivity.isEmpty();

        // S2: ABIs across all split members
        for (Path m : members) {
            r.abis.addAll(AppInstall.libAbis(m));
        }
        boolean abiOk = r.abis.isEmpty();
        for (String a : r.abis) {
            if (SUPPORTED_ABIS.contains(a)) {
                abiOk = true;
                break;
            }
        }
        if (!abiOk) {
            r.verdict = "unsupported";
            r.reasonCode = "UNSUPPORTED_ABI_32BIT_ARM";
            r.reasonDetail = "only " + String.join(",", r.abis);
            return r;
        }

        // S3: SDK & preview codename
        if (r.minSdk >= Manifest.PREVIEW_MIN_SDK || r.minSdk > MAX_SUPPORTED_SDK) {
            r.verdict = "unsupported";
            r.reasonCode = "UNSUPPORTED_MIN_SDK";
            r.reasonDetail = "minSdk=" + r.minSdk;
            return r;
        }

        // S4: Features (required=true only; location.gps allowed as soft warning)
        if (mf.requiredFeatures != null) {
            for (String feat : mf.requiredFeatures) {
                if ("android.hardware.location.gps".equals(feat)) continue;
                if (HARD_UNSUPPORTED_FEATURES.contains(feat)
                        || feat.startsWith("android.hardware.vulkan.")) {
                    r.missingFeatures.add(feat);
                }
            }
        }
        if (!r.missingFeatures.isEmpty()) {
            r.verdict = "unsupported";
            r.reasonCode = "UNSUPPORTED_FEATURE";
            r.reasonDetail = String.join(",", r.missingFeatures);
            return r;
        }

        // S5: Libraries (<uses-library> / <uses-native-library>)
        if (mf.usesLibraries != null) {
            for (Manifest.UsesLibraryEntry lib : mf.usesLibraries) {
                if (!lib.required) continue;
                if (lib.isNative) {
                    if (!hasNativeLibInApks(members, lib.name)) {
                        r.missingLibraries.add(lib.name);
                    }
                } else if (!SUPPORTED_LIBRARIES.contains(lib.name)) {
                    r.missingLibraries.add(lib.name);
                }
            }
        }
        if (!r.missingLibraries.isEmpty()) {
            r.verdict = "unsupported";
            r.reasonCode = "UNSUPPORTED_LIBRARY";
            r.reasonDetail = String.join(",", r.missingLibraries);
            return r;
        }

        // S7: Unsupported engines / packers
        scanPackerSignatures(members, r);
        if (!r.engineSignatures.isEmpty()) {
            r.verdict = "unsupported";
            r.reasonCode = r.engineSignatures.get(0);
            r.reasonDetail = String.join(",", r.engineSignatures);
            return r;
        }

        // S6: Entrypoints (preserves #143 startable activities and background plugins)
        if (mf.activities != null) {
            for (Manifest.ActivityEntry a : mf.activities) {
                if (a.exported && !a.alias) r.startableCount++;
            }
        }
        if (!r.hasLauncher && r.startableCount == 0) {
            r.verdict = "unopenable";
            r.reasonCode = "UNOPENABLE_NO_ACTIVITY";
            r.reasonDetail = "no launchable or exported activity";
        }
        return r;
    }

    private static boolean hasNativeLibInApks(List<Path> members, String soname) {
        String suffix = "/" + soname;
        for (Path m : members) {
            try (ZipFile zf = new ZipFile(m.toFile())) {
                Enumeration<? extends ZipEntry> en = zf.entries();
                while (en.hasMoreElements()) {
                    String n = en.nextElement().getName();
                    if (n.startsWith("lib/") && n.endsWith(suffix)) return true;
                }
            } catch (IOException ignored) {}
        }
        return false;
    }

    private static void scanPackerSignatures(List<Path> members, Result r) {
        Set<String> sigs = new LinkedHashSet<>();
        boolean hasMozglue = false, hasXul = false, hasXposedInit = false;
        for (Path m : members) {
            try (ZipFile zf = new ZipFile(m.toFile())) {
                Enumeration<? extends ZipEntry> en = zf.entries();
                while (en.hasMoreElements()) {
                    String name = en.nextElement().getName();
                    String low = name.toLowerCase(Locale.ROOT);
                    if (low.endsWith("/libpairipcore.so")) sigs.add("SIG_PAIRIP");
                    else if (low.endsWith("/libnesec.so")) sigs.add("SIG_NETEASE_WRAPPER");
                    else if (low.endsWith("/libsgmain.so") || low.endsWith("/libsgsecuritybody.so")) sigs.add("SIG_ALIBABA_SG");
                    else if (low.contains("/libjiagu") || low.endsWith("/libdexhelper.so")
                            || low.contains("/libshella") || low.endsWith("/libexec.so")
                            || low.endsWith("/libtup.so")) sigs.add("SIG_DEX_PACKERS");
                    else if (low.endsWith("/libmozglue.so")) hasMozglue = true;
                    else if (low.endsWith("/libxul.so")) hasXul = true;
                    else if ("assets/xposed_init".equals(low)) hasXposedInit = true;
                }
            } catch (IOException ignored) {}
        }
        if (hasMozglue && hasXul) sigs.add("SIG_GECKO_MULTIPROC");
        if (hasXposedInit && !r.hasLauncher) sigs.add("SIG_XPOSED_MODULE");
        r.engineSignatures.addAll(sigs);
    }

    private static String jsonStr(String s) {
        if (s == null) return "\"\"";
        StringBuilder sb = new StringBuilder("\"");
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '"' || c == '\\') sb.append('\\').append(c);
            else if (c == '\n') sb.append("\\n");
            else if (c == '\r') sb.append("\\r");
            else if (c == '\t') sb.append("\\t");
            else sb.append(c);
        }
        return sb.append('"').toString();
    }

    private static String jsonArr(Iterable<String> items) {
        StringBuilder sb = new StringBuilder("[");
        boolean first = true;
        for (String item : items) {
            if (!first) sb.append(", ");
            sb.append(jsonStr(item));
            first = false;
        }
        return sb.append(']').toString();
    }

    public static void main(String[] args) throws Exception {
        List<Path> members = new ArrayList<>();
        Path out = null;
        for (int i = 0; i < args.length; i++) {
            if ("--out".equals(args[i]) && i + 1 < args.length) {
                out = Path.of(args[++i]);
            } else if (!"--splits".equals(args[i])) {
                members.add(Path.of(args[i]));
            }
        }
        Result res = evaluate(members);
        String json = res.toJson();
        if (out != null) Files.writeString(out, json, StandardCharsets.UTF_8);
        else System.out.print(json);
        System.exit(res.isInstallAllowed() ? 0 : 3);
    }
}
