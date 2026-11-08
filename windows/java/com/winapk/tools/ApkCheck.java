// ApkCheck -- what an APK or a bundle is and who signed it, for the launcher's
// Google services setup:
//
//     winapk-runtime --verify-apk <app.apk|.apkm|.xapk|.apks>
//
// The setup page installs a file from the Downloads folder only when it is one
// of Google's three apps and Google signed it. The launcher has no APK parser
// or signature verifier of its own, so it asks this, which uses the same
// checks the runtime trusts at run time: the package name as AppInstall reads
// it, the signature with ApkVerification (apksig, every APK of a bundle, v3
// rotation lineage included) and Google's certificate as GoogleGmsCertificate
// matches it, as the current signer or an ancestor in the verified lineage.
//
// Prints one `KEY<tab>value` line each, UTF-8:
//   PACKAGE   the manifest's package (empty when there is none)
//   VERIFIED  1 when the signature verifies, else 0
//   REASON    why not (only when VERIFIED is 0)
//   SCHEME    the signature scheme: 3, 2, 1 (JAR), 0 unverified
//   LINEAGE   the certificates in the rotation lineage (1 without one)
//   GOOGLE    1 when Google's certificate signed it, else 0
// Exit codes: 0 verified and signed by Google, 4 verified with another key,
// 3 the signature does not verify, 1 no package name or unreadable, 2 usage.

package com.winapk.tools;

import com.macapk.gms.GoogleGmsCertificate;
import com.macapk.loader.ApkVerification;

import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

public final class ApkCheck {
    private ApkCheck() {}

    public static int run(String[] args) {
        PrintStream out = new PrintStream(System.out, true, StandardCharsets.UTF_8);
        if (args.length != 1) {
            System.err.println("usage: winapk-runtime --verify-apk <app.apk|.apkm|.xapk|.apks>");
            return 2;
        }
        Path file = Paths.get(args[0]);
        String pkg;
        try {
            pkg = Files.isRegularFile(file) ? AppInstall.packageOf(file) : null;
        } catch (Throwable t) {
            System.err.println("[verify] " + t);
            pkg = null;
        }
        out.println("PACKAGE\t" + (pkg == null ? "" : pkg));
        if (pkg == null) {
            out.println("VERIFIED\t0");
            out.println("REASON\tnot an Android app, or unreadable");
            out.println("GOOGLE\t0");
            return 1;
        }
        ApkVerification.Result r = ApkVerification.of(file.toAbsolutePath().toString());
        boolean google = GoogleGmsCertificate.isGoogleSigner(r);
        out.println("VERIFIED\t" + (r.verified ? 1 : 0));
        if (!r.verified) out.println("REASON\t" + oneLine(r.reason));
        out.println("SCHEME\t" + r.scheme);
        out.println("LINEAGE\t" + (r.past == null ? 1 : r.past.size()));
        out.println("GOOGLE\t" + (google ? 1 : 0));
        System.err.println("[verify] " + file + ": " + pkg + (r.verified ? " verified" : " NOT verified ("
                + r.reason + ")") + (google ? ", signed by Google" : ""));
        return !r.verified ? 3 : google ? 0 : 4;
    }

    private static String oneLine(String s) {
        return s == null ? "" : s.replace('\r', ' ').replace('\n', ' ').replace('\t', ' ');
    }

    public static void main(String[] args) {
        System.exit(run(args));
    }
}
