package com.winapk.aospgfx;

import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.text.LineBreakConfig;
import android.graphics.text.LineBreaker;
import android.text.DynamicLayout;
import android.text.Layout;
import android.text.StaticLayout;

/**
 * Android 15's (API 35) members of the classes lib/aosp-graphics.jar takes from AOSP 14 (API 34),
 * which the runtime reports as its SDK level: an app built for 35 calls them, and so does Win.apk's
 * own Layout.Builder (API 35). Without them the call threw NoSuchMethodError: Compose's text layout
 * (StaticLayout.Builder.setUseBoundsForWidth, behind SDK_INT >= 35) failed to measure, and Jellyfin's
 * window stayed blank. build_aosp_gfx.sh adds each public static method here as an instance method
 * of its first parameter's class, same name, the other parameters and return type
 * (scripts/AospAddMethods.java): {@code builder.setUseBoundsForWidth(b)} then runs
 * {@code Api35.setUseBoundsForWidth(builder, b)}.
 *
 * What these do is what AOSP 14 does where 15 adds an option: the layout ignores the glyph bounds
 * and the start overhang, and the minimum font metrics; a getter answers that default. Nothing here
 * draws or measures differently from the rest of the AOSP 14 text stack.
 */
public final class Api35 {
    private Api35() {}

    // ---- StaticLayout.Builder, DynamicLayout.Builder, LineBreaker.Builder: options of 15 ----

    public static StaticLayout.Builder setUseBoundsForWidth(StaticLayout.Builder b, boolean use) { return b; }

    public static StaticLayout.Builder setShiftDrawingOffsetForStartOverhang(StaticLayout.Builder b, boolean s) {
        return b;
    }

    public static StaticLayout.Builder setMinimumFontMetrics(StaticLayout.Builder b, Paint.FontMetrics m) {
        return b;
    }

    public static DynamicLayout.Builder setUseBoundsForWidth(DynamicLayout.Builder b, boolean use) { return b; }

    public static DynamicLayout.Builder setShiftDrawingOffsetForStartOverhang(DynamicLayout.Builder b, boolean s) {
        return b;
    }

    public static DynamicLayout.Builder setMinimumFontMetrics(DynamicLayout.Builder b, Paint.FontMetrics m) {
        return b;
    }

    /** AOSP 14's DynamicLayout breaks with the paint's own LineBreakConfig only. */
    public static DynamicLayout.Builder setLineBreakConfig(DynamicLayout.Builder b, LineBreakConfig c) {
        return b;
    }

    public static LineBreaker.Builder setUseBoundsForWidth(LineBreaker.Builder b, boolean use) { return b; }

    // ---- Layout: the getters of those options, and two of values AOSP 14 already keeps ----

    public static boolean getUseBoundsForWidth(Layout l) { return false; }

    public static boolean getShiftDrawingOffsetForStartOverhang(Layout l) { return false; }

    public static Paint.FontMetrics getMinimumFontMetrics(Layout l) { return null; }

    public static float getLineSpacingAmount(Layout l) { return l.getSpacingAdd(); }

    public static float getLineSpacingMultiplier(Layout l) { return l.getSpacingMultiplier(); }

    // ---- Paint, Path ----

    /** 15 adds the fallback fonts of the paint's locale to the metrics; 14 knows the primary's. */
    public static void getFontMetricsForLocale(Paint p, Paint.FontMetrics out) { p.getFontMetrics(out); }

    public static void getFontMetricsIntForLocale(Paint p, Paint.FontMetricsInt out) { p.getFontMetricsInt(out); }

    public static void set(Paint.FontMetricsInt to, Paint.FontMetricsInt from) {
        to.top = from.top;
        to.ascent = from.ascent;
        to.descent = from.descent;
        to.bottom = from.bottom;
        to.leading = from.leading;
    }

    /** Rounded as 15 does: top floored and bottom ceiled (the clipping bounds), the rest rounded. */
    public static void set(Paint.FontMetricsInt to, Paint.FontMetrics from) {
        to.top = (int) Math.floor(from.top);
        to.ascent = Math.round(from.ascent);
        to.descent = Math.round(from.descent);
        to.bottom = (int) Math.ceil(from.bottom);
        to.leading = Math.round(from.leading);
    }

    /** 15's one-argument form is the exact bounds, as computeBounds(bounds, true) is in 14. */
    public static void computeBounds(Path p, RectF bounds) { p.computeBounds(bounds, true); }
}
