package com.winapk.aospapp;

import android.app.Activity;
import android.app.ActivityOptions;
import android.app.PictureInPictureUiState;
import android.app.Service;

import java.util.Collections;
import java.util.Map;
import java.util.WeakHashMap;

/**
 * Android 15's and 16's (API 35, 36) members of the classes lib/aosp-app.jar takes from AOSP 14, which
 * an app targeting them is told it runs on (PresentedSdk) and so calls: androidx.browser's
 * CustomTabsIntent.Builder.build() calls ActivityOptions.setAllowPassThroughOnTouchOutside whenever
 * SDK_INT >= 36, and Jerboa died opening a link (docs/research/api-gaps.md). build_aosp_app.sh adds each
 * public static method here as an instance method of its first parameter's class
 * (windows/scripts/AospAddMethods.java), as build_aosp_views.sh does with com.winapk.aospviews.Api35/36.
 *
 * A value the object keeps is AOSP's (setter, getter, the later level's default), kept beside it,
 * weakly. The device id overloads of the permission calls are AOSP 15's own defaults: the default
 * device's call. Left out: the members naming ComponentCaller (35), a class AOSP 14 has none of.
 */
public final class Api36 {
    private Api36() {}

    private static final Map<Object, Object[]> sState = Collections.synchronizedMap(new WeakHashMap<>());

    private static Object get(Object o, int slot, Object dflt) {
        Object[] s = sState.get(o);
        return s == null || s[slot] == null ? dflt : s[slot];
    }

    private static void put(Object o, int slot, Object v) {
        synchronized (sState) {
            Object[] s = sState.get(o);
            if (s == null) sState.put(o, s = new Object[4]);
            s[slot] = v;
        }
    }

    // ---- ActivityOptions (36) ----

    /** Whether touches outside a translucent activity's window reach the one under it: kept, as
     *  Win.apk's one task has no activity of another app under a partial Custom Tab. */
    public static void setAllowPassThroughOnTouchOutside(ActivityOptions o, boolean allow) { put(o, 0, allow); }
    public static boolean isAllowPassThroughOnTouchOutside(ActivityOptions o) { return (Boolean) get(o, 0, false); }

    // ---- PictureInPictureUiState (35) ----

    /** No picture-in-picture here: never on the way to it. */
    public static boolean isTransitioningToPip(PictureInPictureUiState s) { return false; }

    // ---- Service (35) ----

    /** AOSP 15's default onTimeout(startId, fgsType) does nothing; an app's override is called by
     *  the system when a foreground service of a timed type runs out, which Win.apk does not time. */
    public static void onTimeout(Service s, int startId, int fgsType) {}

    // ---- Activity (35, 36) ----

    /** AOSP 15: the permission request for a virtual device; the default device's here. */
    public static void requestPermissions(Activity a, String[] permissions, int requestCode, int deviceId) {
        a.requestPermissions(permissions, requestCode);
    }

    public static boolean shouldShowRequestPermissionRationale(Activity a, String permission, int deviceId) {
        return a.shouldShowRequestPermissionRationale(permission);
    }

    /** AOSP 15's default: the three-argument callback. */
    public static void onRequestPermissionsResult(Activity a, int requestCode, String[] permissions,
            int[] grantResults, int deviceId) {
        a.onRequestPermissionsResult(requestCode, permissions, grantResults);
    }

    /** AOSP 16 asks the system to show the "open in browser" education for a web activity: no such
     *  system UI here, as on a device whose launcher shows none. */
    public static void requestOpenInBrowserEducation(Activity a) {}
}
