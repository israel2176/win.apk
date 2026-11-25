// IPackageManager in this process ("package"), for what AOSP's ActivityThread and LoadedApk ask
// PackageManagerService directly (ActivityThread.getPackageManager(), not a Context's
// PackageManager, which stays Win.apk's MacApkPackageManager): the packages of a uid and the app's own
// ApplicationInfo. Methods with no answer here return their type's zero
// (com.winapk.aospapp.gen.IPackageManagerImpl logs each once).
package com.winapk.aospapp;

import android.content.pm.ApplicationInfo;
import android.os.Process;

public final class PackageService {
    private final AospApp app;

    PackageService(AospApp app) { this.app = app; }

    /** The app's uid has the app's package (one package per uid here, no shared uids). */
    public String[] getPackagesForUid(int uid) {
        return uid == Process.myUid() || uid == app.info.uid ? new String[] {app.info.packageName} : null;
    }

    public ApplicationInfo getApplicationInfo(String packageName, long flags, int userId) {
        return app.info.packageName.equals(packageName) ? new ApplicationInfo(app.info) : null;
    }
}
