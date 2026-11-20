package com.winapk.aospviews;

import android.graphics.Paint;
import android.graphics.Rect;
import android.util.Size;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.TextView;

import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.WeakHashMap;
import java.util.concurrent.Executor;
import java.util.function.Consumer;

/**
 * Android 15's (API 35) members of the classes lib/aosp-views.jar takes from AOSP 14 (API 34), which
 * the runtime reports as its SDK level, so an app built for 35 calls them: Compose's
 * {@code View.setRequestedFrameRate} (behind SDK_INT >= 35) threw NoSuchMethodError in Jerboa and
 * Jellyfin. build_aosp_views.sh adds each public static method here as an instance method of its
 * first parameter's class (windows/scripts/AospAddMethods.java, as aosp-graphics.jar's Api35 does).
 *
 * Each has AOSP 15's answer where it is a value the object keeps (a setter and its getter, with 15's
 * default); where 15 acts on it, the action is a hint this runtime has no part for and AOSP 14's
 * behaviour stands: the frame-rate votes and boosts (one fixed-rate display, no SurfaceFlinger), HDR
 * headroom, the text layout options (as aosp-graphics.jar's StaticLayout.Builder ones), stylus
 * handwriting (no handwriting IME), and bounding rects of insets (no cutout or bar shapes; the bars
 * are whole edges). The values live beside the object, weakly, as the classes have no field for them.
 * The left-out 35 members name types AOSP 14 has no class of (credentials, InputTransferToken,
 * ConnectionlessHandwritingCallback) or are WindowManager's, an interface (default methods would be
 * needed); LinkScan lists them.
 */
public final class Api35 {
    private Api35() {}

    private static final Map<Object, Object[]> sState = Collections.synchronizedMap(new WeakHashMap<>());

    private static Object get(Object o, int slot, Object dflt) {
        Object[] s = sState.get(o);
        return s == null || s[slot] == null ? dflt : s[slot];
    }

    private static void put(Object o, int slot, Object v) {
        synchronized (sState) {
            Object[] s = sState.get(o);
            if (s == null) sState.put(o, s = new Object[8]);
            s[slot] = v;
        }
    }

    // ---- View ----

    /** View.REQUESTED_FRAME_RATE_CATEGORY_DEFAULT (15): no vote. */
    private static final float FRAME_RATE_DEFAULT = Float.NaN;

    public static void setRequestedFrameRate(View v, float frameRate) { put(v, 0, frameRate); }
    public static float getRequestedFrameRate(View v) { return (Float) get(v, 0, FRAME_RATE_DEFAULT); }

    /** 15 keeps the absolute velocity; a view that never set one answers 0. */
    public static void setFrameContentVelocity(View v, float pixelsPerSecond) { put(v, 1, Math.abs(pixelsPerSecond)); }
    public static float getFrameContentVelocity(View v) { return Math.max((Float) get(v, 1, 0f), 0f); }

    /** View.CONTENT_SENSITIVITY_AUTO (0) by default; SENSITIVE is 1. */
    public static void setContentSensitivity(View v, int mode) { put(v, 2, mode); }
    public static int getContentSensitivity(View v) { return (Integer) get(v, 2, 0); }
    /** 15: sensitive when marked so; with AUTO, a password field (its input type) is too. */
    public static boolean isContentSensitive(View v) {
        int mode = getContentSensitivity(v);
        if (mode == 1) return true;
        if (mode != 0) return false;
        if (v instanceof TextView t) {
            int type = t.getInputType();
            int variation = type & android.text.InputType.TYPE_MASK_VARIATION;
            int cls = type & android.text.InputType.TYPE_MASK_CLASS;
            return (cls == android.text.InputType.TYPE_CLASS_TEXT
                    && (variation == android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD
                        || variation == android.text.InputType.TYPE_TEXT_VARIATION_WEB_PASSWORD
                        || variation == android.text.InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD))
                    || (cls == android.text.InputType.TYPE_CLASS_NUMBER
                        && variation == android.text.InputType.TYPE_NUMBER_VARIATION_PASSWORD);
        }
        return false;
    }

    public static void setHandwritingDelegateFlags(View v, int flags) { put(v, 3, flags); }
    public static int getHandwritingDelegateFlags(View v) { return (Integer) get(v, 3, 0); }

    // ---- Window and its LayoutParams: frame-rate and HDR hints ----

    public static void setFrameRateBoostOnTouchEnabled(WindowManager.LayoutParams p, boolean enabled) {
        put(p, 0, enabled);
    }
    public static boolean getFrameRateBoostOnTouchEnabled(WindowManager.LayoutParams p) {
        return (Boolean) get(p, 0, true);
    }
    public static void setFrameRatePowerSavingsBalanced(WindowManager.LayoutParams p, boolean balanced) {
        put(p, 1, balanced);
    }
    public static boolean isFrameRatePowerSavingsBalanced(WindowManager.LayoutParams p) {
        return (Boolean) get(p, 1, true);
    }
    public static void setDesiredHdrHeadroom(WindowManager.LayoutParams p, float headroom) { put(p, 2, headroom); }
    public static float getDesiredHdrHeadroom(WindowManager.LayoutParams p) { return (Float) get(p, 2, 0f); }

    /** 15's Window forwards each to its attributes. */
    public static void setFrameRateBoostOnTouchEnabled(Window w, boolean enabled) {
        setFrameRateBoostOnTouchEnabled(w.getAttributes(), enabled);
    }
    public static boolean getFrameRateBoostOnTouchEnabled(Window w) {
        return getFrameRateBoostOnTouchEnabled(w.getAttributes());
    }
    public static void setFrameRatePowerSavingsBalanced(Window w, boolean balanced) {
        setFrameRatePowerSavingsBalanced(w.getAttributes(), balanced);
    }
    public static boolean isFrameRatePowerSavingsBalanced(Window w) {
        return isFrameRatePowerSavingsBalanced(w.getAttributes());
    }
    public static void setDesiredHdrHeadroom(Window w, float headroom) { setDesiredHdrHeadroom(w.getAttributes(), headroom); }
    public static float getDesiredHdrHeadroom(Window w) { return getDesiredHdrHeadroom(w.getAttributes()); }

    // ---- WindowInsets: bounding rects and the frame ----

    public static List<Rect> getBoundingRects(WindowInsets i, int typeMask) { return Collections.emptyList(); }
    public static List<Rect> getBoundingRectsIgnoringVisibility(WindowInsets i, int typeMask) {
        return Collections.emptyList();
    }
    /** The frame the insets are relative to: 0x0, as 15 answers for insets built without one (14's
     *  InsetsState does not hand it over). */
    public static Size getFrame(WindowInsets i) { return new Size(0, 0); }

    public static WindowInsets.Builder setBoundingRects(WindowInsets.Builder b, int typeMask, List<Rect> rects) {
        return b;
    }
    public static WindowInsets.Builder setBoundingRectsIgnoringVisibility(WindowInsets.Builder b, int typeMask,
            List<Rect> rects) {
        return b;
    }
    public static WindowInsets.Builder setFrame(WindowInsets.Builder b, int width, int height) { return b; }

    // ---- TextView: 15's layout options, kept and answered; the layout is AOSP 14's ----

    public static void setUseBoundsForWidth(TextView t, boolean use) { put(t, 4, use); }
    public static boolean getUseBoundsForWidth(TextView t) { return (Boolean) get(t, 4, false); }
    public static void setShiftDrawingOffsetForStartOverhang(TextView t, boolean shift) { put(t, 5, shift); }
    public static boolean getShiftDrawingOffsetForStartOverhang(TextView t) { return (Boolean) get(t, 5, false); }
    public static void setMinimumFontMetrics(TextView t, Paint.FontMetrics m) { put(t, 6, m); }
    public static Paint.FontMetrics getMinimumFontMetrics(TextView t) { return (Paint.FontMetrics) get(t, 6, null); }
    public static void setLocalePreferredLineHeightForMinimumUsed(TextView t, boolean used) { put(t, 7, used); }
    public static boolean isLocalePreferredLineHeightForMinimumUsed(TextView t) { return (Boolean) get(t, 7, false); }

    // ---- Input methods and accessibility: stylus handwriting is not available here ----

    public static void setStylusHandwritingEnabled(EditorInfo e, boolean enabled) { put(e, 0, enabled); }
    public static boolean isStylusHandwritingEnabled(EditorInfo e) { return (Boolean) get(e, 0, false); }

    public static boolean supportsConnectionlessStylusHandwriting(InputMethodInfo i) { return false; }
    public static boolean isConnectionlessStylusHandwritingAvailable(InputMethodManager m) { return false; }

    /** No handwriting IME: the delegation is not accepted, as 15 answers when none is bound. */
    public static void acceptStylusHandwritingDelegation(InputMethodManager m, View delegate, String pkg,
            Executor executor, Consumer<Boolean> callback) {
        executor.execute(() -> callback.accept(false));
    }
    public static void acceptStylusHandwritingDelegation(InputMethodManager m, View delegate, String pkg, int flags,
            Executor executor, Consumer<Boolean> callback) {
        executor.execute(() -> callback.accept(false));
    }

    public static void setGranularScrollingSupported(AccessibilityNodeInfo n, boolean supported) { put(n, 0, supported); }
    public static boolean isGranularScrollingSupported(AccessibilityNodeInfo n) { return (Boolean) get(n, 0, false); }

    /** CollectionInfo.UNDEFINED (-1): 14's collections carry no item counts. */
    public static int getItemCount(AccessibilityNodeInfo.CollectionInfo c) { return -1; }
    public static int getImportantForAccessibilityItemCount(AccessibilityNodeInfo.CollectionInfo c) { return -1; }
}
