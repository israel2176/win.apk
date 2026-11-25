// The system bars as the in-process window manager reports them (docs/research/aosp-views.md, "V1
// as built"): the InsetsState sources WindowManagerService's InsetsStateController gives every
// window, so AOSP's InsetsController, ViewRootImpl, DecorView and the app's own
// fitsSystemWindows / WindowInsets code compute what they do on a phone.
//
// Which bars exist is Win.apk's decision, and stays so: none (the owner's, Win.apk's
// StatusBarDriver.systemBarsEnabled; a Windows app has no phone status bar). Their sizes are AOSP's own: the status bar
// SystemBarUtils.getStatusBarHeight (R.dimen.status_bar_height_default, no cutout), the
// three-button navigation bar R.dimen.navigation_bar_height at the bottom, navigation_bar_width
// on the side where Win.apk puts it (StatusBarDriver.navBarPosition). Whether each is shown is
// the window that controls them's request (InsetsPolicy: the top full-screen app window's
// requested visible types; hide() and FLAG_FULLSCREEN come that way), and the glyphs' shade its
// appearance (APPEARANCE_LIGHT_STATUS_BARS / _NAVIGATION_BARS). The bars' background is the
// window's own: AOSP's DecorView draws it; Win.apk paints only the glyphs on top
// (StatusBarDriver.paintStatusGlyphs / paintNavGlyphs), as SystemUI does.
package com.winapk.aospviews;

import android.content.res.Resources;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.view.InsetsSource;
import android.view.InsetsState;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import com.macapk.loader.StatusBarDriver;

final class Bars {
    private Bars() {}

    static final int ID_STATUS = InsetsSource.createId(null, 0, WindowInsets.Type.statusBars());
    static final int ID_NAV = InsetsSource.createId(null, 0, WindowInsets.Type.navigationBars());

    /** What update() last computed. */
    private static volatile boolean sEnabled, sStatusVisible = true, sNavVisible = true;
    private static volatile int sAppearance;
    private static volatile int sStatusH, sNavH, sNavPos = StatusBarDriver.NAV_BAR_BOTTOM;
    private static volatile int sW, sH;

    static boolean enabled() {
        try {
            return StatusBarDriver.systemBarsEnabled();
        } catch (Throwable t) {
            return false;
        }
    }

    /** The window that controls the bars: the topmost full-screen application window. */
    static AospWindows.Win controlling() {
        AospWindows.Win best = null;
        for (AospWindows.Win w : AospWindows.sWindows.values()) {
            WindowManager.LayoutParams a = w.attrs;
            if (a == null || a.type >= WindowManager.LayoutParams.FIRST_SUB_WINDOW) continue;
            if (a.width != WindowManager.LayoutParams.MATCH_PARENT
                    || a.height != WindowManager.LayoutParams.MATCH_PARENT) continue;
            if (best == null || w.added > best.added) best = w;
        }
        return best;
    }

    /** Recomputes the bars; true when they are not what they were. */
    static synchronized boolean update() {
        boolean enabled = enabled();
        int statusH = 0, navH = 0, navPos = StatusBarDriver.NAV_BAR_BOTTOM, appearance = 0;
        boolean status = true, nav = true;
        if (enabled) {
            Resources res = AospWindows.resources();
            if (res != null) {
                statusH = com.android.internal.policy.SystemBarUtils.getStatusBarHeight(res, null);
                try {
                    navPos = StatusBarDriver.navBarPosition(null);
                } catch (Throwable t) {
                    navPos = StatusBarDriver.NAV_BAR_BOTTOM;
                }
                navH = res.getDimensionPixelSize(navPos == StatusBarDriver.NAV_BAR_BOTTOM
                        ? com.android.internal.R.dimen.navigation_bar_height
                        : com.android.internal.R.dimen.navigation_bar_width);
            }
            AospWindows.Win c = controlling();
            if (c != null) {
                status = (c.requestedVisibleTypes & WindowInsets.Type.statusBars()) != 0;
                nav = (c.requestedVisibleTypes & WindowInsets.Type.navigationBars()) != 0;
                appearance = c.attrs.insetsFlags.appearance;
            }
        }
        int w = AospWindows.widthPx(), h = AospWindows.heightPx();
        boolean changed = enabled != sEnabled || statusH != sStatusH || navH != sNavH || navPos != sNavPos
                || status != sStatusVisible || nav != sNavVisible || w != sW || h != sH;
        boolean appearanceChanged = appearance != sAppearance;
        sEnabled = enabled;
        sStatusH = statusH;
        sNavH = navH;
        sNavPos = navPos;
        sStatusVisible = status;
        sNavVisible = nav;
        sAppearance = appearance;
        sW = w;
        sH = h;
        if (appearanceChanged && !changed) AospWindows.main().post(Compositor::compose);
        return changed;
    }

    static Rect statusFrame() { return new Rect(0, 0, sW, sStatusH); }

    static Rect navFrame() {
        if (sNavPos == StatusBarDriver.NAV_BAR_BOTTOM) return new Rect(0, sH - sNavH, sW, sH);
        if (sNavPos == StatusBarDriver.NAV_BAR_LEFT) return new Rect(0, 0, sNavH, sH);
        return new Rect(sW - sNavH, 0, sW, sH);
    }

    /** The bars' sources, as InsetsStateController reports them: the frame always (the
     *  "ignoring visibility" insets), visible as the controlling window asked. */
    static void addSources(InsetsState state) {
        if (!sEnabled) return;
        if (sStatusH > 0) {
            InsetsSource s = state.getOrCreateSource(ID_STATUS, WindowInsets.Type.statusBars());
            s.setFrame(statusFrame());
            s.setVisible(sStatusVisible);
        }
        if (sNavH > 0) {
            InsetsSource s = state.getOrCreateSource(ID_NAV, WindowInsets.Type.navigationBars());
            s.setFrame(navFrame());
            s.setVisible(sNavVisible);
        }
    }

    /** The display less the navigation bar: DisplayPolicy's non-decor frame, the app bounds. */
    static Rect appBounds() {
        Rect r = new Rect(0, 0, AospWindows.widthPx(), AospWindows.heightPx());
        if (!sEnabled || sNavH <= 0) return r;
        Rect nav = navFrame();
        if (sNavPos == StatusBarDriver.NAV_BAR_BOTTOM) r.bottom = nav.top;
        else if (sNavPos == StatusBarDriver.NAV_BAR_LEFT) r.left = nav.right;
        else r.right = nav.left;
        return r;
    }

    static int statusHeight() { return sEnabled ? sStatusH : 0; }
    static int navThickness() { return sEnabled ? sNavH : 0; }
    static boolean navOnBottom() { return sNavPos == StatusBarDriver.NAV_BAR_BOTTOM; }
    static boolean hidden(int types) {
        if (!sEnabled) return false;
        if ((types & WindowInsets.Type.statusBars()) != 0 && !sStatusVisible) return true;
        return (types & WindowInsets.Type.navigationBars()) != 0 && !sNavVisible;
    }
    static int appearance() { return sAppearance; }

    static boolean paintsAny() { return sEnabled && (sStatusVisible && sStatusH > 0 || sNavVisible && sNavH > 0); }

    /** SystemUI's part of a frame: the glyphs of each shown bar, dark on a light-bar appearance. */
    static void paintGlyphs(Canvas canvas, float density) {
        if (!sEnabled) return;
        if (sStatusVisible && sStatusH > 0) {
            boolean light = (sAppearance & WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS) != 0;
            StatusBarDriver.paintStatusGlyphs(canvas, sW, sStatusH, light ? 0xDD000000 : 0xFFFFFFFF, density);
        }
        if (sNavVisible && sNavH > 0) {
            boolean light = (sAppearance & WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS) != 0;
            Rect nav = navFrame();
            StatusBarDriver.paintNavGlyphs(canvas, sW, sH, nav.top, sNavH, sNavPos,
                    light ? 0xFF1F1F1F : 0xFFFFFFFF, density);
        }
    }
}
