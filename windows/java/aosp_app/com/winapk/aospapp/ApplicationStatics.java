// What Win.apk's own framework asks of android.app.Application statically (Application.macapk_get,
// getInstance, the app's target SDK; 80 call sites in 40 files, counted in docs/research/aosp-app.md)
// that AOSP's Application does not have. AospAppRewrite.java adds each public static method here to
// AOSP's Application as a public static method of the same name and descriptor that calls this one, so
// Win.apk's classes link against AOSP's Application unchanged. The answers come from AOSP's own state:
// the process's Application is ActivityThread's (currentApplication), its target SDK its
// ApplicationInfo's. Step A0 moves these sites behind a seam and these forwards go.
package com.winapk.aospapp;

import android.app.ActivityThread;
import android.app.Application;
import android.content.pm.ApplicationInfo;

public final class ApplicationStatics {
    private ApplicationStatics() {}

    /** Win.apk's Application.macapk_get(): the process's Application. */
    public static Application macapk_get() { return ActivityThread.currentApplication(); }

    /** Win.apk's Application.getInstance(): the same. */
    public static Application getInstance() { return ActivityThread.currentApplication(); }

    /** Win.apk's Application.macapkAppTargetSdk(): the bound app's targetSdkVersion, 0 before. */
    public static int macapkAppTargetSdk() {
        AospApp app = AospApp.get();
        ApplicationInfo ai = app != null ? app.info : null;
        return ai != null && ai.targetSdkVersion > 0 ? ai.targetSdkVersion : 0;
    }

    /** Win.apk's Application.macapkTargetSdkKnown(int): whether `targetSdkVersion` is the app's real
     *  one. Once AOSP's ActivityThread has the app bound, the ApplicationInfo it was bound with is
     *  known; a 0 there is the manifest's own. */
    public static boolean macapkTargetSdkKnown(int targetSdkVersion) {
        return targetSdkVersion > 0 || targetSdkVersion == 0 && AospApp.get() != null;
    }
}
