// SessionUpdateTest -- an update through PackageInstaller's session API, as the Play Store
// installs one (com.macapk.loader.PackageInstallerBackend), and AOSP's update rules.
//
//   java -cp android-framework.jar;macapk-loader.jar;<classes> com.macapk.loader.SessionUpdateTest <scratch dir>
//
// The rules (PackageInstallerBackend.checkUpdate, no PackageManager needed): the same signer
// updates; a different one is refused INSTALL_FAILED_UPDATE_INCOMPATIBLE / STATUS_FAILURE_CONFLICT;
// a new key whose v3 lineage names the installed one updates (key rotation), and so does an
// update signed by a key the installed app rotated away from; a lower versionCode is refused
// INSTALL_FAILED_VERSION_DOWNGRADE / STATUS_FAILURE_INCOMPATIBLE with AOSP's message, the same
// one is a reinstall; unknown on either side refuses nothing. InstallConstraints.GENTLE_UPDATE
// is AOSP's ("app not interacting").
//
// The session, in a child JVM whose environment points MACAPK_INSTALLHELPER at a stand-in for
// the launcher's install helper (this class again, "helper"), MACAPK_STAGING_DIR and the
// installer record into the scratch folder: createSession; openWrite of a base APK, a config
// split and Play's dex metadata; commit with an IntentSender. The helper is handed one bundle
// holding base.apk and the split, byte for byte, without the .dm; its JSON answer becomes
// STATUS_SUCCESS with the package it names and INSTALL_SUCCEEDED; the staged bundle is gone
// afterwards. A declined install (helper exit 3) is STATUS_FAILURE_ABORTED, a failed one
// (exit 5) STATUS_FAILURE with the helper's reason; a helper that prints "[helper] exit <code>" and
// stays up (Win.apk's install window left open on "App installed.") has its status sent at that line,
// not when it exits; an empty session STATUS_FAILURE_INVALID
// without asking the helper; a sealed session refuses a second write.
// Prints PASS/FAIL per check and "ALL PASSED" at the end; exits 1 on a failure.
package com.macapk.loader;

import android.content.Context;
import android.content.Intent;
import android.content.IntentSender;
import android.content.pm.PackageInstaller;
import android.content.pm.Signature;

import java.io.File;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class SessionUpdateTest {
    static int failures;

    static void check(String name, boolean ok) {
        System.out.println((ok ? "PASS " : "FAIL ") + name);
        if (!ok) failures++;
    }

    static void eq(String name, Object want, Object got) {
        check(name + " (want " + want + ", got " + got + ")", Objects.equals(want, got));
    }

    static final String APP = "com.bnhp.payments.paymentsapp";
    static final boolean WINDOWS = File.separatorChar == '\\';

    public static void main(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("helper")) System.exit(helper(args));
        if (args.length > 0 && args[0].equals("session")) {
            session(new File(args[1]));
        } else {
            rules();
            spawnSession(new File(args.length > 0 ? args[0] : "session-update-test"));
        }
        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }

    // ---- the rules -------------------------------------------------------------------

    static Signature[] sig(String... hex) {
        Signature[] out = new Signature[hex.length];
        for (int i = 0; i < hex.length; i++) out[i] = new Signature(hex[i]);
        return out;
    }

    static void rules() {
        Signature[] oldKey = sig("0a0b0c"), newKey = sig("1a1b1c"), other = sig("2a2b2c");
        Signature[] lineage = sig("0a0b0c", "1a1b1c");   // old rotated to new, oldest first
        eq("the same signer, a newer version: an update", null,
                PackageInstallerBackend.checkUpdate(APP, 100, oldKey, oldKey, 101, oldKey, oldKey));
        PackageInstallerBackend.UpdateRefusal r =
                PackageInstallerBackend.checkUpdate(APP, 100, oldKey, oldKey, 101, other, other);
        check("another signer: refused", r != null);
        if (r != null) {
            eq("  STATUS_FAILURE_CONFLICT", PackageInstaller.STATUS_FAILURE_CONFLICT, r.status);
            eq("  INSTALL_FAILED_UPDATE_INCOMPATIBLE", -7, r.legacyStatus);
            check("  AOSP's message", r.message.startsWith("INSTALL_FAILED_UPDATE_INCOMPATIBLE: Package " + APP
                    + " signatures do not match newer version"));
        }
        eq("a rotated key whose lineage names the installed one: an update", null,
                PackageInstallerBackend.checkUpdate(APP, 100, oldKey, oldKey, 101, newKey, lineage));
        eq("signed by a key the installed app rotated away from: an update", null,
                PackageInstallerBackend.checkUpdate(APP, 100, newKey, lineage, 101, oldKey, oldKey));
        r = PackageInstallerBackend.checkUpdate(APP, 100, oldKey, oldKey, 99, oldKey, oldKey);
        check("an older versionCode: refused", r != null);
        if (r != null) {
            eq("  STATUS_FAILURE_INCOMPATIBLE", PackageInstaller.STATUS_FAILURE_INCOMPATIBLE, r.status);
            eq("  INSTALL_FAILED_VERSION_DOWNGRADE", -25, r.legacyStatus);
            eq("  AOSP's message", "INSTALL_FAILED_VERSION_DOWNGRADE: Downgrade detected: Update version code 99"
                    + " is older than current 100", r.message);
        }
        eq("the same versionCode: a reinstall", null,
                PackageInstallerBackend.checkUpdate(APP, 100, oldKey, oldKey, 100, oldKey, oldKey));
        eq("unknown versions and signers refuse nothing", null,
                PackageInstallerBackend.checkUpdate(APP, 0, null, null, 0, other, other));
        check("a downgrade is refused before the signers are compared",
                PackageInstallerBackend.checkUpdate(APP, 100, oldKey, oldKey, 1, other, other).legacyStatus == -25);
        PackageInstaller.InstallConstraints g = PackageInstaller.InstallConstraints.GENTLE_UPDATE;
        check("GENTLE_UPDATE is AOSP's: only \"app not interacting\"", g.isAppNotInteractingRequired()
                && !g.isAppNotForegroundRequired() && !g.isAppNotTopVisibleRequired() && !g.isDeviceIdleRequired()
                && !g.isNotInCallRequired());
    }

    // ---- the session -----------------------------------------------------------------

    static void spawnSession(File dir) throws Exception {
        dir.mkdirs();
        String java = ProcessHandle.current().info().command().orElseThrow();
        String cp = System.getProperty("java.class.path");
        File helper;
        if (WINDOWS) {
            helper = new File(dir, "helper.cmd");
            Files.writeString(helper.toPath(), "@\"" + java + "\" -cp \"" + cp + "\" " + SessionUpdateTest.class.getName()
                    + " helper \"" + dir.getAbsolutePath() + "\" %*\r\n");
        } else {
            helper = new File(dir, "helper.sh");
            Files.writeString(helper.toPath(), "#!/bin/sh\nexec '" + java + "' -cp '" + cp + "' "
                    + SessionUpdateTest.class.getName() + " helper '" + dir.getAbsolutePath() + "' \"$@\"\n");
            helper.setExecutable(true);
        }
        ProcessBuilder pb = new ProcessBuilder(java, "-cp", cp, SessionUpdateTest.class.getName(), "session",
                dir.getAbsolutePath()).inheritIO();
        pb.environment().put("MACAPK_INSTALLHELPER", helper.getAbsolutePath());
        pb.environment().put("MACAPK_STAGING_DIR", new File(dir, "staging").getAbsolutePath());
        pb.environment().put("MACAPK_INSTALLER_RECORD_FILE", new File(dir, "installer-of-record.properties")
                .getAbsolutePath());
        Process p = pb.start();
        boolean done = p.waitFor(300, TimeUnit.SECONDS);
        if (!done) p.destroyForcibly();
        check("the session checks (above) passed", done && p.exitValue() == 0);
    }

    /** The status the session delivers through its IntentSender. */
    static final class Status extends IntentSender {
        final CountDownLatch got = new CountDownLatch(1);
        volatile Intent intent;
        @Override public void sendIntent(Context context, int code, Intent i, OnFinished f, android.os.Handler h) {
            intent = i;
            got.countDown();
        }
        Intent await() throws InterruptedException {
            got.await(120, TimeUnit.SECONDS);
            return intent;
        }
    }

    static void write(PackageInstallerBackend.SessionState s, String name, byte[] bytes) throws Exception {
        try (OutputStream out = s.openWrite(name, 0, bytes.length)) {
            out.write(bytes);
            s.fsync(out);
        }
    }

    static int status(Intent i) { return i == null ? -999 : i.getIntExtra(PackageInstaller.EXTRA_STATUS, -999); }

    static void session(File dir) throws Exception {
        File next = new File(dir, "next-exit"), saw = new File(dir, "helper-saw.txt");
        byte[] base = "BASE-APK-BYTES".getBytes(StandardCharsets.UTF_8);
        byte[] split = "SPLIT-APK-BYTES".getBytes(StandardCharsets.UTF_8);

        // An update with its config split, and the dex metadata Play writes beside it.
        Files.writeString(next.toPath(), "0");
        saw.delete();
        PackageInstallerBackend.SessionState s = PackageInstallerBackend.create(APP, base.length + split.length, "bit");
        write(s, "base.apk", base);
        write(s, "split_config.xxhdpi.apk", split);
        write(s, "base.dm", "DEX-METADATA".getBytes(StandardCharsets.UTF_8));
        Status st = new Status();
        s.commit(st);
        boolean sealed = false;
        try { s.openWrite("late.apk", 0, 1); } catch (SecurityException e) { sealed = true; }
        check("a committed session takes no more writes", sealed);
        Intent i = st.await();
        eq("the helper's success is STATUS_SUCCESS", PackageInstaller.STATUS_SUCCESS, status(i));
        eq("with the package the helper installed", APP,
                i == null ? null : i.getStringExtra(PackageInstaller.EXTRA_PACKAGE_NAME));
        eq("and INSTALL_SUCCEEDED", 1, i == null ? null : i.getIntExtra(PackageInstaller.EXTRA_LEGACY_STATUS, 0));
        eq("for this session", s.id, i == null ? null : i.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1));
        String seen = saw.isFile() ? Files.readString(saw.toPath()) : "";
        check("the helper got one bundle: base.apk and the split, byte for byte, without the .dm",
                seen.contains("base.apk=BASE-APK-BYTES") && seen.contains("=SPLIT-APK-BYTES")
                        && !seen.contains("DEX-METADATA") && seen.contains("--source android"));
        File staging = new File(dir, "staging");
        File[] left = staging.listFiles((d, n) -> n.startsWith("guest-session-"));
        check("the staged bundle is removed after the install", left != null && left.length == 0);

        // The user declined (the helper's exit 3).
        Files.writeString(next.toPath(), "3");
        s = PackageInstallerBackend.create(APP, base.length, "bit");
        write(s, "base.apk", base);
        st = new Status();
        s.commit(st);
        eq("declined: STATUS_FAILURE_ABORTED", PackageInstaller.STATUS_FAILURE_ABORTED, status(st.await()));

        // The install failed (exit 5), with the helper's reason.
        Files.writeString(next.toPath(), "5");
        s = PackageInstallerBackend.create(APP, base.length, "bit");
        write(s, "base.apk", base);
        st = new Status();
        s.commit(st);
        i = st.await();
        eq("failed: STATUS_FAILURE", PackageInstaller.STATUS_FAILURE, status(i));
        String msg = i == null ? "" : String.valueOf(i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
        check("with the helper's reason: " + msg, msg.contains("the app is running and could not be stopped"));

        // Win.apk's helper shows the install's end ("App installed." with Open and Done) and stays up until
        // the user closes it; its "[helper] exit 0" line, printed when the install ends, is when the session's
        // status goes out, as a device sends it when the install ends, not when its dialog is dismissed.
        Files.writeString(next.toPath(), "0-linger");
        s = PackageInstallerBackend.create(APP, base.length, "bit");
        write(s, "base.apk", base);
        st = new Status();
        long t0 = System.nanoTime();
        s.commit(st);
        i = st.await();
        long ms = (System.nanoTime() - t0) / 1_000_000;
        eq("an install whose window stays: STATUS_SUCCESS", PackageInstaller.STATUS_SUCCESS, status(i));
        check("sent when the helper says the install ended, not when its window closes (" + ms + " ms)",
                ms < LINGER_MS - 5_000);
        Files.writeString(next.toPath(), "5-linger");
        s = PackageInstallerBackend.create(APP, base.length, "bit");
        write(s, "base.apk", base);
        st = new Status();
        t0 = System.nanoTime();
        s.commit(st);
        i = st.await();
        ms = (System.nanoTime() - t0) / 1_000_000;
        eq("a failure whose window stays: STATUS_FAILURE", PackageInstaller.STATUS_FAILURE, status(i));
        check("also sent at once (" + ms + " ms)", ms < LINGER_MS - 5_000);
        eq("the exit line's code", Integer.valueOf(3), PackageInstallerBackend.helperExitLine("[helper] exit 3"));
        eq("not an exit line", null, PackageInstallerBackend.helperExitLine("[helper] progress 0.50 extracted"));

        // Nothing written: refused before the helper is asked.
        saw.delete();
        s = PackageInstallerBackend.create(APP, 0, "bit");
        st = new Status();
        s.commit(st);
        eq("an empty session: STATUS_FAILURE_INVALID", PackageInstaller.STATUS_FAILURE_INVALID, status(st.await()));
        check("and the helper was not asked", !saw.isFile());
    }

    // ---- the stand-in for Win.apk.exe install ----------------------------------------

    static final long LINGER_MS = 30_000;

    static int helper(String[] args) throws Exception {
        File dir = new File(args[1]);
        String next = Files.readString(new File(dir, "next-exit").toPath()).trim();
        boolean linger = next.endsWith("-linger");
        int exit = Integer.parseInt(linger ? next.substring(0, next.indexOf('-')) : next);
        List<String> rest = new ArrayList<>();
        for (int k = 2; k < args.length; k++) rest.add(args[k]);
        StringBuilder sb = new StringBuilder(String.join(" ", rest)).append('\n');
        int at = rest.indexOf("--apk");
        File apk = at >= 0 ? new File(rest.get(at + 1)) : null;
        if (apk != null && apk.getName().endsWith(".apkm")) {
            try (ZipFile z = new ZipFile(apk)) {
                for (java.util.Enumeration<? extends ZipEntry> e = z.entries(); e.hasMoreElements();) {
                    ZipEntry en = e.nextElement();
                    sb.append(en.getName()).append('=')
                            .append(new String(z.getInputStream(en).readAllBytes(), StandardCharsets.UTF_8))
                            .append('\n');
                }
            }
        } else if (apk != null) {
            sb.append("single=").append(Files.readString(apk.toPath())).append('\n');
        }
        Files.writeString(new File(dir, "helper-saw.txt").toPath(), sb.toString());
        System.out.println("[helper] progress 0.50 extracted");
        if (exit == 0) {
            System.out.println("{\"ok\":true,\"package\":\"" + APP + "\",\"wasUpdate\":\"true\"}");
        } else if (exit == 3) {
            System.out.println("{\"ok\":false,\"stage\":\"consent\",\"message\":\"declined\"}");
        } else {
            System.out.println("{\"ok\":false,\"stage\":\"install\",\"message\":\"the app is running and could not"
                    + " be stopped\"}");
        }
        if (linger) {  // the install window, left open on its result
            System.out.println("[helper] exit " + exit);
            System.out.flush();
            Thread.sleep(LINGER_MS);
        }
        System.out.flush();
        return exit;
    }
}
