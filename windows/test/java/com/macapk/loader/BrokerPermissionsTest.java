// BrokerPermissionsTest -- the permission checks the system broker makes when one app reaches
// another's component (com.macapk.loader.BrokerPermissions), as AOSP 14's system_server makes them,
// on APKs the test signs (apksig's ApkSigner, keys keytool makes) so the signature checks are the
// real ones (ApkVerification):
//   - a signature permission: held by a package signed as its declarer is, not by one signed by
//     another key, not by one that does not request it; the declarer itself holds it;
//   - Google services' case: Play services declares com.google.android.c2dm.permission.SEND
//     (signature); Play services and the Play Store, signed by one key, hold it and may send to an
//     app's FCM receiver that requires it; an app signed otherwise may not; the app's own
//     <pkg>.permission.C2D_MESSAGE (signature, declared by the app) is the app's;
//   - a normal permission: held when requested, not when not requested; a permission nobody
//     declares: held by nobody;
//   - the platform's (framework-res-34.apk): INTERNET normal, BIND_JOB_SERVICE signature (nobody's),
//     INSTALL_PACKAGES signature|privileged (a privileged system app's only); CAMERA dangerous (the
//     user's grant, or an app targeting API 22);
//   - checkComponentPermission: the same app always, an unexported component nobody else;
//   - a provider: opening it (read or write permission, a path-permission's, a URI grant), each
//     operation's read or write permission and its path-permissions, AOSP's messages;
//   - URI grants: from the provider's app to another (then that uri, not another, not for
//     writing), refused when the provider does not allow granting or the granter has no access, a
//     prefix grant, an intent's data and ClipData, revoked, and gone with a package.
//
//   java -cp android-framework.jar;macapk-loader.jar;<classes> com.macapk.loader.BrokerPermissionsTest
//        <framework-res-34.apk> <scratch dir>
// Prints PASS/FAIL per check and "ALL PASSED" at the end; exits 1 on a failure.
package com.macapk.loader;

import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;
import com.android.apksig.ApkSigner;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.file.Files;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

public final class BrokerPermissionsTest {
    static int failures;
    static File scratch;

    static void check(String name, boolean ok) { check(name, ok, ""); }

    static void check(String name, boolean ok, String detail) {
        System.out.println("  [" + (ok ? "PASS" : "FAIL") + "] " + name + (detail.isEmpty() ? "" : ": " + detail));
        if (!ok) failures++;
    }

    static final String GMS = "com.google.android.gms", VENDING = "com.android.vending";
    static final String APP = "com.example.app", EVIL = "com.example.evil", OLD = "com.example.old";
    static final String SEND = "com.google.android.c2dm.permission.SEND";
    static final String C2D = APP + ".permission.C2D_MESSAGE";
    static final String NORMAL = APP + ".permission.NORMAL", READ = APP + ".permission.READ",
            WRITE = APP + ".permission.WRITE", SECRET = APP + ".permission.SECRET";

    /** The installed packages of the test: manifest, signed APK, privileged, the user's grants. */
    static final Map<String, Manifest> manifests = new LinkedHashMap<>();
    static final Map<String, String> apks = new HashMap<>();
    static final Map<String, List<String>> granted = new HashMap<>();
    static final List<String> privileged = new ArrayList<>();

    static final BrokerPermissions.Packages PACKAGES = new BrokerPermissions.Packages() {
        @Override public Manifest manifest(String pkg) { return manifests.get(pkg); }
        @Override public ApkVerification.Result signers(String pkg) { return ApkVerification.of(apks.get(pkg)); }
        @Override public boolean privileged(String pkg) { return privileged.contains(pkg); }
        @Override public boolean runtimeGranted(String pkg, Manifest m, String perm) {
            return granted.getOrDefault(pkg, List.of()).contains(perm);
        }
        @Override public int uid(String pkg) {
            int i = new ArrayList<>(manifests.keySet()).indexOf(pkg);
            return i < 0 ? -1 : 10_000 + i;
        }
        @Override public Iterable<String> all() { return new ArrayList<>(manifests.keySet()); }
    };

    static final BrokerPermissions.ProviderLookup LOOKUP = authority -> {
        for (Map.Entry<String, Manifest> e : manifests.entrySet()) {
            for (Manifest.ProviderEntry p : e.getValue().providers) {
                for (String a : p.authorities.split(";")) if (a.equals(authority)) return new Object[] { e.getKey(), p };
            }
        }
        return null;
    };

    static Manifest.ProviderEntry provider(String authority) { return (Manifest.ProviderEntry) LOOKUP.find(authority)[1]; }

    public static void main(String[] args) throws Exception {
        File framework = new File(args[0]);
        scratch = new File(args[1]);
        scratch.mkdirs();
        ApkVerification.setCacheDir(new File(scratch, "cache"));
        System.setProperty("winapk.aosp.res.framework", framework.getPath());

        // One small unsigned APK (the platform's manifest alone), signed by Google's stand-in key and
        // by another.
        File unsigned = new File(scratch, "unsigned.apk");
        try (ZipFile z = new ZipFile(framework); ZipOutputStream zo = new ZipOutputStream(new FileOutputStream(unsigned))) {
            ZipEntry m = z.getEntry("AndroidManifest.xml");
            zo.putNextEntry(new ZipEntry("AndroidManifest.xml"));
            try (InputStream in = z.getInputStream(m)) { in.transferTo(zo); }
            zo.closeEntry();
        }
        Key google = key("google"), other = key("other");
        String byGoogle = sign(unsigned, "google.apk", google).getPath();
        String byGoogleToo = sign(unsigned, "google2.apk", google).getPath();
        String byOther = sign(unsigned, "other.apk", other).getPath();
        String byOtherToo = sign(unsigned, "other2.apk", other).getPath();
        check("the test APKs verify", ApkVerification.of(byGoogle).verified && ApkVerification.of(byOther).verified,
                ApkVerification.of(byGoogle).reason + " / " + ApkVerification.of(byOther).reason);

        install(GMS, byGoogle, 34, ""
                + "<permission android:name=\"" + SEND + "\" android:protectionLevel=\"signature\"/>"
                + "<uses-permission android:name=\"" + SEND + "\"/>",
                "");
        install(VENDING, byGoogleToo, 34, "<uses-permission android:name=\"" + SEND + "\"/>"
                + "<uses-permission android:name=\"android.permission.INSTALL_PACKAGES\"/>", "");
        privileged.add(VENDING);
        install(APP, byOther, 34, ""
                + "<permission android:name=\"" + C2D + "\" android:protectionLevel=\"signature\"/>"
                + "<uses-permission android:name=\"" + C2D + "\"/>"
                + "<permission android:name=\"" + NORMAL + "\"/>"
                + "<permission android:name=\"" + READ + "\" android:protectionLevel=\"normal\"/>"
                + "<permission android:name=\"" + WRITE + "\" android:protectionLevel=\"signature\"/>"
                + "<permission android:name=\"" + SECRET + "\" android:protectionLevel=\"signature\"/>"
                + "<uses-permission android:name=\"android.permission.CAMERA\"/>",
                ""
                + "<receiver android:name=\".FcmReceiver\" android:exported=\"true\" android:permission=\"" + SEND + "\">"
                + "  <intent-filter><action android:name=\"com.google.android.c2dm.intent.RECEIVE\"/></intent-filter>"
                + "</receiver>"
                + "<service android:name=\".Guarded\" android:exported=\"true\" android:permission=\"" + NORMAL + "\"/>"
                + "<service android:name=\".Private\" android:exported=\"false\"/>"
                + "<provider android:name=\".Data\" android:authorities=\"" + APP + ".data\" android:exported=\"true\""
                + "    android:readPermission=\"" + READ + "\" android:writePermission=\"" + WRITE + "\"/>"
                + "<provider android:name=\".Paths\" android:authorities=\"" + APP + ".paths\" android:exported=\"true\">"
                + "  <path-permission android:pathPrefix=\"/secret\" android:readPermission=\"" + SECRET + "\"/>"
                + "</provider>"
                + "<provider android:name=\".Files\" android:authorities=\"" + APP + ".files\" android:exported=\"false\""
                + "    android:grantUriPermissions=\"true\"/>"
                + "<provider android:name=\".NoGrants\" android:authorities=\"" + APP + ".nogrants\""
                + "    android:exported=\"false\"/>"
                + "<provider android:name=\".SomeGrants\" android:authorities=\"" + APP + ".some\" android:exported=\"false\">"
                + "  <grant-uri-permission android:pathPrefix=\"/shared/\"/>"
                + "</provider>");
        install(EVIL, byOtherToo, 34, "<uses-permission android:name=\"" + SEND + "\"/>"
                + "<uses-permission android:name=\"" + READ + "\"/>"
                + "<uses-permission android:name=\"android.permission.INTERNET\"/>"
                + "<uses-permission android:name=\"android.permission.BIND_JOB_SERVICE\"/>"
                + "<uses-permission android:name=\"android.permission.INSTALL_PACKAGES\"/>"
                + "<uses-permission android:name=\"android.permission.CAMERA\"/>"
                + "<uses-permission android:name=\"com.example.NOBODY_DECLARES\"/>", "");
        install(OLD, byOther, 22, "<uses-permission android:name=\"android.permission.CAMERA\"/>"
                + "<uses-permission android:name=\"" + NORMAL + "\"/>", "");
        granted.put(APP, List.of("android.permission.CAMERA"));

        BrokerPermissions bp = new BrokerPermissions(PACKAGES);

        System.out.println("signature permissions");
        check("Play services, the declarer, holds c2dm SEND", bp.holds(GMS, SEND));
        check("the Play Store, signed by the same key, holds it", bp.holds(VENDING, SEND));
        check("an app signed by another key does not, though it requests it", !bp.holds(EVIL, SEND));
        check("Play services may send to an app's FCM receiver that requires SEND",
                bp.checkComponent(SEND, GMS, APP, true));
        check("another app may not", !bp.checkComponent(SEND, EVIL, APP, true));
        check("the app holds its own C2D_MESSAGE (it declares and requests it)", bp.holds(APP, C2D));
        check("Play services does not hold the app's C2D_MESSAGE (another signer, not requested)", !bp.holds(GMS, C2D));

        System.out.println("normal, unknown and platform permissions");
        check("a normal permission requested is held", bp.holds(OLD, NORMAL));
        check("not requested, it is not", !bp.holds(EVIL, NORMAL));
        check("a service guarded by a normal permission: open to the app that requests it",
                bp.checkComponent(NORMAL, OLD, APP, true));
        check("closed to one that does not", !bp.checkComponent(NORMAL, EVIL, APP, true));
        check("a permission nobody declares is held by nobody", !bp.holds(EVIL, "com.example.NOBODY_DECLARES"));
        check("platform INTERNET (normal) is held when requested", bp.holds(EVIL, "android.permission.INTERNET"));
        check("platform BIND_JOB_SERVICE (signature) is no app's", !bp.holds(EVIL, "android.permission.BIND_JOB_SERVICE"));
        check("platform INSTALL_PACKAGES (signature|privileged): a privileged app's",
                bp.holds(VENDING, "android.permission.INSTALL_PACKAGES"));
        check("  not another app's", !bp.holds(EVIL, "android.permission.INSTALL_PACKAGES"));
        check("platform CAMERA (dangerous): held once the user granted it", bp.holds(APP, "android.permission.CAMERA"));
        check("  not before", !bp.holds(EVIL, "android.permission.CAMERA"));
        check("  held at install by an app targeting API 22", bp.holds(OLD, "android.permission.CAMERA"));
        check("the system holds everything", bp.holds(null, SEND) && bp.checkComponent(SEND, null, APP, false));

        System.out.println("checkComponentPermission");
        check("the same app reaches its unexported component", bp.checkComponent(null, APP, APP, false));
        check("the same app needs no permission it guards with", bp.checkComponent(SECRET, APP, APP, true));
        check("another app does not reach an unexported component", !bp.checkComponent(null, EVIL, APP, false));
        check("an exported component with no permission is anyone's", bp.checkComponent(null, EVIL, APP, true));

        System.out.println("content providers");
        Manifest.ProviderEntry data = provider(APP + ".data");
        check("open: the read permission is enough", bp.providerOpenDenial(EVIL, APP, data, "(pid=1, uid=1)") == null);
        String denial = bp.providerOpenDenial(OLD, APP, data, "(pid=1, uid=1)");
        check("open without read or write permission: AOSP's denial", denial != null
                && denial.equals("Permission Denial: opening provider " + APP + ".Data from (pid=1, uid=1) requires "
                + READ + " or " + WRITE), String.valueOf(denial));
        Uri row = Uri.parse("content://" + APP + ".data/rows/1");
        check("query with the read permission", (Integer) bp.providerOp(EVIL, APP, data, row, false)[0]
                == BrokerPermissions.OP_ALLOWED);
        Object[] w = bp.providerOp(EVIL, APP, data, row, true);
        check("insert without the write permission is denied", (Integer) w[0] == BrokerPermissions.OP_DENIED
                && (" requires " + WRITE + ", or grantUriPermission()").equals(w[1]), String.valueOf(w[1]));
        check("the provider's own app may write", (Integer) bp.providerOp(APP, APP, data, row, true)[0]
                == BrokerPermissions.OP_ALLOWED);
        Manifest.ProviderEntry paths = provider(APP + ".paths");
        check("an unprotected path of a provider with path-permissions is readable",
                (Integer) bp.providerOp(EVIL, APP, paths, Uri.parse("content://" + APP + ".paths/public/1"), false)[0]
                        == BrokerPermissions.OP_ALLOWED_URI);
        Object[] sec = bp.providerOp(EVIL, APP, paths, Uri.parse("content://" + APP + ".paths/secret/1"), false);
        check("a path its path-permission guards is not", (Integer) sec[0] == BrokerPermissions.OP_DENIED
                && (" requires " + SECRET + ", or grantUriPermission()").equals(sec[1]), String.valueOf(sec[1]));

        System.out.println("URI grants");
        Manifest.ProviderEntry files = provider(APP + ".files");
        String open = bp.providerOpenDenial(EVIL, APP, files, "(pid=1, uid=1)");
        check("an unexported provider: not opened by another app", open != null
                && open.endsWith("that is not exported from uid " + PACKAGES.uid(APP)), String.valueOf(open));
        Uri photo = Uri.parse("content://" + APP + ".files/photos/1.jpg");
        Uri other1 = Uri.parse("content://" + APP + ".files/photos/2.jpg");
        check("another app cannot grant what it cannot reach",
                bp.grant(EVIL, OLD, photo, Intent.FLAG_GRANT_READ_URI_PERMISSION, false, LOOKUP) != null);
        String noGrants = bp.grant(APP, EVIL, Uri.parse("content://" + APP + ".nogrants/x"),
                Intent.FLAG_GRANT_READ_URI_PERMISSION, false, LOOKUP);
        check("a provider without grantUriPermissions refuses a grant", noGrants != null
                && noGrants.contains("does not allow granting of Uri permissions"), String.valueOf(noGrants));
        check("a <grant-uri-permission> path allows it",
                bp.grant(APP, EVIL, Uri.parse("content://" + APP + ".some/shared/a"),
                        Intent.FLAG_GRANT_READ_URI_PERMISSION, false, LOOKUP) == null);
        check("  only under that path", bp.grant(APP, EVIL, Uri.parse("content://" + APP + ".some/private/a"),
                Intent.FLAG_GRANT_READ_URI_PERMISSION, false, LOOKUP) != null);
        Intent share = new Intent(Intent.ACTION_SEND).setData(photo)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        check("an intent from the provider's app grants its data", bp.grantFromIntent(APP, EVIL, share, LOOKUP).isEmpty());
        check("the grant opens the unexported provider", bp.providerOpenDenial(EVIL, APP, files, "(pid=1, uid=1)") == null);
        check("  reads the granted uri", (Integer) bp.providerOp(EVIL, APP, files, photo, false)[0]
                == BrokerPermissions.OP_ALLOWED_URI);
        check("  not for writing", (Integer) bp.providerOp(EVIL, APP, files, photo, true)[0]
                == BrokerPermissions.OP_DENIED);
        check("  not another uri", (Integer) bp.providerOp(EVIL, APP, files, other1, false)[0]
                == BrokerPermissions.OP_DENIED);
        check("  not for a third app", (Integer) bp.providerOp(OLD, APP, files, photo, false)[0]
                == BrokerPermissions.OP_DENIED);
        check("the grantee may pass its grant on", bp.grant(EVIL, OLD, photo, Intent.FLAG_GRANT_READ_URI_PERMISSION,
                false, LOOKUP) == null && bp.uriAccess(OLD, APP, files, photo, Intent.FLAG_GRANT_READ_URI_PERMISSION));
        Uri clipUri = Uri.parse("content://" + APP + ".files/docs/a.pdf");
        Intent clip = new Intent(Intent.ACTION_SEND).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        clip.setClipData(ClipData.newRawUri("doc", clipUri));
        check("an intent's ClipData is granted too, read and write", bp.grantFromIntent(APP, EVIL, clip, LOOKUP).isEmpty()
                && (Integer) bp.providerOp(EVIL, APP, files, clipUri, true)[0] == BrokerPermissions.OP_ALLOWED_URI);
        Uri dir = Uri.parse("content://" + APP + ".files/albums/");
        check("a prefix grant covers the uris under it",
                bp.grant(APP, EVIL, dir, Intent.FLAG_GRANT_READ_URI_PERMISSION, true, LOOKUP) == null
                && bp.uriAccess(EVIL, APP, files, Uri.parse("content://" + APP + ".files/albums/9/cover"),
                        Intent.FLAG_GRANT_READ_URI_PERMISSION));
        bp.grants.revoke(APP, null, photo, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        check("revoked, the uri is closed again", (Integer) bp.providerOp(EVIL, APP, files, photo, false)[0]
                == BrokerPermissions.OP_DENIED);
        bp.grants.forget(EVIL);
        check("a removed package's grants go with it",
                !bp.uriAccess(EVIL, APP, files, clipUri, Intent.FLAG_GRANT_READ_URI_PERMISSION));

        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }

    static void install(String pkg, String apk, int target, String perms, String components) throws Exception {
        File xml = new File(scratch, pkg + ".xml");
        Files.writeString(xml.toPath(), "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                + "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"" + pkg + "\">\n"
                + "  <uses-sdk android:minSdkVersion=\"21\" android:targetSdkVersion=\"" + target + "\"/>\n"
                + "  " + perms + "\n"
                + "  <application>" + components + "</application>\n"
                + "</manifest>\n");
        manifests.put(pkg, Manifest.parse(xml, null));
        apks.put(pkg, apk);
    }

    // ---- signing (as ApkVerificationTest does) ---------------------------------------

    record Key(PrivateKey key, X509Certificate cert) {}

    static Key key(String alias) throws Exception {
        File ks = new File(scratch, alias + ".p12");
        ks.delete();
        boolean windows = File.separatorChar == '\\';
        String keytool = new File(System.getProperty("java.home"), windows ? "bin\\keytool.exe" : "bin/keytool")
                .getPath();
        Process p = new ProcessBuilder(keytool, "-genkeypair", "-keyalg", "EC", "-groupname", "secp256r1",
                "-sigalg", "SHA256withECDSA", "-dname", "CN=Test " + alias, "-validity", "10000", "-alias", alias,
                "-keystore", ks.getPath(), "-storetype", "PKCS12", "-storepass", "testpass", "-keypass", "testpass")
                .redirectErrorStream(true).start();
        String out = new String(p.getInputStream().readAllBytes());
        if (p.waitFor() != 0) throw new IllegalStateException("keytool: " + out);
        KeyStore store = KeyStore.getInstance("PKCS12");
        try (InputStream in = new FileInputStream(ks)) { store.load(in, "testpass".toCharArray()); }
        return new Key((PrivateKey) store.getKey(alias, "testpass".toCharArray()),
                (X509Certificate) store.getCertificate(alias));
    }

    static File sign(File in, String name, Key k) throws Exception {
        File out = new File(scratch, name);
        ApkSigner.SignerConfig config = new ApkSigner.SignerConfig.Builder("CERT", k.key(), List.of(k.cert())).build();
        new ApkSigner.Builder(List.of(config)).setInputApk(in).setOutputApk(out).setMinSdkVersion(24)
                .setV1SigningEnabled(false).setV2SigningEnabled(true).setV3SigningEnabled(true)
                .setV4SigningEnabled(false).setDebuggableApkPermitted(true).build().sign();
        return out;
    }
}
