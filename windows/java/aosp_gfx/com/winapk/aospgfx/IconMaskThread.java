package com.winapk.aospgfx;

import android.app.ActivityThread;

/**
 * AOSP's AdaptiveIconDrawable constructor reads the icon mask (config_icon_mask) from
 * {@code ActivityThread.currentActivityThread() == null ? Resources.getSystem()
 * : currentActivityThread().getApplication().getResources()}. On Android no icon is built before
 * bindApplication; Win.apk's start builds the app's icon for its window before the Application
 * exists, when Win.apk's ActivityThread is there but has no Application, and the constructor threw a
 * NullPointerException (Win.apk's inflater then built the icon). build_aosp_gfx.sh points that
 * class's currentActivityThread calls here (scripts/AospCallRedirect.java): with no Application yet
 * the answer is null, so the mask comes from Resources.getSystem(), AOSP's own other branch.
 */
public final class IconMaskThread {
    private IconMaskThread() {}

    public static ActivityThread currentActivityThread() {
        ActivityThread t = ActivityThread.currentActivityThread();
        return t != null && t.getApplication() != null ? t : null;
    }
}
