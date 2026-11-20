// Win.apk's window under AOSP's own window root: the one display, the window manager service and
// its sessions, and the host surface the frames go to (docs/research/aosp-views.md, step V1).
//
// On Android an app's ViewRootImpl talks to system_server: IWindowManager.openSession gives an
// IWindowSession; addToDisplayAsUser registers the window, relayout gives it a frame, a
// configuration and a SurfaceControl, and ViewRootImpl then draws into the Surface it copies from
// that control. Here the "server" is in the process and the screen is the host window:
//
//   * the display is the host window's box (DisplayConfig's size, the resources' density), handed
//     out as AOSP's DisplayInfo through an IDisplayManager, so DisplayManagerGlobal and Display are
//     AOSP's own;
//   * a window's frame is computed by AOSP's own WindowLayout.computeFrames over the display, as
//     WindowManagerService does with no system bars (the status bar and IME insets come with V2);
//   * its SurfaceControl is Win.apk's (android.view.SurfaceControl stays Win.apk's), sized to the
//     frame, and the Surface ViewRootImpl copies from it draws in software: lockCanvas gives a
//     Canvas on a frame Bitmap (AOSP's graphics) and unlockCanvasAndPost presents it to the host
//     (NativeCocoa.presentBitmap), the same way a SurfaceView's Surface already does;
//   * frames are timed by NativeDisplayEventReceiver's clock, which Choreographer (AOSP's) asks.
//
// One host surface serves the activity windows (the host's render loop ticks it and reports its
// size); a window on top of another (a dialog, a popup) draws into its own Surface, which the host
// shows over the window as it shows any Surface: later presents win (no composition yet).
package com.winapk.aospviews;

import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Rect;
import android.hardware.display.DisplayManagerGlobal;
import android.hardware.display.IDisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.util.MergedConfiguration;
import android.view.Display;
import android.view.DisplayAdjustments;
import android.view.DisplayInfo;
import android.view.IWindow;
import android.view.IWindowManager;
import android.view.IWindowSession;
import android.view.IWindowSessionCallback;
import android.view.InputChannel;
import android.view.InsetsSourceControl;
import android.view.InsetsState;
import android.view.SurfaceControl;
import android.view.WindowLayout;
import android.view.WindowManager;
import android.view.WindowManagerGlobal;
import android.window.ClientWindowFrames;

import com.macapk.loader.MetalSurfaceCallbacks;
import com.macapk.loader.NativeCocoa;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

public final class AospWindows {
    private AospWindows() {}

    static final String TAG = "[aosp-views] ";

    // ---- the display -------------------------------------------------------------------------

    private static volatile Context sMetricsContext;
    private static volatile int sWidthPx, sHeightPx;
    private static volatile boolean sRegistered;

    /** The context whose resources give the density (the application's). */
    static void noteContext(Context c) {
        if (sMetricsContext == null && c != null) {
            Context app = c.getApplicationContext();
            sMetricsContext = app != null ? app : c;
        }
    }

    static int widthPx() {
        int w = sWidthPx;
        if (w <= 0) w = com.macapk.loader.DisplayConfig.realWidthPx();
        return w > 0 ? w : 720;
    }

    static int heightPx() {
        int h = sHeightPx;
        if (h <= 0) h = com.macapk.loader.DisplayConfig.realHeightPx();
        return h > 0 ? h : 1280;
    }

    static DisplayMetrics metrics() {
        Context c = sMetricsContext;
        DisplayMetrics m = null;
        try {
            if (c != null) m = c.getResources().getDisplayMetrics();
        } catch (Throwable ignored) {
            // Resources not up yet.
        }
        if (m == null) {
            m = new DisplayMetrics();
            m.setToDefaults();
        }
        return m;
    }

    /** The one display's DisplayInfo: the host window's box at the resources' density. */
    static DisplayInfo displayInfo() {
        DisplayMetrics m = metrics();
        int w = widthPx(), h = heightPx();
        float hz = 1_000_000_000f / NativeDisplayEventReceiver.frameIntervalNanos();
        DisplayInfo d = new DisplayInfo();
        d.layerStack = 0;
        d.displayId = Display.DEFAULT_DISPLAY;
        d.type = Display.TYPE_INTERNAL;
        d.name = "Built-in Screen";
        d.uniqueId = "local:0";
        d.logicalWidth = w;
        d.logicalHeight = h;
        d.appWidth = w;
        d.appHeight = h;
        d.smallestNominalAppWidth = Math.min(w, h);
        d.smallestNominalAppHeight = Math.min(w, h);
        d.largestNominalAppWidth = Math.max(w, h);
        d.largestNominalAppHeight = Math.max(w, h);
        d.rotation = 0;
        d.modeId = 1;
        d.defaultModeId = 1;
        d.renderFrameRate = hz;
        d.supportedModes = new Display.Mode[] {new Display.Mode(1, w, h, hz)};
        d.logicalDensityDpi = m.densityDpi > 0 ? m.densityDpi : DisplayMetrics.DENSITY_DEFAULT;
        d.physicalXDpi = m.xdpi > 0 ? m.xdpi : d.logicalDensityDpi;
        d.physicalYDpi = m.ydpi > 0 ? m.ydpi : d.logicalDensityDpi;
        d.state = Display.STATE_ON;
        d.committedState = Display.STATE_ON;
        d.flags = Display.FLAG_SUPPORTS_PROTECTED_BUFFERS | Display.FLAG_TRUSTED;
        d.brightnessMinimum = 0f;
        d.brightnessMaximum = 1f;
        d.brightnessDefault = 0.5f;
        d.presentationDeadlineNanos = NativeDisplayEventReceiver.frameIntervalNanos();
        return d;
    }

    /** Context.getDisplayNoVerify / ViewHost.newDisplay: AOSP's Display of the one display. */
    static Display defaultDisplay() {
        ensureServices();
        DisplayManagerGlobal g = DisplayManagerGlobal.getInstance();
        return g.getCompatibleDisplay(Display.DEFAULT_DISPLAY, DisplayAdjustments.DEFAULT_DISPLAY_ADJUSTMENTS);
    }

    // ---- the services --------------------------------------------------------------------------

    static synchronized void ensureServices() {
        if (sRegistered) return;
        sRegistered = true;
        Services.register(Context.DISPLAY_SERVICE, IDisplayManager.class, new DisplayService());
        Services.register(Context.WINDOW_SERVICE, IWindowManager.class, new WindowManagerService());
        Services.register(Context.INPUT_METHOD_SERVICE, com.android.internal.view.IInputMethodManager.class,
                new InputMethodService());
    }

    /** IDisplayManager, the calls DisplayManagerGlobal makes for the one display. */
    public static final class DisplayService {
        public DisplayInfo getDisplayInfo(int displayId) {
            return displayId == Display.DEFAULT_DISPLAY ? displayInfo() : null;
        }
        public int[] getDisplayIds(boolean includeDisabled) { return new int[] {Display.DEFAULT_DISPLAY}; }
        public android.graphics.Point getStableDisplaySize() {
            return new android.graphics.Point(widthPx(), heightPx());
        }
        public boolean isUidPresentOnDisplay(int uid, int displayId) { return displayId == Display.DEFAULT_DISPLAY; }
        public float getPreferredWideGamutColorSpaceId() { return 0; }
    }

    /** IWindowManager, the calls an app process makes. */
    public static final class WindowManagerService {
        private final WindowSession mSession = new WindowSession();
        private final IWindowSession mSessionProxy = Services.of(IWindowSession.class, mSession);

        public IWindowSession openSession(IWindowSessionCallback callback) { return mSessionProxy; }
        public float getCurrentAnimatorScale() { return 1f; }
        public float getAnimationScale(int which) { return 1f; }
        public float[] getAnimationScales() { return new float[] {1f, 1f, 1f}; }
        public boolean useBLAST() { return false; }
        public boolean hasNavigationBar(int displayId) { return false; }
        public boolean isInTouchMode(int displayId) { return true; }
        public boolean isKeyguardLocked() { return false; }
        public boolean isKeyguardSecure(int userId) { return false; }
        public int getDefaultDisplayRotation() { return 0; }
        public int getDisplayUserRotation(int displayId) { return 0; }
        public void getInitialDisplaySize(int displayId, android.graphics.Point size) { size.set(widthPx(), heightPx()); }
        public void getBaseDisplaySize(int displayId, android.graphics.Point size) { size.set(widthPx(), heightPx()); }
        public int getInitialDisplayDensity(int displayId) { return metrics().densityDpi; }
        public int getBaseDisplayDensity(int displayId) { return metrics().densityDpi; }
        public boolean isTaskSnapshotSupported() { return false; }
        public int getDockedStackSide() { return -1; }
    }

    /** IInputMethodManager: the IME service InputMethodManager asks for. No input method here yet
     *  (V2 puts the host's IME behind it): no methods, no subtypes, no session to bind. */
    public static final class InputMethodService {
        public java.util.List<android.view.inputmethod.InputMethodInfo> getInputMethodList(int userId,
                int directBootAwareness) {
            return new java.util.ArrayList<>();
        }
        public java.util.List<android.view.inputmethod.InputMethodInfo> getEnabledInputMethodList(int userId) {
            return new java.util.ArrayList<>();
        }
        public java.util.List<android.view.inputmethod.InputMethodSubtype> getEnabledInputMethodSubtypeList(
                String imiId, boolean allowsImplicitlyEnabledSubtypes, int userId) {
            return new java.util.ArrayList<>();
        }
    }

    // ---- a window's session ------------------------------------------------------------------

    static final class Win {
        final IWindow window;
        WindowManager.LayoutParams attrs;
        Rect frame = new Rect();
        SurfaceControl surface;
        int seq;

        Win(IWindow window) { this.window = window; }
    }

    static final Map<android.os.IBinder, Win> sWindows = new ConcurrentHashMap<>();

    /** IWindowSession: what WindowManagerService does for a window, with no system bars. */
    public static final class WindowSession {
        private final WindowLayout mLayout = new WindowLayout();

        public int addToDisplayAsUser(IWindow window, WindowManager.LayoutParams attrs, int viewVisibility,
                int displayId, int userId, int requestedVisibleTypes, InputChannel outInputChannel,
                InsetsState outInsetsState, InsetsSourceControl.Array outActiveControls, Rect outAttachedFrame,
                float[] outSizeCompatScale) {
            return addToDisplay(window, attrs, viewVisibility, displayId, requestedVisibleTypes, outInputChannel,
                    outInsetsState, outActiveControls, outAttachedFrame, outSizeCompatScale);
        }

        public int addToDisplay(IWindow window, WindowManager.LayoutParams attrs, int viewVisibility,
                int layerStackId, int requestedVisibleTypes, InputChannel outInputChannel,
                InsetsState outInsetsState, InsetsSourceControl.Array outActiveControls, Rect outAttachedFrame,
                float[] outSizeCompatScale) {
            Win w = new Win(window);
            w.attrs = new WindowManager.LayoutParams();
            w.attrs.copyFrom(attrs);
            sWindows.put(window.asBinder(), w);
            fillInsets(outInsetsState);
            if (outAttachedFrame != null) outAttachedFrame.set(0, 0, -1, -1);  // not valid: no parent frame
            if (outSizeCompatScale != null && outSizeCompatScale.length > 0) outSizeCompatScale[0] = 1f;
            return WindowManagerGlobal.ADD_OKAY;
        }

        public int addToDisplayWithoutInputChannel(IWindow window, WindowManager.LayoutParams attrs,
                int viewVisibility, int layerStackId, InsetsState outInsetsState, Rect outAttachedFrame,
                float[] outSizeCompatScale) {
            return addToDisplay(window, attrs, viewVisibility, layerStackId, 0, null, outInsetsState, null,
                    outAttachedFrame, outSizeCompatScale);
        }

        public void remove(IWindow window) {
            Win w = sWindows.remove(window.asBinder());
            if (w != null && w.surface != null) w.surface.release();
        }

        public int relayout(IWindow window, WindowManager.LayoutParams attrs, int requestedWidth,
                int requestedHeight, int viewVisibility, int flags, int seq, int lastSyncSeqId,
                ClientWindowFrames outFrames, MergedConfiguration outMergedConfiguration,
                SurfaceControl outSurfaceControl, InsetsState outInsetsState,
                InsetsSourceControl.Array outActiveControls, Bundle outSyncSeqIdBundle) {
            Win w = sWindows.get(window.asBinder());
            if (w == null) {
                w = new Win(window);
                w.attrs = new WindowManager.LayoutParams();
                sWindows.put(window.asBinder(), w);
            }
            if (attrs != null) w.attrs.copyFrom(attrs);
            w.seq = seq;
            InsetsState state = new InsetsState();
            fillInsets(state);
            Rect bounds = new Rect(0, 0, widthPx(), heightPx());
            ClientWindowFrames frames = outFrames != null ? outFrames : new ClientWindowFrames();
            mLayout.computeFrames(w.attrs, state, bounds, bounds,
                    android.app.WindowConfiguration.WINDOWING_MODE_FULLSCREEN, requestedWidth, requestedHeight,
                    android.view.WindowInsets.Type.defaultVisible(), 1f, frames);
            w.frame.set(frames.frame);
            if (outMergedConfiguration != null) {
                Configuration global = new Configuration(configuration());
                global.windowConfiguration.setBounds(bounds);
                global.windowConfiguration.setAppBounds(bounds);
                global.windowConfiguration.setMaxBounds(bounds);
                outMergedConfiguration.setConfiguration(global, new Configuration());
            }
            if (outInsetsState != null) outInsetsState.set(state, true);
            int result = 0;
            if (viewVisibility == android.view.View.VISIBLE && outSurfaceControl != null) {
                int fw = Math.max(1, frames.frame.width()), fh = Math.max(1, frames.frame.height());
                if (w.surface == null || w.surface.getWidth() != fw || w.surface.getHeight() != fh) {
                    if (w.surface != null) w.surface.release();
                    w.surface = new SurfaceControl.Builder().setName(w.attrs.getTitle() + "")
                            .setBufferSize(fw, fh).build();
                    result |= WindowManagerGlobal.RELAYOUT_RES_SURFACE_CHANGED;
                }
                outSurfaceControl.copyFrom(w.surface, "relayout");
            } else if (w.surface != null && viewVisibility != android.view.View.VISIBLE) {
                w.surface.release();
                w.surface = null;
            }
            return result;
        }

        public void relayoutAsync(IWindow window, WindowManager.LayoutParams attrs, int requestedWidth,
                int requestedHeight, int viewVisibility, int flags, int seq, int lastSyncSeqId) {
            Win w = sWindows.get(window.asBinder());
            if (w != null && attrs != null) w.attrs.copyFrom(attrs);
        }

        public boolean outOfMemory(IWindow window) { return false; }

        public void finishDrawing(IWindow window, SurfaceControl.Transaction postDrawTransaction, int seqId) {
            if (postDrawTransaction != null) postDrawTransaction.apply();
        }

        public boolean performHapticFeedback(int effectId, boolean always) { return false; }

        public boolean cancelDraw(IWindow window) { return false; }
    }

    static Configuration configuration() {
        Context c = sMetricsContext;
        try {
            if (c != null) return c.getResources().getConfiguration();
        } catch (Throwable ignored) {
            // Resources not up yet.
        }
        Configuration cfg = new Configuration();
        cfg.setToDefaults();
        return cfg;
    }

    /** The display's frame and nothing on it: no status bar, navigation bar or IME yet (V2). */
    static void fillInsets(InsetsState state) {
        if (state == null) return;
        Rect display = new Rect(0, 0, widthPx(), heightPx());
        state.setDisplayFrame(display);
    }

    // ---- the host surface --------------------------------------------------------------------

    private static long sHostWindow;
    private static long sHostView;

    /** The activity windows' host surface: the window the launcher showed and a render view in it
     *  whose loop reports its size and ticks (Activity.macapkInstallContentDirect's wiring for a
     *  plain-View app). Idempotent. */
    static synchronized void ensureHostSurface(String title) {
        if (sHostView != 0) return;
        try {
            long window = NativeCocoa.getPlaceholderWindow();
            if (window == 0) window = NativeCocoa.createWindow(title);
            else NativeCocoa.setWindowTitle(window, title);
            int w = NativeCocoa.phoneWidth(), h = NativeCocoa.phoneHeight();
            long view = NativeCocoa.createMetalSurfaceView(w > 0 ? w : 800, h > 0 ? h : 600);
            if (view == 0) return;
            NativeCocoa.setRenderer(view, new HostSurface());
            NativeCocoa.setContentView(window, view);
            NativeCocoa.showWindow(window);
            sHostWindow = window;
            sHostView = view;
        } catch (UnsatisfiedLinkError e) {
            // A plain JVM (no host): the windows draw into memory only.
        }
    }

    /** The host's callbacks for the render view: its size (a resize of the host window). The
     *  frames themselves are ViewRootImpl's, on Choreographer's clock. */
    static final class HostSurface implements MetalSurfaceCallbacks {
        private final Handler mMain = new Handler(Looper.getMainLooper());

        @Override public void macapkSurfaceChanged(int widthPx, int heightPx) {
            mMain.post(() -> resize(widthPx, heightPx));
        }

        @Override public void macapkDrawFrame() {}
    }

    /** The display is now w x h px: every window gets its new frame, as WindowManagerService's
     *  IWindow.resized does. */
    static void resize(int w, int h) {
        if (w <= 0 || h <= 0 || (w == sWidthPx && h == sHeightPx)) return;
        boolean first = sWidthPx <= 0;
        sWidthPx = w;
        sHeightPx = h;
        if (first) return;
        com.macapk.loader.DisplayConfig.commitPending();
        for (Win win : sWindows.values()) {
            try {
                ClientWindowFrames frames = new ClientWindowFrames();
                InsetsState state = new InsetsState();
                fillInsets(state);
                Rect bounds = new Rect(0, 0, w, h);
                new WindowLayout().computeFrames(win.attrs, state, bounds, bounds,
                        android.app.WindowConfiguration.WINDOWING_MODE_FULLSCREEN,
                        WindowLayout.UNSPECIFIED_LENGTH, WindowLayout.UNSPECIFIED_LENGTH,
                        android.view.WindowInsets.Type.defaultVisible(), 1f, frames);
                MergedConfiguration merged = new MergedConfiguration();
                Configuration global = new Configuration(configuration());
                global.windowConfiguration.setBounds(bounds);
                global.windowConfiguration.setAppBounds(bounds);
                global.windowConfiguration.setMaxBounds(bounds);
                merged.setConfiguration(global, new Configuration());
                win.window.resized(frames, false, merged, state, true, false, Display.DEFAULT_DISPLAY, -1, false);
            } catch (Throwable t) {
                System.err.println(TAG + "resize of a window failed: " + t);
            }
        }
    }
}
