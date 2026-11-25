// com.macapk.loader.DisplayConfig -- the ONE committed display size, the pure
// derivation of DisplayMetrics + Configuration from it, and the commit that
// applies a new size the way a device applies a display change.
//
// WHY THIS EXISTS (v1.0.1637). The window a Mac.apk app draws into IS its
// display (the window paints its own system bars), and a user can resize it.
// Before this, nothing wrote size or orientation after boot: Boot built
// DisplayMetrics + Configuration once from the native canvas globals, Display
// read those globals live (and a window drag never writes them), and the view
// tree was laid out at whatever size the surface actually became. So after any
// resize -- and, until Stage 0, even at launch -- an app was told one display
// and handed another. Full AOSP contract, the 62-app configChanges survey and
// the staged plan: docs/window-resize-and-cross-thread-surface_2026-09-23.md.
//
// THE MODEL. A window resize is a DISPLAY size change (not a freeform-window
// override), because our window is the whole device screen:
//   - one committed real size, written ONLY on the Android main (owner) thread;
//   - DisplayMetrics / Configuration derived from it by compute(), the same
//     function Boot uses, so launch and resize cannot disagree;
//   - the commit updates Resources (rebuilding the ResourceIndex only when a
//     qualifier threshold is actually crossed), tells the process
//     (Application + ComponentCallbacks) and the display listeners once, then
//     applies AOSP's per-SDK relaunch / report rules to the top activity and
//     re-lays-out the view tree.
//
// THREADING. The signal comes from the AppKit render thread (drawInMTKView ->
// macapkNoteSurfaceSize, native). It only records the newest size and posts ONE
// commit through an OWNER-ONLY Handler (v1.0.1636), so the commit never runs on
// the OS-main drain and is FIFO-ordered ahead of the doSurfaceChanged that the
// same native frame posts right after it. A newer size supersedes an older one
// that has not run yet. doSurfaceChanged also calls commitPending() before it
// measures (idempotent), for any path that reaches it directly.
//
// GATES. MACAPK_RESIZE_CONFIG=0 is the bisect gate for the whole v1.0.1637
// display model: the committed size freezes at the boot value AND the native
// canvas fit goes back to the raw visible frame (cocoa_bridge.mm
// content_budget_for_visible reads the same variable), which reproduces the
// old portrait launch metrics exactly (418x906dp for an 808x1796 surface).
// A change goes to AOSP's app layer (AppHost.configurationChanged), which relaunches an activity
// whose android:configChanges does not cover it, as ActivityRecord.shouldRelaunchLocked does.
//
// THE DENSITY (Win.apk). The display's density is committed with its size: the host window shows
// the phone at a scale in px per dp, and when that scale changes (the window maximized, which shows
// the phone turned as large as the screen; restored; moved to a monitor of another scale) the
// native side sends the density for the new scale with the new size (macapkNoteDisplay). As on an
// Android display whose density changes (Settings > Display size, a desktop window moved to a
// display of another density), it is a configuration change with a new densityDpi
// (ActivityInfo.CONFIG_DENSITY): ResourcesImpl.updateConfiguration takes the DisplayMetrics'
// density from it and flushes the density-dependent caches, ActivityThread updates
// DisplayMetrics.DENSITY_DEVICE and Bitmap's default density, and an activity that does not declare
// configChanges="density" is relaunched with its saved state. DENSITY_DEVICE_STABLE stays the
// launch density, as it stays the device's on Android.
package com.macapk.loader;

import android.content.res.Configuration;
import android.content.res.Resources;
import android.util.DisplayMetrics;
import com.macapk.loader.res.ResBackend;
import com.macapk.loader.view.ViewHost;

public final class DisplayConfig {
    private static final String TAG = "Mac.apk.DisplayConfig";

    private DisplayConfig() {}

    static final boolean RESIZE_ON =
        !"0".equals(System.getenv("MACAPK_RESIZE_CONFIG"));

    // ---- the committed display ------------------------------------------------

    private static volatile int sRealW, sRealH;   // px; 0 until Boot seeds
    /** The committed density in dpi; 0 until the first density change (the launch density). */
    private static volatile int sDensityDpi;
    /** The configuration of the last commit after Boot (null before one). */
    private static volatile Configuration sCommitted;

    /** The display's density now, in dpi: DENSITY_DEVICE_STABLE (the launch density) until the
     *  host window shows the phone at another scale. */
    public static int densityDpi() {
        int d = RESIZE_ON ? sDensityDpi : 0;
        return d > 0 ? d : android.util.DisplayMetrics.DENSITY_DEVICE_STABLE;
    }

    /** densityDpi() as px per dp. */
    public static float density() {
        return densityDpi() / (float) android.util.DisplayMetrics.DENSITY_DEFAULT;
    }

    /** Copies the committed display's fields (dp sizes, density, orientation, bounds) over `c`, a
     *  configuration made from a Resources that may not have heard the newest commit yet (the
     *  in-process window manager's, built while the commit is on its way to the process), so the
     *  windows are never handed the display it replaced. Nothing before the first commit. */
    public static void overlayCommitted(Configuration c) {
        Configuration last = sCommitted;
        if (last != null && c != null) adoptDisplayFields(last, c);
    }

    /** The committed display width in px (decor included). Falls back to the
     *  native canvas before Boot has seeded, which is exactly the old answer. */
    public static int realWidthPx() {
        // Gate off = the pre-v1.0.1637 answer exactly: the LIVE canvas globals
        // (which Cmd+R / setRequestedOrientation move and a drag does not).
        int w = RESIZE_ON ? sRealW : 0;
        return w > 0 ? w : Math.round(NativeCocoa.phoneWidth() * pxPerPoint());
    }
    public static int realHeightPx() {
        int h = RESIZE_ON ? sRealH : 0;
        return h > 0 ? h : Math.round(NativeCocoa.phoneHeight() * pxPerPoint());
    }

    /** Pixels per point of the canvas BEFORE the first surface says what it
     *  really is: the launch screen's backing scale, which is what the device
     *  density is derived from (v1.0.2187, CocoaDisplayDensityDpi; 2 on Retina,
     *  1 on a 1x monitor). Was a literal 2 ("retina"), so on a 1x monitor the
     *  seed described a display twice the window's pixels until the first
     *  surface commit halved it. Under a MACAPK_DISPLAY_DENSITY pin the two can
     *  differ; the first surface commit then corrects the seed, as before. */
    private static float pxPerPoint() {
        return android.util.DisplayMetrics.macapkDeviceDensity();
    }

    /** Boot: seed from the native canvas, AFTER CocoaSetMaxAspectRatio has
     *  finished reshaping it (that call can grow or shrink g_phone_w/h
     *  synchronously; reading before it measured a canvas the window no longer
     *  has). With Stage 0 the canvas equals the render view's content size, so
     *  the first surfaceChanged commits nothing. */
    static void seedFromCanvas() {
        // At the density now: a density change before Boot (the window maximized while the app
        // started) is committed with no Resources to tell.
        sRealW = Math.round(NativeCocoa.phoneWidth() * density());
        sRealH = Math.round(NativeCocoa.phoneHeight() * density());
    }

    // ---- the pure derivation ---------------------------------------------------

    /** Fill `m` and the display-derived fields of `c` for a real display of
     *  realW x realH px at the committed density (densityDpi(): the launch
     *  screen's until the window's scale changes). Reads the decor accessors, which read the
     *  COMMITTED display, so the caller commits the size and density first.
     *
     *  AOSP (android14-release DisplayContent):
     *    computeScreenAppConfiguration :2385-2399
     *      appBounds = non-decor frame; orientation = (dw <= dh) ? PORTRAIT : LANDSCAPE
     *      screenWidthDp  = (int) (configFrame.width()  / density + 0.5f)
     *      screenHeightDp = (int) (configFrame.height() / density + 0.5f)
     *    computeSizeRanges :2546-2575 / adjustDisplaySizeRanges :2577
     *      smallestScreenWidthDp = (int) (min over ALL FOUR rotations of the
     *      config-frame width / density + 0.5f)  -- rotation-INVARIANT, so a
     *      rotate (Cmd+R) is not a smallest-width change.
     *  The pre-O max-aspect letterbox and the decor ladder are unchanged from
     *  Boot's v1.0.874 / v1.0.939 / v1.0.997 code, moved here verbatim in
     *  effect; see those notes in Boot.run's history. */
    public static void compute(int realW, int realH, DisplayMetrics m, Configuration c,
                               boolean log) {
        // ResourcesImpl.updateConfiguration's: the metrics' density from the configuration's
        // densityDpi, scaledDensity that times the font scale.
        final int densityDpi = densityDpi();
        final float density = densityDpi / (float) android.util.DisplayMetrics.DENSITY_DEFAULT;
        m.densityDpi = densityDpi;
        m.density = density;
        m.scaledDensity = density * (c.fontScale != 0f ? c.fontScale : 1.0f);
        final int navW   = ViewHost.get().navDecorWidthPx();
        final int navH   = ViewHost.get().navDecorHeightPx();
        final int status = ViewHost.get().statusDecorPx();
        // DisplayMetrics = the APP area (v1.0.939): nav decor off the axis the
        // bar occupies (v1.0.997). Identical to Display.getWidth/getHeight.
        m.widthPixels  = realW - navW;
        m.heightPixels = realH - navH;
        // AOSP's pre-O maximum aspect ratio (v1.0.874). See
        // Boot.macapkMaxAspectRatio for the gate and its citations.
        final float maxAspect = Boot.sMaxAspectRatio;
        if (maxAspect >= 1.0f) {
            int w = m.widthPixels, h = m.heightPixels;
            int shortSide = Math.min(w, h), longSide = Math.max(w, h);
            if (shortSide > 0) {
                float containingRatio = (float) longSide / (float) shortSide;
                float ratio = Math.min(containingRatio, maxAspect);
                // AppCompatAspectRatioPolicy.applyAspectRatio's tolerance
                // (ASPECT_RATIO_ROUNDING_TOLERANCE = 0.005f) and rounding.
                if (containingRatio - ratio > 0.005f) {
                    if (w < h) m.heightPixels = (int) ((w * ratio) + 0.5f);
                    else       m.widthPixels  = (int) ((h * ratio) + 0.5f);
                    if (log) {
                        NativeLog.i("Mac.apk.Boot", "pre-O max aspect " + maxAspect
                            + ": window " + w + "x" + h + " -> "
                            + m.widthPixels + "x" + m.heightPixels);
                    }
                }
            }
        }
        c.densityDpi = densityDpi;
        c.screenWidthDp = (int) (m.widthPixels / density + 0.5f);
        // v1.0.939: the config height excludes BOTH decor bars (WMS's "config"
        // rung of the size ladder), getSize/getMetrics only the navigation bar.
        c.screenHeightDp = (int) ((realH - navH - status) / density + 0.5f);
        // Rotation-invariant smallest width: the portrait config width is the
        // short side; the landscape one is the long side minus a side nav bar
        // when the bar moves (phone-class displays). Same ladder as
        // Display.getCurrentSizeRange's smallest.x.
        final int shortSide = Math.min(realW, realH), longSide = Math.max(realW, realH);
        final int sideNav = StatusBarDriver.navBarCanMove()
                ? ViewHost.get().navDecorPx() : 0;
        c.smallestScreenWidthDp =
            (int) (Math.min(shortSide, longSide - sideNav) / density + 0.5f);
        // screenLayout FOLLOWS the dp metrics (AOSP reduceScreenLayout).
        ResBackend.get().deriveScreenLayout(c);
        c.orientation = (realW <= realH)
            ? Configuration.ORIENTATION_PORTRAIT : Configuration.ORIENTATION_LANDSCAPE;
        // The @hide windowConfiguration AndroidX Window reads reflectively:
        // bounds/maxBounds the whole display, appBounds the app area.
        android.graphics.Rect whole = new android.graphics.Rect(0, 0, realW, realH);
        c.windowConfiguration.setBounds(whole);
        c.windowConfiguration.setAppBounds(
            new android.graphics.Rect(0, 0, m.widthPixels, m.heightPixels));
        c.windowConfiguration.setMaxBounds(whole);
    }

    // ---- the resize signal -----------------------------------------------------

    private static final Object sLock = new Object();
    private static int sPendingW, sPendingH, sPendingDpi;
    private static boolean sCommitPosted;
    private static boolean sApplying;
    private static android.os.Handler sOwnerHandler;

    /** NATIVE ENTRY (cocoa_bridge.mm drawInMTKView), any thread: the primary
     *  window's render view now has a w x h px drawable.
     *
     *  RETURNS whether the display is COMMITTED at w x h -- true only when no
     *  commit is queued or mid-apply and the committed size equals w x h.
     *  For a RESIZE the native side holds its macapkSurfaceChanged re-fire
     *  until this answers true (bounded), so a SurfaceView / GLSurfaceView
     *  engine hears surfaceChanged AFTER onConfigurationChanged and with
     *  Resources/DisplayMetrics already describing the new size -- AOSP's
     *  order (ViewRootImpl applies the configuration in performTraversals
     *  before SurfaceView.updateSurface fires surfaceChanged). v1.0.1637 first
     *  shipped the opposite order: the re-fire ran synchronously on the AppKit
     *  thread while the commit sat in the owner queue, so a live Unity surface
     *  was re-created against the OLD configuration; an intermittent Unity
     *  freeze on a hand-drag resize was observed on that build.
     *  The first-surface (boot) fire ignores the answer. Answers true when the
     *  display model is off, so nothing is ever held then. */
    public static boolean macapkNoteSurfaceSize(int w, int h) {
        return macapkNoteDisplay(w, h, 0);
    }

    /** NATIVE ENTRY (native_cocoa.cpp's render thread), any thread: the display is now w x h px
     *  at densityDpi (0: the density does not change). One display change, committed as one
     *  configuration change; answers as macapkNoteSurfaceSize. */
    public static boolean macapkNoteDisplay(int w, int h, int densityDpi) {
        if (!RESIZE_ON || w <= 0 || h <= 0) return true;
        if (!android.os.Looper.MACAPK_REAL_LOOPER) {
            // The legacy looper has no owner thread to serialize on; a post
            // there lands in GLOBAL_QUEUE, which a renderer-path frame never
            // drains, so the flag below could stick. Commit inline instead.
            synchronized (sLock) { sPendingW = w; sPendingH = h; sPendingDpi = densityDpi; }
            commitPending();
            return true;
        }
        android.os.Handler hd;
        synchronized (sLock) {
            sPendingW = w;
            sPendingH = h;
            sPendingDpi = densityDpi;
            boolean sameDensity = densityDpi <= 0 || densityDpi == densityDpi();
            if (!sCommitPosted && !sApplying && w == sRealW && h == sRealH && sameDensity) return true;
            if (sCommitPosted) return false;   // the queued commit takes the newest size
            hd = ownerHandler();
            if (hd == null) {
                // No main Looper yet: Boot seeds from the canvas, at this density.
                if (densityDpi > 0) sDensityDpi = densityDpi;
                return true;
            }
            sCommitPosted = true;
        }
        if (!hd.post(DisplayConfig::commitPending)) {
            synchronized (sLock) { sCommitPosted = false; }
            return true;
        }
        return false;
    }

    private static android.os.Handler ownerHandler() {
        if (sOwnerHandler == null) {
            android.os.Looper main = android.os.Looper.getMainLooper();
            if (main == null) return null;
            // OWNER-ONLY on the real-looper path: the commit mutates Resources
            // and the view tree, so it must never run on the OS-main drain
            // (v1.0.1636). The legacy path has no owner distinction.
            sOwnerHandler = android.os.Looper.MACAPK_REAL_LOOPER
                ? android.os.Handler.macapkCreateOwnerOnly(main)
                : new android.os.Handler(main);
        }
        return sOwnerHandler;
    }

    /** Owner thread: apply the newest pending size, if it differs from the
     *  committed one. Idempotent, so ViewCanvasDriver.doSurfaceChanged can call
     *  it unconditionally before it measures. */
    public static void commitPending() {
        int w, h, dpi;
        synchronized (sLock) {
            sCommitPosted = false;
            w = sPendingW;
            h = sPendingH;
            dpi = sPendingDpi > 0 ? sPendingDpi : densityDpi();
            sApplying = true;
        }
        try {
            if (!RESIZE_ON || w <= 0 || h <= 0) return;
            if (w == sRealW && h == sRealH && dpi == densityDpi()) return;
            apply(w, h, dpi);
        } catch (Throwable t) {
            NativeLog.w(TAG, "display commit " + w + "x" + h + " threw: " + t);
        } finally {
            synchronized (sLock) { sApplying = false; }
        }
    }

    // ---- the commit ---------------------------------------------------------------

    private static void apply(int w, int h, int dpi) {
        final int oldW = sRealW, oldH = sRealH, oldDpi = densityDpi();
        // A host window resize / rotate is not input, so not in a recording: note it
        // (a replay refuses at the next `window` line unless the size comes back).
        if (InputRecorder.ON && oldW > 0 && oldH > 0 && (oldW != w || oldH != h))
            InputRecorder.unrecorded("host display resize " + oldW + "x" + oldH + " -> " + w + "x" + h + " px");
        // Commit first: Display/WindowManager (and the decor accessors compute()
        // reads) answer the new display from here on, as DisplayInfo does on a
        // device before the configuration change is delivered.
        sRealW = w;
        sRealH = h;
        sDensityDpi = dpi;
        final android.app.Application app = com.macapk.loader.app.Apps.current();
        final Resources res = (app != null) ? app.getResources() : null;
        final Configuration cur = (res != null) ? res.getConfiguration() : null;
        if (cur == null) {
            NativeLog.i(TAG, "display " + w + "x" + h + " committed before Resources "
                + "exist; boot derives from it");
            return;
        }
        final Configuration next = new Configuration(cur);
        final DisplayMetrics m = new DisplayMetrics();
        m.setTo(res.getDisplayMetrics());
        compute(w, h, m, next, false);
        sCommitted = new Configuration(next);
        final int diff = cur.diff(next);
        final boolean bounds = boundsDiffer(cur, next);
        final boolean metricsChanged = !m.equals(res.getDisplayMetrics());
        NativeLog.i(TAG, "display " + oldW + "x" + oldH + " -> " + w + "x" + h + " px: "
            + cur.screenWidthDp + "x" + cur.screenHeightDp + "dp sw" + cur.smallestScreenWidthDp
            + " -> " + next.screenWidthDp + "x" + next.screenHeightDp + "dp sw"
            + next.smallestScreenWidthDp + " " + orientName(next.orientation)
            + (dpi != oldDpi ? " density " + oldDpi + " -> " + dpi + " dpi" : "")
            + " diff=0x" + Integer.toHexString(diff) + (bounds ? " +bounds" : "")
            + " [owner=" + android.os.Looper.macapkIsActivityThread() + "]");
        if (diff == 0 && !bounds && !metricsChanged) return;

        // The activity task manager (com.macapk.loader.app.AppHost) sends the configuration to the
        // process and its activities (onConfigurationChanged, or a relaunch), and AOSP's
        // ResourcesManager updates every Resources, the system's included.
        com.macapk.loader.app.AppHost appHost = com.macapk.loader.app.AppHost.get();
        if (appHost == null || !appHost.started()) return;
        appHost.configurationChanged(next);
        try { android.hardware.display.DisplayManager.macapkNotifyDisplayChanged(0); }
        catch (Throwable t) { NativeLog.w(TAG, "display listener dispatch threw: " + t); }
        if (android.os.Looper.MACAPK_REAL_LOOPER) {
            try { ViewHost.get().scheduleLayout(); } catch (Throwable ignored) {}
        }
    }

    /** Copy the DISPLAY-derived fields of `from` into `to` (dp sizes,
     *  smallest width, screenLayout, orientation, density, window bounds).
     *  For a configuration built from an older snapshot -- the appearance
     *  flip's -- so it cannot revert a resize committed meanwhile. Returns
     *  true if anything changed. */
    static boolean adoptDisplayFields(Configuration from, Configuration to) {
        if (from == null || to == null) return false;
        boolean changed = to.screenWidthDp != from.screenWidthDp
            || to.screenHeightDp != from.screenHeightDp
            || to.smallestScreenWidthDp != from.smallestScreenWidthDp
            || to.screenLayout != from.screenLayout
            || to.orientation != from.orientation
            || to.densityDpi != from.densityDpi
            || boundsDiffer(from, to);
        if (!changed) return false;
        to.screenWidthDp = from.screenWidthDp;
        to.screenHeightDp = from.screenHeightDp;
        to.smallestScreenWidthDp = from.smallestScreenWidthDp;
        to.screenLayout = from.screenLayout;
        to.orientation = from.orientation;
        to.densityDpi = from.densityDpi;
        to.windowConfiguration.setBounds(from.windowConfiguration.getBounds());
        to.windowConfiguration.setAppBounds(from.windowConfiguration.getAppBounds());
        to.windowConfiguration.setMaxBounds(from.windowConfiguration.getMaxBounds());
        return true;
    }

    /** android14-release ConfigurationHelper.shouldUpdateWindowMetricsBounds. */
    private static boolean boundsDiffer(Configuration a, Configuration b) {
        return !java.util.Objects.equals(a.windowConfiguration.getBounds(),
                                         b.windowConfiguration.getBounds())
            || !java.util.Objects.equals(a.windowConfiguration.getMaxBounds(),
                                         b.windowConfiguration.getMaxBounds());
    }

    private static String orientName(int o) {
        return o == Configuration.ORIENTATION_LANDSCAPE ? "land" : "port";
    }
}
