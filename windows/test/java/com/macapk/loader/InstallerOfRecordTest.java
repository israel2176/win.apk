// InstallerOfRecordTest -- the installer PackageManager reports (com.macapk.loader.InstallerOfRecord):
// Google Play for an app the user installed while Settings > Apps > "Report Google Play as the
// installer" is on (WINAPK_PLAY_INSTALLER, absent = on), null when it is off, and a real record
// (an app that installed another through PackageInstaller) over either.
//
//   java -cp android-framework.jar;macapk-loader.jar;<classes> com.macapk.loader.InstallerOfRecordTest <scratch dir>
//
// The environment, whether a Play Store package is reported and the Play ledger backfill are the
// class's test seams; the record is a file in the scratch dir. Checks: the setting absent/1/0; a
// record wins either way and its removal brings the default back; a package not installed, Play,
// Play services and GSF get no default; no Play Store package, no default;
// and MacApkPackageManager's two doors: getInstallerPackageName and getInstallSourceInfo (installing,
// initiating, originating, update owner, package source) agree, in the shape a Play install has on
// Android 14, and keep the plain shape for another store.
// Prints PASS/FAIL per check and "ALL PASSED" at the end; exits 1 on a failure.
package com.macapk.loader;

import android.content.pm.InstallSourceInfo;
import android.content.pm.PackageInstaller;
import java.io.File;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;

public class InstallerOfRecordTest {
    static int failures;

    static void check(String name, boolean ok) {
        System.out.println((ok ? "PASS " : "FAIL ") + name);
        if (!ok) failures++;
    }

    static void eq(String name, Object want, Object got) {
        check(name + " (want " + want + ", got " + got + ")", Objects.equals(want, got));
    }

    static final String PLAY = "com.android.vending";
    static final String APP = "com.bnhp.payments.paymentsapp";
    static final String FDROID = "org.fdroid.fdroid";
    static final Map<String, String> env = new HashMap<>();
    static boolean playPresent = true;

    public static void main(String[] args) throws Exception {
        File dir = new File(args.length > 0 ? args[0] : "installer-test");
        dir.mkdirs();
        File record = new File(dir, "installer-of-record.properties");
        record.delete();
        env.put("MACAPK_INSTALLER_RECORD_FILE", record.getPath());
        InstallerOfRecord.env = env::get;
        InstallerOfRecord.playPresent = () -> playPresent;
        InstallerOfRecord.backfill = false;

        // The setting: absent = on, "1" on, "0" off.
        eq("default on: an installed app names Play", PLAY, InstallerOfRecord.reported(APP, true));
        env.put("WINAPK_PLAY_INSTALLER", "1");
        eq("setting 1: Play", PLAY, InstallerOfRecord.reported(APP, true));
        env.put("WINAPK_PLAY_INSTALLER", "0");
        eq("setting 0: no installer", null, InstallerOfRecord.reported(APP, true));
        check("setting 0: playDefaultOn false", !InstallerOfRecord.playDefaultOn());

        // A real record wins either way, and its removal brings the default back.
        InstallerOfRecord.set(APP, FDROID);
        eq("record, setting 0: the recorded store", FDROID, InstallerOfRecord.reported(APP, true));
        env.remove("WINAPK_PLAY_INSTALLER");
        eq("record, setting on: the recorded store, not Play", FDROID, InstallerOfRecord.reported(APP, true));
        eq("record: get() is the record", FDROID, InstallerOfRecord.get(APP));
        InstallerOfRecord.set(APP, null);
        eq("record removed: Play again", PLAY, InstallerOfRecord.reported(APP, true));
        eq("no record: get() null", null, InstallerOfRecord.get(APP));

        // No default where a device has no store as installer.
        eq("not installed: null", null, InstallerOfRecord.reported("com.example.absent", false));
        eq("Play itself: null", null, InstallerOfRecord.reported(PLAY, true));
        eq("Play services: null", null, InstallerOfRecord.reported("com.google.android.gms", true));
        eq("GSF: null", null, InstallerOfRecord.reported("com.google.android.gsf", true));
        eq("null package: null", null, InstallerOfRecord.reported(null, true));
        playPresent = false;
        eq("no Play Store package reported: null", null, InstallerOfRecord.reported(APP, true));
        playPresent = true;

        // PackageManager: the running app is installed; both doors give one answer.
        MacApkPackageManager pm = new MacApkPackageManager(APP);
        eq("PM getInstallerPackageName(self)", PLAY, pm.getInstallerPackageName(APP));
        InstallSourceInfo info = pm.getInstallSourceInfo(APP);
        eq("PM installing", PLAY, info.getInstallingPackageName());
        eq("PM initiating", PLAY, info.getInitiatingPackageName());
        eq("PM originating", null, info.getOriginatingPackageName());
        eq("PM update owner", PLAY, info.getUpdateOwnerPackageName());
        eq("PM package source STORE", PackageInstaller.SessionParams.PACKAGE_SOURCE_STORE, info.getPackageSource());
        check("PM initiating signing info is Play's certificate", info.getInitiatingPackageSigningInfo() != null);
        eq("PM a package not installed: no installer", null, pm.getInstallerPackageName("com.example.absent"));
        eq("PM Play's own installer", null, pm.getInstallerPackageName(PLAY));

        env.put("WINAPK_PLAY_INSTALLER", "0");
        info = pm.getInstallSourceInfo(APP);
        eq("PM setting 0: getInstallerPackageName", null, pm.getInstallerPackageName(APP));
        eq("PM setting 0: installing", null, info.getInstallingPackageName());
        eq("PM setting 0: initiating", null, info.getInitiatingPackageName());
        eq("PM setting 0: update owner", null, info.getUpdateOwnerPackageName());

        InstallerOfRecord.set(APP, FDROID);
        info = pm.getInstallSourceInfo(APP);
        eq("PM record: getInstallerPackageName", FDROID, pm.getInstallerPackageName(APP));
        eq("PM record: installing", FDROID, info.getInstallingPackageName());
        eq("PM record: initiating", FDROID, info.getInitiatingPackageName());
        eq("PM record: update owner", null, info.getUpdateOwnerPackageName());
        InstallerOfRecord.set(APP, null);
        env.remove("WINAPK_PLAY_INSTALLER");

        if (failures == 0) {
            System.out.println("ALL PASSED");
        } else {
            System.out.println(failures + " FAILED");
            System.exit(1);
        }
    }
}
