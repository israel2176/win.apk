// The process's Application and the app's target SDK, for Win.apk's framework. AOSP's ActivityThread
// (lib/aosp-app.jar) is what knows them: its Application is the one handleBindApplication made, its
// ApplicationInfo the one the app was bound with. A JVM without the app layer, or before the bind,
// has none.
package com.macapk.loader.app;

import android.app.Application;
import android.content.pm.ApplicationInfo;

public final class Apps {
    private Apps() {}

    /** ActivityThread.currentApplication(): the process's Application, or null before the bind. */
    public static Application current() {
        try {
            return android.app.ActivityThread.currentApplication();
        } catch (LinkageError e) {
            return null;   // no app layer in this JVM
        }
    }

    /** The bound app's targetSdkVersion, 0 before the bind. */
    public static int targetSdk() {
        Application app = current();
        ApplicationInfo ai = null;
        try {
            if (app != null) ai = app.getApplicationInfo();
        } catch (Throwable ignored) {
            // an Application whose base context is not attached yet
        }
        return ai != null && ai.targetSdkVersion > 0 ? ai.targetSdkVersion : 0;
    }

    /** Whether `targetSdkVersion` is the app's real one: any positive value, or a 0 once the app is
     *  bound (then the manifest's own). Every compat gate that would test `targetSdkVersion > 0`
     *  tests this, so a value read before the bind is not taken for an old app's. */
    public static boolean targetSdkKnown(int targetSdkVersion) {
        return targetSdkVersion > 0 || targetSdkVersion == 0 && current() != null;
    }
}
