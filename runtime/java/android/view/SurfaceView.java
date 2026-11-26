// android.view.SurfaceView -- a View that owns an off-thread drawing
// surface. Win.apk's own, kept with AOSP's views (build_aosp_views.sh).
//
// Every SurfaceView shares the host's one GL view (the native window), whose
// render loop reports its size each frame (macapkSurfaceChanged); this view
// turns that into its SurfaceHolder.Callback's surfaceCreated and
// surfaceChanged, on the UI thread, at the view's size, and again at every
// resize. What draws into the surface is the app's: a lockCanvas thread, a
// native engine through ANativeWindow, or a GL thread -- GLSurfaceView's own
// (AOSP's GLSurfaceView, lib/aosp-graphics.jar) or an app's copy of it --
// with an EGL window surface on the holder (MacApkEGL10, EGL14), whose frames
// the host shows under the window through the hole draw() punches.
//
// Until November 2026 a renderer was also looked for by reflection (a field
// whose value had an onDrawFrame method) and driven from the host's render
// thread, while an app's own GL thread was refused its context so that it
// died: Mac.apk could not give such a thread a window. A renderer the
// reflection could not find (an obfuscated app's, one held by a helper
// object: Waze's map) then never drew at all.
package android.view;

import com.macapk.loader.gfx.GfxBackend;
import android.content.Context;
import com.macapk.loader.MetalSurfaceCallbacks;
import com.macapk.loader.NativeCocoa;
import com.macapk.loader.NativeLog;

public class SurfaceView extends View implements MetalSurfaceCallbacks {
    private static final String TAG = "Mac.apk.SurfaceView";

    public SurfaceView(Context c) {
        super(c);
        macapkInitSurface();
    }

    /** With AOSP's views (-Dwinapk.aosp.views=1) View's constructor reads the element's attributes
     *  itself (android:id, padding, visibility...), as Win.apk's inflater does after construction
     *  for its own View; this SurfaceView, which stays Win.apk's there, hands them on. Win.apk's
     *  View(Context, null) is View(Context). */
    private static final boolean MACAPK_ATTRS_TO_VIEW = com.macapk.loader.view.ViewHost.get().aospWindows();

    public SurfaceView(Context c, android.util.AttributeSet a) {
        super(c, MACAPK_ATTRS_TO_VIEW ? a : null);
        macapkInitSurface();
    }

    private void macapkInitSurface() {
        // Share the singleton MTKView. ANGLE binds to whatever native
        // MTKView's CAMetalLayer it sees first; if every SurfaceView
        // had its own, the visible one and the ANGLE-bound one could
        // diverge (and they did -- caused a black screen on Flappy).
        long handle = NativeCocoa.getOrCreateSharedMetalSurfaceView(
            NativeCocoa.phoneWidth(), NativeCocoa.phoneHeight());
        setNativeHandle(handle);
        // The host's render loop tells this view its surface's size each frame
        // (macapkSurfaceChanged), which the holder's callbacks hear.
        if (handle != 0) NativeCocoa.setRenderer(handle, this);
    }
    /** AOSP API 21 four-argument constructor. AOSP's three-argument form is
     *  exactly this(context, attrs, defStyleAttr, 0), so delegating the other
     *  way is identical for defStyleRes == 0; a non-zero defStyleRes is
     *  DROPPED here, as in the existing FrameLayout/Button four-argument forms
     *  (TextView, ProgressBar and their subclasses honour it). Without it an
     *  app subclass calling the four-argument
     *  super() dies with NoSuchMethodError; see debug/ctorsdk_gate.sh. */
    public SurfaceView(Context context, android.util.AttributeSet attrs, int defStyleAttr,
            int defStyleRes) {
        this(context, attrs, defStyleAttr);
    }
    public SurfaceView(Context c, android.util.AttributeSet a, int s) { this(c, a); }

    public SurfaceHolder getHolder() { return holder; }
    /** AOSP setZOrderOnTop (android14 SurfaceView.java:660) moves the surface's
     *  sublayer above the window (APPLICATION_PANEL_SUBLAYER) or back to
     *  APPLICATION_MEDIA_SUBLAYER. Recorded (2026-09-26) so the per-surface
     *  layer can be placed on the right side of the view tree and so draw()
     *  does not punch a hole for a surface that sits ABOVE the window
     *  (AOSP draw() only punches when !isAboveParent()). With no per-surface
     *  layer in the runtime (the default until the native half lands) nothing
     *  reads it, exactly as before. */
    private volatile boolean mZOrderOnTop;
    public void setZOrderOnTop(boolean onTop) { mZOrderOnTop = onTop; }
    /** AOSP setZOrderMediaOverlay (:638) picks MEDIA_OVERLAY vs MEDIA sublayer;
     *  both are below the window, so for this runtime it only clears onTop the
     *  way AOSP's own body does (it resets mRequestedSubLayer below the window). */
    public void setZOrderMediaOverlay(boolean isMediaOverlay) { mZOrderOnTop = false; }
    /** Does this surface sit ABOVE its window (setZOrderOnTop)? Read by
     *  WebView.macapkBackdropFor: a below-window SurfaceView punches a hole
     *  through everything its window drew before it (AOSP draw()), an
     *  above-window one does not. */
    public final boolean macapkIsZOrderOnTop() { return mZOrderOnTop; }

    /** Per AOSP SurfaceView.setSurfaceLifecycle: governs whether the Surface
     *  tracks view visibility (destroyed/recreated on hide/show, the
     *  SURFACE_LIFECYCLE_DEFAULT / _FOLLOWS_VISIBILITY behavior) or window
     *  attachment (created once at first attach, kept alive until detach,
     *  _FOLLOWS_ATTACHMENT). Our native MTKView handle is allocated once in
     *  the constructor and lives for this View's whole lifetime regardless
     *  of visibility (see class header comment), which already matches the
     *  observable behavior of FOLLOWS_ATTACHMENT -- so there is no separate
     *  code path to switch on here. This is a store-and-report-back, not a
     *  behavior change: it exists so the call resolves instead of throwing
     *  NoSuchMethodError (nothing in our SDK_INT report gates it away). */
    public static final int SURFACE_LIFECYCLE_DEFAULT = 0;
    public static final int SURFACE_LIFECYCLE_FOLLOWS_VISIBILITY = 1;
    public static final int SURFACE_LIFECYCLE_FOLLOWS_ATTACHMENT = 2;
    private int mRequestedSurfaceLifecycleStrategy = SURFACE_LIFECYCLE_DEFAULT;
    public void setSurfaceLifecycle(int lifecycleStrategy) {
        mRequestedSurfaceLifecycleStrategy = lifecycleStrategy;
    }

    /** Per AOSP SurfaceView.setSecure: marks the surface so its content is
     *  excluded from screenshots and non-secure displays (backed on real
     *  Android by SurfaceControl.SECURE + Display.FLAG_SECURE). Mac.apk has
     *  no SurfaceFlinger and no screen-capture-exclusion path; the macOS
     *  analog is NSWindow.sharingType = .none in the native Cocoa bridge,
     *  which this file does not own and does not wire up here. Recording
     *  the flag stops the call from throwing NoSuchMethodError; actually
     *  enforcing it is a follow-on native change outside runtime/java. */
    private boolean mIsSecure;
    public void setSecure(boolean isSecure) {
        mIsSecure = isSecure;
    }

    /** Tracks whether the holder's surfaceCreated callback has fired
     *  yet. Per AOSP SurfaceView.java + the public SurfaceHolder.Callback
     *  contract, surfaceCreated MUST fire exactly once per surface
     *  lifetime (before any surfaceChanged), and surfaceChanged fires on
     *  every dimension/format change. MTKView calls macapkSurfaceChanged
     *  repeatedly when the layer resizes, so we latch this flag to keep
     *  surfaceCreated single-shot. */
    private volatile boolean holderSurfaceCreatedFired;

    /** v1.0.572: surface size recorded when the first macapkSurfaceChanged
     *  arrived before the holder dispatch could complete (no callbacks
     *  registered yet, or a callback rejected the delivery -- see
     *  macapkTryFireHolderCreated). macapkDrawFrame retries the deferred
     *  surfaceCreated dispatch each frame until it succeeds. */
    private int pendingSurfaceW;
    private int pendingSurfaceH;
    private long surfaceDeferLogCount;

    // MACAPK_TRACE_SURFACE_DISPATCH=1: name WHICH arm decided each dispatch.
    // Added for the v1.0.1135 BLANK(d=1) failure (~1 launch in 6), where MCPE
    // gains the graphics context only ONCE, never receives the second
    // macapkSurfaceChanged carrying the real ANativeWindow, and so never creates
    // its own EGL display -- our ANGLE path then presents an empty surface
    // forever at a healthy frame rate. See
    // docs/mcpe-blank-uniform-frame-surface-handover_2026-08-31.md.
    //
    // Logged ONLY on a TRANSITION of the (hasCallbacks, resumed, result) triple,
    // never per frame: this decision is evaluated on the Cocoa render thread
    // every frame, and the failure is an ordering RACE, so a per-frame log would
    // change the very timing it is meant to measure (this project has measured
    // that observer effect repeatedly -- heavy tracing took MCPE to 0/6 crosses).
    private static final boolean MACAPK_TRACE_DISPATCH =
        System.getenv("MACAPK_TRACE_SURFACE_DISPATCH") != null;
    private String macapkLastDispatchState;

    private boolean macapkHolderDispatchReady() {
        boolean cb = holder.hasCallbacks();
        boolean resumed;
        boolean res;
        if (cb) {
            resumed = false; res = true;
        } else {
            try {
                // ON: THIS view's activity window has been added (AOSP
                // mWindowAdded, latched at its first visible resume and kept
                // across a later pause), not "the top activity is resumed now".
                // Win.apk's own activity stack's "window added" / "top resumed" latches, which
                // AOSP's app layer does not set: the holder's callbacks decide.
                resumed = false;
                res = resumed;
            } catch (Throwable t) {
                resumed = false; res = true;   // harness/embedded runs without ActivityHost
            }
        }
        if (MACAPK_TRACE_DISPATCH) {
            String st = cb + "/" + resumed + "/" + res;
            if (!st.equals(macapkLastDispatchState)) {
                macapkLastDispatchState = st;
                NativeLog.i(TAG, "[surface-dispatch] ready? hasCallbacks=" + cb
                    + " resumed=" + resumed + " -> " + res
                    + " on " + getClass().getName());
            }
        }
        return res;
    }

    /** Attempt the one-shot surfaceCreated + mandatory first surfaceChanged
     *  dispatch, then window focus. Returns false WITHOUT latching when a
     *  callback rejected the delivery -- typically AGDK GameActivity's
     *  surfaceCreated calling onSurfaceCreatedNative before
     *  initializeNativeCode has registered the native methods
     *  (UnsatisfiedLinkError). The caller keeps the size pending and
     *  macapkDrawFrame retries next frame, converging as soon as the app's
     *  onCreate finishes registering. Real Android needs no retry because
     *  surfaceCreated is a post-onResume main-looper message; the retry
     *  reconstructs that guarantee under our concurrent dispatch. */
    /** v1.0.572: focus is pending until the activity's launch burst
     *  completes. Real Android delivers onWindowFocusChanged(true) after
     *  onResume (and after the surface callbacks); delivering it mid-
     *  onCreate reached Minecraft's MainActivity.onWindowFocusChanged
     *  before its onCreate had set `this.platform` -> NPE, and the once-
     *  guarded delivery latch would have consumed GAINED_FOCUS for the
     *  whole session (focus-gated native loops then never render). The
     *  surface-before-focus ordering Unity depends on is preserved: focus
     *  fires only after holderSurfaceCreatedFired. */
    private boolean pendingFocusDelivery;

    /** Per-frame log counters. These two branches run on EVERY frame for the
     *  whole life of the app, so an unconditional line here is ~60 lines/second
     *  of noise. MEASURED 2026-08-13 on Vector Pinball: 4,117 of 4,968 log lines
     *  (83%) were "focus-gate: not pending" -- enough to swamp
     *  debug/logdigest.py, which is what sessions read instead of the raw log.
     *  Rate-limited on the same 1 / 60 / every-600 cadence the deferred-surface
     *  logging in this file already uses: the first occurrences are the ones
     *  that carry information, and the periodic one proves it is still true. */
    private long focusNotPendingLogCount;
    private long focusDeferredLogCount;

    private static boolean macapkLogNth(long n) {
        return n == 1 || n == 60 || (n % 600) == 0;
    }

    private void macapkDeliverFocusIfResumed() {
        if (!pendingFocusDelivery) {
            if (macapkLogNth(++focusNotPendingLogCount)) {
                NativeLog.i(TAG, "focus-gate: not pending (already delivered or"
                    + " never armed) x" + focusNotPendingLogCount);
            }
            return;
        }
        // Window focus is the window manager's to give (AOSP's ViewRootImpl gets it with the
        // window); Win.apk's own delivery through its activity stack is gone.
        pendingFocusDelivery = false;
    }

    /** Converge the DEFERRED surfaceCreated dispatch and the window-focus
     *  delivery. Runs once per frame, and must run for EVERY SurfaceView --
     *  including one whose subclass shortcuts the draw path.
     *
     *  WHY IT IS A SEPARATE METHOD (v1.0.964). It used to be inlined in
     *  macapkDrawFrame, which made it collateral damage of an unrelated
     *  optimisation: GLSurfaceView returns early from BOTH macapkDrawFrame
     *  and macapkSurfaceChanged once it owns a Renderer, so from the frame
     *  the app called setRenderer() onwards, super was never reached again
     *  and the deferred delivery was orphaned FOREVER. On real Android the
     *  two things are unrelated -- window focus comes from ViewRootImpl and
     *  has nothing to do with who draws the frame -- so skipping it with the
     *  draw work is a lifecycle divergence, not an optimisation.
     *
     *  MEASURED on Vector Pinball (JDWP, 55s settled run): the first
     *  macapkSurfaceChanged landed mid-onCreate with zero holder callbacks
     *  and an unresumed activity, so it deferred; two frames later the app
     *  called setRenderer() and the base drawFrame stopped being reached.
     *  onWindowFocusChanged(true) was therefore NEVER delivered, so
     *  BouncyActivity.unpauseGame() -> FieldDriver.start() never ran, its
     *  simulation thread did not exist at all, and FieldViewManager's
     *  cachedScale/cachedHeight sat at their Java defaults of 0.0 -- which
     *  collapses every world coordinate onto one pixel and made the GL
     *  table draw 560 zero-area triangles. See
     *  docs/glsurfaceview-window-focus-never-delivered_2026-08-13.md.
     *
     *  `widthPx`/`heightPx` seed the pending size for a caller that reached
     *  us WITHOUT going through macapkSurfaceChanged's own deferral path
     *  (a subclass that installed its renderer before the first surface
     *  callback, so super.macapkSurfaceChanged never ran); pass 0 to mean
     *  "nothing new to record". */
    /** The last surface size macapkConvergeSurfaceLifecycle saw after the
     *  holder's surfaceCreated (0 = none yet). v1.0.1637. */
    private int macapkConvergedW, macapkConvergedH;

    public final void macapkConvergeSurfaceLifecycle(int widthPx, int heightPx) {
        // The renderer path (GLSurfaceView) never reaches macapkSurfaceChanged
        // below once it owns a Renderer, so until its native entry records the
        // display itself (macapkNoteNativeDisplaySize) this per-frame size is
        // the only one there is. Never after that: it is whatever the subclass
        // last stored, and a holder callback can store the view's own size
        // there (a GLSurfaceView registered as its own holder's Callback, as
        // AOSP GLSurfaceView.init does), which then sized the whole widget
        // tree from the GL view.
        if (!macapkDisplayFromNative) macapkNoteDisplaySize(widthPx, heightPx);
        if (!holderSurfaceCreatedFired) {
            if (pendingSurfaceW <= 0 && widthPx > 0 && heightPx > 0) {
                pendingSurfaceW = widthPx;
                pendingSurfaceH = heightPx;
            }
            // v1.0.572: deferred surfaceCreated retry. The first
            // macapkSurfaceChanged can arrive while the activity is still
            // inside onCreate (Cocoa render thread vs activity thread), before
            // the app registered its SurfaceHolder.Callback or its native
            // surface handlers. Retry each frame; macapkTryFireHolderCreated
            // refuses to latch until every callback accepts the delivery, so
            // this converges the moment the app's setup completes.
            if (pendingSurfaceW > 0
                    && macapkHolderDispatchReady()
                    && macapkTryFireHolderCreated(pendingSurfaceW, pendingSurfaceH)) {
                NativeLog.i(TAG, "deferred surfaceCreated fired on "
                    + getClass().getName() + " (listeners/native became ready)");
            }
        } else if (widthPx > 0 && heightPx > 0
                && (widthPx != macapkConvergedW || heightPx != macapkConvergedH)) {
            // v1.0.1637: a LATER size change -- a window resize -- on the
            // renderer path. GLSurfaceView.macapkSurfaceChanged drives its
            // Renderer directly and returns without super, so after the first
            // surface nothing re-fired the HOLDER: getSurfaceFrame() (which
            // sizes the ViewCanvasDriver widget overlay) and every app
            // SurfaceHolder.Callback kept the launch size forever. AOSP fires
            // surfaceChanged on every dimension change (SurfaceView
            // .updateSurface). Keyed on the SIZE THIS PATH LAST SAW, not on the
            // holder frame: an app's setFixedSize writes the frame too, and a
            // frame comparison would fight it every frame. Recorded before
            // calling out, so GLSurfaceView's own surfaceChanged ->
            // macapkSurfaceChanged re-entry sees an equal size and stops.
            macapkConvergedW = widthPx;
            macapkConvergedH = heightPx;
            if (!holder.macapkFixedSize) {
                // MACAPK_SV_VIEW_SIZE: the HOLDER's size for this display size,
                // which is the display size itself unless the view does not
                // fill its window (see macapkHolderSurfaceSize). Gate off:
                // exactly (widthPx, heightPx), as before.
                int[] s = macapkHolderSurfaceSize(widthPx, heightPx);
                holder.fireChanged(0, s[0], s[1]);
            }
        } else {
            macapkConvergeHolderToLayout();
        }
        // Focus deferred at surface-dispatch time (activity not yet resumed)
        // is delivered here once the launch burst completes.
        macapkDeliverFocusIfResumed();
    }

    // ---- SurfaceHolder.Callback runs on the UI thread (MACAPK_SV_UI_THREAD_CALLBACKS)
    //
    // AOSP delivers every SurfaceHolder.Callback from ViewRootImpl.performTraversals
    // -> SurfaceView.updateSurface, i.e. ON THE UI THREAD (android14 SurfaceView
    // .java updateSurface: "callbacks = getSurfaceCallbacks(); for (SurfaceHolder
    // .Callback c : callbacks) c.surfaceCreated(mSurfaceHolder);" inside the
    // traversal), after onResume. Ours arrive from the Cocoa render thread
    // (drawInMTKView -> macapkDrawFrame -> macapkSurfaceChanged), and code that
    // pins a thread identity at construction dies there. MEASURED on Organic
    // Maps (lanes19 media-playback, 2026-09-29): MapView's surfaceCreated ->
    // Framework::CreateDrapeEngine -> routing_session.cpp:163 IsActive():
    //     CHECK(m_threadChecker.CalledOnOriginalThread())
    // -> guest abort(6) with drawInMTKView at the bottom of the stack, because
    // the Framework was built on the Android main thread in Application.onCreate.
    // So when a holder dispatch starts OFF the UI thread while the real main loop
    // runs, the whole dispatch (surfaceCreated -> surfaceChanged -> the AGDK
    // global-layout kick -> focus, in that order) is POSTED to the UI thread
    // through an owner-only Handler, and later surfaceChanged/surfaceDestroyed
    // follow the same queue, so the AOSP order is kept. The retry contract of
    // fireCreatedTolerant is unchanged: a rejected delivery does not latch and
    // the next frame posts it again (one in flight at a time).
    private volatile boolean macapkCreatePosted;
    /** Native arrivals (macapkSurfaceChanged) that landed while the posted
     *  surfaceCreated was in flight, and the LATEST size among them. Guarded by
     *  macapkPostLock so "is a create in flight and not yet latched" and "record
     *  this arrival" are one step, against the posted runnable's "read what was
     *  missed and clear the in-flight flag". */
    private final Object macapkPostLock = new Object();
    private int macapkMissedArrivals;
    private int macapkMissedW, macapkMissedH;

    /** Called for every native arrival that reaches macapkSurfaceChanged before
     *  the holder latched. True when a posted create is in flight and this
     *  arrival was recorded for it (the runnable then delivers its
     *  surfaceChanged after created -> changed, in AOSP order); false when the
     *  caller must handle it as before (nothing in flight, or already latched). */
    private boolean macapkRecordArrivalDuringPostedCreate(int widthPx, int heightPx) {
        synchronized (macapkPostLock) {
            if (!macapkCreatePosted || holderSurfaceCreatedFired) return false;
            macapkMissedArrivals++;
            macapkMissedW = widthPx;
            macapkMissedH = heightPx;
            return true;
        }
    }

    static boolean macapkCallbackOffUiThread() {
        return android.os.Looper.macapkRealMainLoopRunning()
            && !android.os.Looper.macapkIsActivityThread();
    }
    static void macapkPostToUiThread(Runnable r) {
        android.os.Handler.macapkCreateOwnerOnly(android.os.Looper.getMainLooper()).post(r);
    }

    private boolean macapkTryFireHolderCreated(int widthPx, int heightPx) {
        if (macapkCallbackOffUiThread()) {
            // Not latched here: the posted dispatch latches when every callback
            // accepted it; until then each frame's retry sees the flag and waits.
            synchronized (macapkPostLock) {
                if (macapkCreatePosted) return false;
                macapkCreatePosted = true;
                macapkMissedArrivals = 0;
            }
            macapkPostToUiThread(() -> {
                try {
                    if (!holderSurfaceCreatedFired
                            && macapkTryFireHolderCreatedHere(widthPx, heightPx)) {
                        NativeLog.i(TAG, "surfaceCreated fired on the UI thread on "
                            + getClass().getName() + " (" + widthPx + "x" + heightPx + ")");
                        macapkDeliverMissedArrivals();
                    }
                } catch (Throwable t) {
                    NativeLog.e(TAG, "UI-thread surfaceCreated dispatch threw: " + t);
                } finally {
                    synchronized (macapkPostLock) {
                        macapkCreatePosted = false;
                        macapkMissedArrivals = 0;
                    }
                }
            });
            return false;
        }
        return macapkTryFireHolderCreatedHere(widthPx, heightPx);
    }

    /** Runs on the UI thread right after the posted create latched
     *  (created -> changed(first arrival) -> global-layout -> focus). Every native
     *  arrival recorded while it was in flight fires its own surfaceChanged now,
     *  at the latest recorded size, once per arrival: the inline path fires one
     *  changed per arrival (MCPE's handover depends on the SECOND arrival
     *  reaching its holder), so the posted path must not fold them into one or
     *  lose them. An arrival after the latch takes the fired-branch of
     *  macapkSurfaceChanged instead (fireChanged posts behind this runnable),
     *  so no arrival is counted twice. */
    private void macapkDeliverMissedArrivals() {
        int n, w, h;
        synchronized (macapkPostLock) {
            n = macapkMissedArrivals; w = macapkMissedW; h = macapkMissedH;
            macapkMissedArrivals = 0;
        }
        for (int i = 0; i < n; i++) {
            int[] s = macapkHolderSurfaceSize(w, h);
            holder.fireChanged(0, s[0], s[1]);
        }
        if (n > 0) {
            NativeLog.i(TAG, "UI-thread dispatch: " + n + " native arrival(s) during the posted"
                + " surfaceCreated delivered as surfaceChanged " + w + "x" + h);
        }
    }

    private boolean macapkTryFireHolderCreatedHere(int widthPx, int heightPx) {
        if (!holder.fireCreatedTolerant()) return false;
        holderSurfaceCreatedFired = true;
        pendingSurfaceW = 0;
        pendingSurfaceH = 0;
        macapkConvergedW = widthPx;   // the resize re-fire's baseline (v1.0.1637)
        macapkConvergedH = heightPx;
        // MACAPK_SV_VIEW_SIZE: the view's laid-out size when it does not fill
        // its window (AOSP updateSurface :985-988); the display size otherwise
        // and with the gate off, as before.
        int[] firstSize = macapkHolderSurfaceSize(widthPx, heightPx);
        holder.fireChanged(0, firstSize[0], firstSize[1]);
        // v1.0.575: AGDK content-rect delivery. GameActivity registers an
        // OnGlobalLayoutListener on THIS SurfaceView's ViewTreeObserver (in
        // onCreate); its onGlobalLayout reads getWidth()/getHeight() +
        // getLocationInWindow and calls onContentRectChangedNative(handle,x,y,w,h)
        // when the rect changes from its cached (0,0,0,0). That command
        // (APP_CMD_CONTENT_RECT_CHANGED) is what android_main / RenderDragon needs
        // to size its viewport and build the main-menu scene; without it the game
        // parks after its loading frames (cohtml TaskChannel idle downstream).
        // ViewCanvasDriver dispatches global-layout on the ROOT observer per frame
        // (and View.macapkDispatchAttachedToWindow now merges this view's floating
        // observer into the root), but drive it directly here too so the listener
        // fires ONCE with a real rect the instant the surface goes live -- not only
        // on some later incidental requestLayout. Lay the view out to the real
        // (pixel) surface size first so the rect is non-zero (getWidth()==right-left).
        // Generic: any SurfaceView whose consumer watches global-layout benefits;
        // engines that ignore it (cocos2d-x GLSurfaceView) are unaffected.
        try {
            if (getWidth() == 0 || getHeight() == 0) {
                // Not a layout pass: the size snapshot (setFrame below) must
                // not read this as where the tree placed the view.
                macapkSelfLayout = true;
                try { layout(0, 0, widthPx, heightPx); }
                finally { macapkSelfLayout = false; }
            }
            android.view.ViewTreeObserver vto = getViewTreeObserver();
            if (vto != null) vto.dispatchOnGlobalLayout();
        } catch (Throwable t) {
            NativeLog.e(TAG, "content-rect global-layout dispatch threw: " + t);
        }
        pendingFocusDelivery = true;
        macapkDeliverFocusIfResumed();
        return true;
    }

    /** v1.0.70: process-wide overlay driver. When an Activity's
     *  contentView contains BOTH a SurfaceView descendant AND plain
     *  widgets (Buttons, TextViews, etc.) as siblings (Bouncy: GL
     *  field view + button panel; AndEngine: rendering view +
     *  HUD), the SurfaceView's MTKView handles GL but the plain
     *  widgets need a separate Canvas paint pass. Activity.set-
     *  ContentView wires that here by calling
     *  macapkSetOverlayDriver; every SurfaceView instance then
     *  runs the overlay's paint after its own renderer dispatch.
     *  The overlay is process-wide (static) because there's only
     *  one active Activity content view at a time, and the
     *  SurfaceView class doesn't know which Activity owns it. */
    private static MetalSurfaceCallbacks staticOverlayDriver;
    private static int staticOverlayW;
    private static int staticOverlayH;
    private static boolean staticOverlaySurfaceCreatedFired;

    /** Called from Activity.setContentView when a Canvas overlay is
     *  needed on top of an existing GL view. Wraps the driver's
     *  macapkSurfaceChanged once (when first set) and lets each
     *  SurfaceView pull it on every macapkDrawFrame. */
    public static void macapkSetOverlayDriver(MetalSurfaceCallbacks driver,
                                                  int widthPx, int heightPx) {
        staticOverlayDriver = driver;
        staticOverlayW = widthPx > 0 ? widthPx : 800;
        staticOverlayH = heightPx > 0 ? heightPx : 600;
        staticOverlaySurfaceCreatedFired = false;
        if (driver != null) {
            final MetalSurfaceCallbacks d = driver;
            final Runnable fire = () -> {
                if (staticOverlayDriver != d) return;   // superseded before the post ran
                if (staticOverlaySurfaceCreatedFired) return;   // macapkDriveOverlay already fired it
                try {
                    // v1.0.1636: send the CURRENT size, not the size captured
                    // when this runnable was created. macapkDriveOverlay may have
                    // recorded the real surface size (808x1796) between the post
                    // and the run, while the captured guess was the stale
                    // phoneWidth*2 (836x1860); firing the guess left the overlay
                    // permanently at the wrong size (the "self-corrects" note at
                    // the demote path was false for this path).
                    final int w = staticOverlayW, h = staticOverlayH;
                    d.macapkSurfaceChanged(w, h);
                    staticOverlaySurfaceCreatedFired = true;
                    NativeLog.i(TAG, "overlay driver registered " + w + "x" + h);
                } catch (Throwable t) {
                    NativeLog.e(TAG, "overlay driver surfaceChanged threw: " + t);
                }
            };
            // v1.0.997 rewire hardening (2026-08-18): this is called from
            // Activity.setContentView AND from the rewire's demote path, both
            // of which can sit INSIDE a ViewGroup.addView -> measure/layout
            // dispatch on the activity loop. ViewCanvasDriver.macapkSurface-
            // Changed only self-posts when called OFF the loop; called ON the
            // loop it ran INLINE, which (a) re-entered measure -- Compose
            // rejects it with "performMeasureAndLayout called during measure
            // layout" -- and (b) nulled the driver's frame bitmap out from
            // under the doFrame that was mid-draw (the doFrame NPE this
            // session's independent lane caught). Posting defers it until the
            // current dispatch unwinds; if the post is beaten by a newer
            // registration, macapkDriveOverlay's created-fired backstop still
            // fires it with the then-current size.
            if (android.os.Looper.macapkIsActivityThread()) {
                // OWNER-ONLY (v1.0.1636): fire calls d.macapkSurfaceChanged,
                // which nulls the driver's frame bitmap. It must run on the
                // owner thread, never on the OS-main drain concurrently with a
                // doFrame -- so post it through an owner-only Handler.
                android.os.Handler.macapkCreateOwnerOnly(
                    android.os.Looper.getMainLooper()).post(fire);
            } else {
                fire.run();
            }
        }
    }

    /** Drive the overlay driver's draw + surfaceChanged-on-resize from
     *  any SurfaceView's macapkDrawFrame. Cheap when no driver is
     *  set. */
    /**
     * Derive THIS surface's real pixel size and drive the ViewCanvasDriver
     * overlay with it.
     *
     * WHY THIS IS FACTORED OUT AND `protected` (v1.0.835). The size derivation
     * below is drift-prone -- see the long comment in macapkDrawFrame about
     * points-vs-pixels re-firing macapkSurfaceChanged at the wrong size and
     * corrupting an engine's viewport into the bottom-left corner. It must
     * therefore exist exactly ONCE.
     *
     * (Public since GLSurfaceView reaches it, and the other GL hooks it calls,
     * through com.macapk.loader.view.ViewHost; still final.)
     *
     * It needs to be reachable from GLSurfaceView because of the two-tier
     * renderer dispatch: when a subclass DID call setRenderer(),
     * GLSurfaceView.macapkDrawFrame handles the frame itself and returns
     * WITHOUT calling super, so SurfaceView's prologue -- and with it this
     * overlay drive -- never ran. It composited WindowManager overlays
     * (Dialog/PopupWindow) on that path but not the ViewCanvasDriver overlay,
     * so an app whose real UI is ordinary widgets SHARING a ViewGroup with the
     * GL surface drew nothing at all.
     *
     * Measured (2026-07-29): `grep -c "ViewCanvasDriver.*fire #"` across one
     * census gave sgtpuzzles 9 (overlay driven), bouncy 0, jetpackjoyride 0 --
     * three apps, one variable. Bouncy's GL tier was provably healthy the whole
     * time: 1,800+ frames, glViewport(0,0,808,1796), 3,600 cumulative
     * glDrawElements, err=0x0 throughout.
     *
     * AOSP contract this restores, from the SurfaceView javadoc: "The view
     * hierarchy will take care of correctly compositing ... any siblings ...
     * that would normally appear on top of it."
     */
    public final int[] macapkDriveOverlayForCurrentSurface() {
        int overlayW, overlayH;
        if (macapkDisplayDecoupled()) {
            // MACAPK_SV_DISPLAY_SIZE (2026-09-26): the overlay is the WINDOW's
            // view tree, so it is sized from the DISPLAY surface this view was
            // last given, never from the holder's frame. That frame is the
            // surface BUFFER: an app's setFixedSize writes it (AOSP
            // setFixedSize :1589-1600 only changes mRequestedWidth/Height,
            // and the window keeps its size), and under MACAPK_SV_VIEW_SIZE it
            // is the view's own size. Measured on Super Jetpack
            // (docs/fleet-unity-constant-gaps_2026-09-26.md §4 step 6):
            // setFixedSize(40,40) laid the whole widget tree out at 40x40.
            int[] d = macapkKnownDisplaySize();
            overlayW = d[0];
            overlayH = d[1];
        } else {
            android.graphics.Rect ownFrame = getHolder().getSurfaceFrame();
            overlayW = ownFrame.width();
            overlayH = ownFrame.height();
            if (overlayW <= 0 || overlayH <= 0) {
                // v1.0.1637: the committed display (a window drag never updates
                // the canvas globals this used to read).
                overlayW = Display.macapkRealWidthPx();
                overlayH = Display.macapkRealHeightPx();
            }
        }
        macapkDriveOverlay(overlayW, overlayH);
        // Returned so the caller can reuse the SAME numbers for the
        // WindowManager overlay instead of re-deriving them.
        return new int[] { overlayW, overlayH };
    }

    private static void macapkDriveOverlay(int currentW, int currentH) {
        MetalSurfaceCallbacks d = staticOverlayDriver;
        if (d == null) return;
        if (!staticOverlaySurfaceCreatedFired
            || currentW != staticOverlayW
            || currentH != staticOverlayH) {
            try {
                staticOverlayW = currentW;
                staticOverlayH = currentH;
                d.macapkSurfaceChanged(currentW, currentH);
                staticOverlaySurfaceCreatedFired = true;
            } catch (Throwable t) {
                NativeLog.e(TAG, "overlay driver surfaceChanged threw: " + t);
            }
        }
        try {
            d.macapkDrawFrame();
        } catch (Throwable t) {
            NativeLog.e(TAG, "overlay driver drawFrame threw: " + t);
        }
    }

    // ======================================================================
    //  Surface GEOMETRY (2026-09-26): which size a surface is given, and where
    //  it is shown. Research: docs/surfaceview-surface-geometry_2026-09-26.md.
    // ======================================================================
    //
    // AOSP (android14-release core/java/android/view/SurfaceView.java):
    //   updateSurface() :985-988
    //       int myWidth = mRequestedWidth;
    //       if (myWidth <= 0) myWidth = getWidth();
    //       int myHeight = mRequestedHeight;
    //       if (myHeight <= 0) myHeight = getHeight();
    //   :1120  c.surfaceChanged(mSurfaceHolder, mFormat, myWidth, myHeight);
    //   and the compositor shows that buffer at mScreenRect, the view's own
    //   window rect (:1040-1043), BELOW the window (mSubLayer =
    //   APPLICATION_MEDIA_SUBLAYER, :200) through a hole draw() punches
    //   (:512-521, clearSurfaceViewPort :579-597). setFrame() is overridden
    //   (:478-482: super.setFrame(...); updateSurface();), so every layout
    //   pass re-evaluates the size.
    //
    // Ours handed the holder the DISPLAY surface (the native drawable) and a
    // renderer the WINDOW box (StatusBarDriver.contentBox*), whatever the
    // view's own size, and a lockCanvas frame was presented over the whole
    // window. Measured (docs/fleet-misc-shatteredpd-retrowars-bobball-bouncy-
    // unciv_2026-09-26.md §1): BobBall's SurfaceView is laid out 3360x1560
    // between two HUD rows but was handed 3360x1636 (board 1.049x too big),
    // and bouncy's GLFieldView is 1226x1524 under a 112px ScoreView but its
    // renderer got 1226x1636 (half-dome bumpers, table 1.074x too big).
    //
    // WHAT CHANGES, and the boundary kept on purpose:
    //  * A view that FILLS its window (laid-out size == the root's) keeps the
    //    pre-change answer exactly. Every native consumer we have (Unity,
    //    AGDK/MCPE, native EGL through ANativeWindow, the ANGLE layer) is built
    //    at that size, so changing it there would split the holder from the
    //    layer it describes. That population (HCR, Unity, MCPE, cocos,
    //    fullscreen Canvas games) is untouched by construction. The residual
    //    AOSP gap -- a filling view in a window with VISIBLE bars is told the
    //    display, AOSP tells it the content box -- is OPEN and belongs with
    //    the native layers.
    //  * A view that does NOT fill its window gets its own laid-out size, as
    //    AOSP does: the holder once it has served a lockCanvas (its frame is
    //    then presented AT the view's rect, see macapkPlaceInDisplay /
    //    macapkPresentThroughSurfaceLayer) and, before that, by the renderer
    //    rule, because its consumer may be GL (macapkHolderSurfaceSize); a
    //    renderer only while the view's
    //    bottom-left corner is the ANGLE layer's. The one GL layer is the
    //    window box and GL's origin is its bottom-left, so glViewport(0, 0,
    //    w, h) then lands exactly on the view (bouncy's shape). A GL view
    //    placed anywhere else (a banner BELOW it) keeps the window box until
    //    the ANGLE layer can follow the view's rect natively: told its real
    //    size it would draw shifted, which is worse than drawing too big.
    //  * setFixedSize is left exactly as it was. AOSP gives the buffer the
    //    fixed size and SCALES it to the view; our native layers cannot scale
    //    a fixed buffer, and Unity requests one -- a holder that reports it
    //    while Unity's layer stays display-sized would render into a corner.
    //    OPEN, with the native layers.
    //
    // The view's size is read from a SNAPSHOT published by setFrame on the
    // thread that lays the tree out, never by calling getWidth() on the render
    // thread: a read racing a relayout could see the new root with the old
    // child, flip a filling view to "not filling" for one frame and hand a
    // latching engine (cocos) a wrong size -- the v1.0.742 failure shape
    // StatusBarDriver.contentBoxHeightPx's comment warns about.

    private static boolean macapkDisplayDecoupled() {
        return true;
    }

    /** The DISPLAY surface (native drawable) this view was last handed:
     *  (w << 32) | h, 0 = none yet. ONE volatile word, so a reader on another
     *  thread (a lockCanvas drawing thread, the Cocoa rect pull) never pairs a
     *  new width with an old height across a resize. Written by the native
     *  entry (macapkSurfaceChanged, through macapkNoteNativeDisplaySize) and,
     *  only until it has run, by
     *  macapkConvergeSurfaceLifecycle. */
    private volatile long macapkDisplaySize;
    /** A native entry has recorded the display: the per-frame converge size is
     *  no longer taken as the display (see macapkConvergeSurfaceLifecycle). */
    private volatile boolean macapkDisplayFromNative;

    private void macapkNoteDisplaySize(int w, int h) {
        if (w > 0 && h > 0) macapkDisplaySize = ((long) w << 32) | (h & 0xffffffffL);
    }

    /** Record the display size where the NATIVE display arrives. A subclass
     *  that handles the native macapkSurfaceChanged without calling super
     *  (GLSurfaceView with a Renderer) calls this first, before anything it
     *  does can re-enter with a HOLDER's size, which under MACAPK_SV_VIEW_SIZE
     *  is the view's own size and not the window's. */
    public final void macapkNoteNativeDisplaySize(int w, int h) {
        if (w > 0 && h > 0) {
            macapkNoteDisplaySize(w, h);
            macapkDisplayFromNative = true;
        }
    }

    /** The recorded display {w, h}, or null when none has been recorded. */
    private int[] macapkNotedDisplaySize() {
        long p = macapkDisplaySize;
        if (p == 0L) return null;
        return new int[] { (int) (p >>> 32), (int) p };
    }

    /** The display size to use when this view has not been handed one yet:
     *  the committed display (the same fallback the overlay drive used). */
    private int[] macapkKnownDisplaySize() {
        int[] d = macapkNotedDisplaySize();
        if (d != null) return d;
        return new int[] { Display.macapkRealWidthPx(), Display.macapkRealHeightPx() };
    }

    /** Where the last LAYOUT pass put this view, relative to its window's
     *  root. Immutable, published whole through a volatile field, so the
     *  render thread never sees half of a relayout. */
    private static final class LayoutSnap {
        final int w, h, rootW, rootH, x, y;
        LayoutSnap(int w, int h, int rootW, int rootH, int x, int y) {
            this.w = w; this.h = h; this.rootW = rootW; this.rootH = rootH;
            this.x = x; this.y = y;
        }
    }
    private volatile LayoutSnap macapkLayoutSnap;
    /** True while macapkTryFireHolderCreated lays the view out itself (the
     *  AGDK content-rect kick): that is not the tree placing it. */
    private boolean macapkSelfLayout;

    /** AOSP SurfaceView overrides setFrame (android14 :478-482) to run
     *  updateSurface() after every layout pass; this is the same hook,
     *  publishing where the pass put the view. The parent chain is already
     *  placed when a child's setFrame runs (layout is top-down), so the root's
     *  size and this view's offset in it are final here. */
    @Override
    protected boolean setFrame(int left, int top, int right, int bottom) {
        boolean changed = super.setFrame(left, top, right, bottom);
        macapkSnapLayout(right - left, bottom - top);
        return changed;
    }

    private void macapkSnapLayout(int w, int h) {
        try {
            View root = getRootView();
            if (macapkSelfLayout || getParent() == null || root == null || root == this) {
                if (!macapkSelfLayout) macapkLayoutSnap = null;
                return;
            }
            // The offset in the root by walking the parents, NOT
            // getLocationInWindow: that answers 0,0 for a view that is not
            // attached yet (AOSP transformFromViewToWindowSpace's first branch),
            // and a tree may be laid out before it is attached.
            int x = 0, y = 0;
            View v = this;
            while (v != null && v != root) {
                x += v.getLeft();
                y += v.getTop();
                ViewParent p = v.getParent();
                if (!(p instanceof View)) break;
                View pv = (View) p;
                x -= pv.getScrollX();
                y -= pv.getScrollY();
                v = pv;
            }
            macapkLayoutSnap = new LayoutSnap(w, h, root.getWidth(), root.getHeight(), x, y); /*SNAP-OFFSET*/
        } catch (Throwable t) {
            macapkLayoutSnap = null;   // a half-built tree: fall back to the legacy size
        }
    }

    /** The view's laid-out size when the tree placed it and it does NOT fill
     *  its window; null otherwise (= keep the legacy answer). {@code gl} adds
     *  the single-ANGLE-layer constraint described in the section comment. */
    private int[] macapkNonFillingViewSize(int displayW, int displayH, boolean gl) {
        LayoutSnap s = macapkLayoutSnap;
        if (s == null || !macapkAttached) return null;
        if (s.w <= 0 || s.h <= 0 || s.rootW <= 0 || s.rootH <= 0) return null;
        if (s.w == s.rootW && s.h == s.rootH) return null;          /*FILLS*/
        if (gl) {
            if (s.w > s.rootW || s.h > s.rootH) return null;
            // The ANGLE layer is the window box; the root must BE that box for
            // "the root's bottom-left" to be GL's origin.
            int boxW = com.macapk.loader.StatusBarDriver.contentBoxWidthPx(this, displayW);
            int boxH = com.macapk.loader.StatusBarDriver.contentBoxHeightPx(this, displayH);
            if (s.rootW != boxW || s.rootH != boxH) return null;
            if (s.x != 0 || s.y + s.h != s.rootH) return null;       /*GL-ANCHOR*/
        }
        return new int[] { s.w, s.h };
    }

    /** The size the HOLDER reports (surfaceChanged, getSurfaceFrame,
     *  lockCanvas) for a display surface of displayW x displayH.
     *
     *  WHICH RULE, and why it depends on lockCanvas. A holder does not say who
     *  consumes it. A Canvas consumer's frame is shown AT the view's rect (see
     *  "where a lockCanvas frame is shown"), so it can take the view's size
     *  wherever the view is. A GL/EGL consumer -- a forked GLThread, SDL, a
     *  native EGL engine reaching the surface through ANativeWindow -- draws
     *  into the ONE window-box GL layer, exactly like a renderer, so it can
     *  only take the view's size where a renderer can (the view on the GL
     *  layer's bottom-left corner); anywhere else it would draw shifted inside
     *  the window, which is worse than drawing too big (the renderer rule in
     *  the section comment). Corpus consumers of that shape include HCR's
     *  FSGLSurfaceView, Super Jetpack's GLSurfaceViewMortar, OpenTTD's
     *  GLSurfaceView_SDL, klooni's GLSurfaceViewAPI18, Wikipedia's
     *  MapLibreSurfaceView and several SDLSurface apps. So until this holder
     *  has served a lockCanvas it answers by the GL rule, and from the first
     *  lockCanvas on by the Canvas rule; macapkConvergeHolderToLayout
     *  re-delivers the size the frame after the rule changes. The first answer
     *  a Canvas app gets is therefore the GL rule's, which for a view off the
     *  GL corner is the pre-change display size, and it is corrected after its
     *  first frame (BobBall: 3360x1636, then 3360x1560). */
    private int[] macapkHolderSurfaceSize(int displayW, int displayH) {
        if (!holder.macapkFixedSize) {
            boolean gl = !holder.macapkLockedCanvas;                   /*HOLDER-RULE*/
            int[] v = macapkNonFillingViewSize(displayW, displayH, gl);
            if (v == null && gl) v = macapkOversizeUnclaimedViewSize();
            if (v != null) return v;
        }
        return new int[] { displayW, displayH };
    }

    // ---- a holder nobody draws into yet, on a view larger than its window ------
    //
    // A "center crop" preview lays its SurfaceView out LARGER than the window
    // (negative margins) so the frame's aspect ratio survives, then waits for
    // surfaceChanged to report that size before it starts producing frames:
    // AOSP always sizes the surface from the view (updateSurface :985-988). The
    // GL rule above refuses an overflowing view, so such a holder kept the
    // window size -- and because its producer (a camera1 preview drawing with
    // lockCanvas, say) only starts after that surfaceChanged, the Canvas rule
    // never took over either: the preview never started (QR Bot, 2026-10-28).
    //
    // The GL rule protects a GL/EGL consumer; a view with a resolved renderer, a
    // GLSurfaceView, or the native-window owner (ANativeWindow_fromSurface) is
    // one, and keeps it. A holder with none of those has no GL consumer the
    // window-box layer could misplace, so it gets the view's size, as on AOSP.
    //
    // Always on: its bisect switch (MACAPK_SV_OVERSIZE_HOLDER=0, the window size
    // for these holders) is gone, as Win.apk has no switches.
    static final boolean sOversizeHolderGate = true;

    private int[] macapkOversizeUnclaimedViewSize() {
        if (!sOversizeHolderGate) return null;
        LayoutSnap s = macapkLayoutSnap;
        if (s == null || !macapkAttached) return null;
        if (s.w <= 0 || s.h <= 0 || s.rootW <= 0 || s.rootH <= 0) return null;
        if (s.w <= s.rootW && s.h <= s.rootH) return null;           // not overflowing: the GL rule's call
        if (s.w > 16384 || s.h > 16384) return null;
        if (this instanceof android.opengl.GLSurfaceView) return null;
        java.lang.ref.WeakReference<SurfaceView> owner = sNativeWindowOwner;
        if (owner != null && owner.get() == this) return null;
        return new int[] { s.w, s.h };
    }

    /** Layout moved a created surface's size (AOSP: setFrame -> updateSurface
     *  -> sizeChanged -> surfaceChanged). Every frame; one comparison unless
     *  the answer changed. Never for a fixed-size holder (left as before). */
    private void macapkConvergeHolderToLayout() {
        if (holder.macapkFixedSize) return;
        int[] d = macapkNotedDisplaySize();
        if (d == null) return;
        int dW = d[0], dH = d[1];
        int[] s = macapkHolderSurfaceSize(dW, dH);
        android.graphics.Rect f = holder.getSurfaceFrame();
        if (s[0] == f.width() && s[1] == f.height()) return;
        NativeLog.i(TAG, "surface follows layout: " + f.width() + "x" + f.height()
            + " -> " + s[0] + "x" + s[1] + " (display " + dW + "x" + dH + ") on "
            + getClass().getName());
        holder.fireChanged(0, s[0], s[1]);
    }

    /** This view's top-left in DISPLAY-surface pixels -- the space the
     *  ViewCanvasDriver canvas, and so the canvas overlay, lives in. ONE
     *  formula for the video plane (macapkActiveVideoRectPacked) and the
     *  placement of a lockCanvas surface, so a hole and its plane cannot drift
     *  apart; the v1.0.997 correction it carries is documented there. */
    private void macapkWindowOrigin(int[] out) {
        int[] loc = new int[2];
        getLocationInWindow(loc);
        View root = getRootView();
        int rootLeft = root != null ? root.getLeft() : 0;
        int rootTop  = root != null ? root.getTop()  : 0;
        out[0] = loc[0] - rootLeft;
        out[1] = loc[1] - rootTop + com.macapk.loader.StatusBarDriver.heightPx(root);
    }

    // ---- where a lockCanvas frame is shown -----------------------------------
    //
    // Two routes, best first:
    //  (1) the runtime's PER-SURFACE layer (MACAPK_SV_SURFACE_LAYER): the
    //      frame goes to a CALayer of its own, at the view's rect, BELOW the
    //      view-tree overlay, and draw() punches the hole AOSP punches. This is
    //      what lets a HUD drawn by sibling views show over the game
    //      (BobBall's "Time Left" / "Score" rows, F2 of the fleet note); on the
    //      shared overlay layer the game thread's frame replaces the view
    //      tree's, last writer wins. Resolved by a capability probe
    //      (NativeCocoa.presentSurfaceBitmap), so this file builds and runs
    //      the same before and after the native half lands.
    //  (2) otherwise, a surface that is NOT display-sized (a non-filling view
    //      under MACAPK_SV_VIEW_SIZE) is composited into a display-sized frame
    //      at its rect before the legacy whole-window present -- which
    //      stretches whatever it is given over the window
    //      (kCAGravityResize), so a view-sized frame handed over as-is would
    //      be stretched anisotropically. Outside the rect the frame is
    //      transparent. The HUD still loses to the game's frame on this route.
    //  Anything else takes the legacy present unchanged.

    private static final java.util.concurrent.atomic.AtomicInteger sSurfaceKeySeq =
        new java.util.concurrent.atomic.AtomicInteger();
    private static final java.util.concurrent.ConcurrentHashMap<Integer,
            java.lang.ref.WeakReference<SurfaceView>> sSurfaceByKey =
        new java.util.concurrent.ConcurrentHashMap<>();
    private volatile int macapkSurfaceKey;
    /** AOSP mDrawFinished: a frame of this surface has been shown, so draw()
     *  now punches its hole (and a first frame asked the tree to redraw).
     *  STICKY on purpose, as AOSP's is: android14 SurfaceView.java writes
     *  mDrawFinished only at its declaration (:177, false) and in
     *  performDrawFinished (:427, true) -- neither onDetachedFromWindow
     *  (:435-461) nor either surface teardown in updateSurface (:975,
     *  :1088-1095) clears it. A re-attached view punches its hole at once; ours then
     *  shows the layer's last frame (the rect pull hid it while detached,
     *  and hiding keeps its contents) until the next present. */
    private volatile boolean macapkSurfaceLayerDrawn;

    private int macapkSurfaceKey() {
        int k = macapkSurfaceKey;
        if (k != 0) return k;
        synchronized (sSurfaceByKey) {
            k = macapkSurfaceKey;
            if (k == 0) {
                k = sSurfaceKeySeq.incrementAndGet();
                sSurfaceByKey.put(k, new java.lang.ref.WeakReference<>(this));
                macapkSurfaceKey = k;
            }
        }
        return k;
    }

    /** 0 = not probed, 1 = the runtime has a per-surface layer, -1 = it does not. */
    private static volatile int sSurfaceLayerState;
    private static volatile java.lang.invoke.MethodHandle sSurfaceLayerMH;

    private static java.lang.invoke.MethodHandle macapkSurfaceLayerPresenter() {
        int st = sSurfaceLayerState;
        if (st == 1) return sSurfaceLayerMH;
        if (st < 0) return null;
        try {
            java.lang.invoke.MethodHandle mh = java.lang.invoke.MethodHandles.publicLookup()
                .findStatic(NativeCocoa.class, "presentSurfaceBitmap",
                    java.lang.invoke.MethodType.methodType(void.class,
                        int.class, int[].class, int.class, int.class));
            sSurfaceLayerMH = mh;
            sSurfaceLayerState = 1;
            return mh;
        } catch (Throwable t) {
            sSurfaceLayerState = -1;
            NativeLog.i(TAG, "per-surface layer: not in this runtime (" + t.getClass().getSimpleName()
                + "); lockCanvas frames use the shared overlay");
            return null;
        }
    }

    /** Route (1). False = not available (or it failed): take route (2). */
    boolean macapkPresentThroughSurfaceLayer(int[] px, int w, int h) {
        java.lang.invoke.MethodHandle mh = macapkSurfaceLayerPresenter();
        if (mh == null) return false;
        int key = macapkSurfaceKey();
        try {
            mh.invokeExact(key, px, w, h);
        } catch (UnsatisfiedLinkError e) {
            // Declared but not registered: a runtime binary older than its jar.
            sSurfaceLayerState = -1;
            NativeLog.w(TAG, "per-surface layer: presentSurfaceBitmap not registered ("
                + e.getMessage() + "); lockCanvas frames use the shared overlay");
            return false;
        } catch (Throwable t) {
            NativeLog.w(TAG, "per-surface layer present threw: " + t);
            return false;
        }
        if (!macapkSurfaceLayerDrawn) {
            macapkSurfaceLayerDrawn = true;
            NativeLog.i(TAG, "per-surface layer: first frame " + w + "x" + h + " (key " + key
                + ") of " + getClass().getName() + "; the view tree now punches its hole");
            // The tree's current recording predates the hole: re-record it.
            try { postInvalidate(); } catch (Throwable ignored) { }
        }
        return true;
    }

    /** Route (2)'s frame, reused across frames (drawing thread only). */
    private int[] macapkCompositeBuf;
    int macapkCompositeW, macapkCompositeH;
    private int macapkPlacedX = Integer.MIN_VALUE, macapkPlacedY, macapkPlacedW, macapkPlacedH;

    /** Route (2): the display-sized frame to present instead of {@code px}, or
     *  null to present {@code px} the legacy way. Drawing thread. */
    int[] macapkPlaceInDisplay(int[] px, int bw, int bh) {
        if (px == null) return null;
        int[] d = macapkNotedDisplaySize();
        if (d == null) return null;
        int dW = d[0], dH = d[1];
        if (bw == dW && bh == dH) return null;           // display-sized: legacy IS its placement
        if (holder.macapkFixedSize || !macapkAttached) return null;
        LayoutSnap s = macapkLayoutSnap;
        if (s == null || s.w != bw || s.h != bh) return null;   // not (yet) the view's own size
        if ((long) dW * dH > Integer.MAX_VALUE || px.length < bw * bh) return null;
        int[] o = new int[2];
        try { macapkWindowOrigin(o); } catch (Throwable t) { return null; }
        int x = o[0], y = o[1];
        int[] buf = macapkCompositeBuf;
        if (buf == null || macapkCompositeW != dW || macapkCompositeH != dH) {
            buf = new int[dW * dH];
            macapkCompositeBuf = buf;
            macapkCompositeW = dW;
            macapkCompositeH = dH;
            macapkPlacedX = Integer.MIN_VALUE;
        } else if (x != macapkPlacedX || y != macapkPlacedY
                || bw != macapkPlacedW || bh != macapkPlacedH) {
            java.util.Arrays.fill(buf, 0);                // the old rect is stale
        }
        if (x != macapkPlacedX || y != macapkPlacedY || bw != macapkPlacedW || bh != macapkPlacedH) {
            NativeLog.i(TAG, "lockCanvas frame placed at " + x + "," + y + " " + bw + "x" + bh
                + " in the " + dW + "x" + dH + " display (no per-surface layer: composited"
                + " into the shared overlay) on " + getClass().getName());
        }
        macapkPlacedX = x; macapkPlacedY = y; macapkPlacedW = bw; macapkPlacedH = bh;
        int x0 = Math.max(0, x), x1 = Math.min(dW, x + bw);
        int y0 = Math.max(0, y), y1 = Math.min(dH, y + bh);
        if (x0 < x1 && y0 < y1) {
            int n = x1 - x0;
            for (int row = y0; row < y1; row++) {
                System.arraycopy(px, (row - y) * bw + (x0 - x), buf, row * dW + x0, n);
            }
        }
        return buf;
    }

    /** Native pull for the per-surface layer (Cocoa main thread, per present
     *  and per tick while the layer exists), same packing and sentinels as
     *  macapkActiveVideoRectPacked: (x << 48) | (y << 32) | (w << 16) | h in
     *  DISPLAY-surface pixels, x/y signed; 0 = no such surface any more (drop
     *  the layer); -1 = not visible right now (hide it). A display-sized or
     *  fixed-size buffer keeps the legacy whole-window placement, which is
     *  where its pixels have always been shown. */
    public static long macapkSurfaceRectPacked(int key) {
        java.lang.ref.WeakReference<SurfaceView> ref = sSurfaceByKey.get(key);
        SurfaceView v = ref != null ? ref.get() : null;
        if (v == null) {
            if (ref != null) sSurfaceByKey.remove(key);
            return 0L;
        }
        try {
            if (!v.macapkAttached || !v.isShown()) {
                v.macapkNoteLayerPull(v.macapkAttached ? -3L : -2L, -1L);
                return -1L;
            }
            int[] d = v.macapkNotedDisplaySize();
            int dW = d != null ? d[0] : 0, dH = d != null ? d[1] : 0;
            android.graphics.Rect f = v.holder.getSurfaceFrame();
            int x, y, w, h;
            if (v.holder.macapkFixedSize || (f.width() == dW && f.height() == dH)) {
                if (dW <= 0 || dH <= 0) return -1L;
                x = 0; y = 0; w = dW; h = dH;
            } else {
                w = v.getWidth();
                h = v.getHeight();
                if (w <= 0 || h <= 0) { v.macapkNoteLayerPull(-4L, -1L); return -1L; }
                int[] o = new int[2];
                v.macapkWindowOrigin(o);
                x = o[0]; y = o[1];
            }
            long packed = ((x & 0xffffL) << 48)
                 | ((y & 0xffffL) << 32)
                 | ((w & 0xffffL) << 16)
                 |  (h & 0xffffL);
            v.macapkNoteLayerPull(packed, packed);
            return packed;
        } catch (Throwable t) {
            return -1L;   // never throw across JNI into the compositor
        }
    }

    /** The rect pull's answer, logged only on a change of STATE CLASS (shown /
     *  not attached / attached but not shown / zero size): the pull runs per
     *  Cocoa tick, and a layer that stays hidden is otherwise silent (a
     *  never-attached content root read as "hidden" on every tick, which is how
     *  Frozen Bubble's black game screen stayed invisible in the logs). A shown
     *  surface that MOVES (scrolling, an animation, a window drag) changes its
     *  rect every tick and must not log one line per frame, so only the first
     *  shown rect of a run of shown answers is printed. `state` is the packed
     *  rect, or -2 not attached / -3 attached but not shown / -4 zero size;
     *  nothing is allocated unless the class changed. */
    private volatile int macapkLastLayerClass = Integer.MIN_VALUE;
    void macapkNoteLayerPull(long state, long answer) {
        final int cls = (answer == -1L) ? (int) state : 0;   // 0 = shown
        if (cls == macapkLastLayerClass) return;
        macapkLastLayerClass = cls;
        String what = state == -2L ? "hidden (not attached to a window)"
                    : state == -3L ? "hidden (attached, not shown: vis=" + getVisibility() + ")"
                    : state == -4L ? "hidden (zero size)"
                    : "shown " + (short) (answer >>> 48) + "," + (short) (answer >>> 32)
                      + " " + ((answer >>> 16) & 0xffff) + "x" + (answer & 0xffff);
        NativeLog.i(TAG, "per-surface layer pull (key " + macapkSurfaceKey + "): " + what
            + " on " + getClass().getName());
    }

    /** Native pull: does this surface sit ABOVE its window (setZOrderOnTop)? */
    public static boolean macapkSurfaceZOrderOnTop(int key) {
        java.lang.ref.WeakReference<SurfaceView> ref = sSurfaceByKey.get(key);
        SurfaceView v = ref != null ? ref.get() : null;
        return v != null && v.mZOrderOnTop;
    }

    // ---- the native window owner: below the window + a hole ------------------
    //
    // AOSP composites EVERY SurfaceView's surface below its window unless the app
    // asked setZOrderOnTop(true): mSubLayer defaults to APPLICATION_MEDIA_SUBLAYER
    // (-2) and isAboveParent() is `mSubLayer >= 0` (android14
    // core/java/android/view/SurfaceView.java :200, :1528-1530). The window then
    // shows the surface through a hole the view punches in its own draw pass once
    // the first redraw completed (draw()/dispatchDraw() :512-533,
    // `if (mDrawFinished && !isAboveParent()) clearSurfaceViewPort(canvas)`,
    // clearSurfaceViewPort :579-597 = canvas.punchHole over the view bounds;
    // mDrawFinished is set by performDrawFinished :426-432 after updateSurface's
    // surfaceCreated/Changed/RedrawNeeded callbacks, sticky). Views drawn AFTER
    // the SurfaceView (a game's HUD, a menu drawn by Canvas) composite over it.
    //
    // Our native-EGL window layer -- the CAMetalLayer a native producer gets from
    // ANativeWindow_fromSurface (Unity, AGDK, Plague Inc's GLView, SDL) -- used to
    // sit ABOVE the whole window (zPosition 2.0), so a Canvas UI sharing the
    // screen with such a surface was invisible under it (Plague Inc "stuck on the
    // blue background", docs/surfaceview-z-below-default_2026-09-29.md). Moving
    // the layer below WITHOUT the hole is what blacked out Fruit Ninja in the
    // first attempt: the window's own opaque background then covered the game.
    // So the two halves land together, keyed on the view whose Surface the
    // producer consumed (the shim's fromSurface calls
    // Surface.macapkNoteNativeWindowConsumer): the layer goes below only when
    // that view is known, and that view punches its hole.
    private static volatile java.lang.ref.WeakReference<SurfaceView> sNativeWindowOwner;
    /** Sticky: some SurfaceView HAS owned the native window. Once true the layer
     *  answer never falls back to the legacy above-window slot (0), even after the
     *  owner has been collected: a destroyed SurfaceView's surface must not jump
     *  over the NEXT screen's Canvas UI (AOSP destroys the surface with its view). */
    private static volatile boolean sHadNativeWindowOwner;
    /** AOSP mDrawFinished for the native-window route; sticky, as AOSP's is. */
    private volatile boolean macapkNativeWindowDrawFinished;
    /** The hole-punch line is logged once per ownership: the log proof the hole branch
     *  of draw() was reached after the "native window owner" line
     *  (owner_issue_scan.py HIDDEN-GUEST keys on its absence). */
    private volatile boolean macapkNativeHoleLogged;

    /** The runtime handed {@code owner}'s Surface to a native producer (null: a
     *  Surface no SurfaceView vends, e.g. one built on a SurfaceTexture or an
     *  ImageReader -- IGNORED: AOSP layering is a property of the SurfaceView,
     *  mSubLayer, and never depends on some other Surface acquiring a native
     *  window, so such a Surface must not revoke the real owner). The surface
     *  callbacks have been delivered by then (producers consume the window inside
     *  or after surfaceCreated/surfaceChanged), which is the point AOSP's
     *  performDrawFinished marks; like it, ask the tree to redraw so the hole
     *  appears (performDrawFinished's invalidate(); posted, as this can run on the
     *  producer's thread). */
    static void macapkNoteNativeWindowOwner(SurfaceView owner) {
        if (owner == null) return;
        java.lang.ref.WeakReference<SurfaceView> prevRef = sNativeWindowOwner;
        SurfaceView prev = prevRef != null ? prevRef.get() : null;
        sNativeWindowOwner = new java.lang.ref.WeakReference<>(owner);
        sHadNativeWindowOwner = true;
        owner.macapkNativeWindowDrawFinished = true;
        try { owner.postInvalidate(); } catch (Throwable ignored) { }
        if (prev != null && prev != owner) {
            // the previous owner stops punching: redraw it so its hole closes
            try { prev.postInvalidate(); } catch (Throwable ignored) { }
        }
        if (prev != owner) {
            owner.macapkNativeHoleLogged = false;   // each owner line is followed by its own punch line
            NativeLog.i(TAG, "native window owner: " + owner.getClass().getName()
                + " onTop=" + owner.mZOrderOnTop + " -> its layer composites "
                + (owner.mZOrderOnTop ? "ABOVE" : "BELOW (hole punched)")
                + " the window (MACAPK_SV_GUEST_BELOW)");
        }
    }

    private boolean macapkIsNativeWindowOwner() {
        java.lang.ref.WeakReference<SurfaceView> ref = sNativeWindowOwner;
        return ref != null && ref.get() == this;
    }

    /** Native pull, every drawInMTKView tick: where the native-EGL window layer
     *  goes. 0 = no SurfaceView has EVER owned it (or MACAPK_SV_GUEST_BELOW=0):
     *  the legacy slot above the whole window; 1 = below the window
     *  (APPLICATION_MEDIA_SUBLAYER / MEDIA_OVERLAY), its owner punches the hole --
     *  also the answer once the owner is gone (nothing of a destroyed surface is
     *  visible, as on AOSP); 2 = above its own window (setZOrderOnTop(true),
     *  APPLICATION_PANEL_SUBLAYER) but still below other windows.
     *  cocoa_bridge.mm macapk_guest_layer_z_for maps the answer to a zPosition. */
    public static int macapkNativeWindowLayerZ() {
        java.lang.ref.WeakReference<SurfaceView> ref = sNativeWindowOwner;
        SurfaceView v = ref != null ? ref.get() : null;
        if (v == null) return sHadNativeWindowOwner ? 1 : 0;
        return v.mZOrderOnTop ? 2 : 1;
    }

    // ---- MetalSurfaceCallbacks: the host's surface size, each frame ----
    @Override
    public void macapkSurfaceChanged(int widthPx, int heightPx) {
        if (MACAPK_TRACE_DISPATCH) {
            // Every arrival, with the latch state. A good MCPE run shows a
            // SECOND arrival that carries the real ANativeWindow; the BLANK
            // failure shows only the first.
            NativeLog.i(TAG, "[surface-dispatch] macapkSurfaceChanged " + widthPx
                + "x" + heightPx + " createdFired=" + holderSurfaceCreatedFired
                + " pending=" + pendingSurfaceW + "x" + pendingSurfaceH
                + " on " + getClass().getName());
        }
        // Publish the REAL drawable size for the keyboard-binding path, which
        // has to denormalise a bound point into the same surface-pixel space a
        // real click lands in. This is the authoritative source; deriving it
        // from the points-space globals instead is the drift that caused
        // v1.0.742's viewport bug.
        try { com.macapk.loader.TouchDispatcher.macapkNoteSurfaceSize(widthPx, heightPx); }
        catch (Throwable ignored) { }
        macapkNoteNativeDisplaySize(widthPx, heightPx);
        // Fire SurfaceHolder.Callback callbacks. SurfaceView subclasses that
        // use lockCanvas/unlockCanvasAndPost (Frozen Bubble's GameView,
        // Bouncy's CanvasFieldView, any 2D game with a custom render
        // thread, all non-OpenGL SurfaceView consumers) register holder
        // callbacks via getHolder().addCallback() and expect them to fire
        // when the surface is ready. Without this dispatch surfaceCreated
        // never runs -> the engine's drawing thread never starts ->
        // lockCanvas/unlockCanvasAndPost never run -> screen stays black
        // even though MTKView is hosting and ANGLE is swapping.
        //
        // Per AOSP frameworks/base/core/java/android/view/SurfaceView.java
        // and the public SurfaceHolder.Callback javadoc, surfaceCreated
        // fires exactly once per surface lifetime, surfaceChanged fires
        // on every dimension/format change. We mirror that contract here.
        //
        // A GLSurfaceView (AOSP's) registers itself as a callback, and
        // its GLThread starts drawing once it has heard surfaceCreated and
        // surfaceChanged; an app's own GL thread does the same through its
        // own callback.
        // v1.0.158 DIAGNOSTIC: confirm this fires (and on which subclass
        // instance) so we can tell whether the SurfaceView our framework
        // drives is Unity's a.g (which carries the engine's holder
        // callback) or some other SurfaceView.
        //
        // v1.0.572: surfaceCreated delivery waits until the dispatch can
        // actually be heard. On real Android the surface callbacks are
        // main-looper messages that CANNOT run until onResume returns, so
        // anything an app sets up in onCreate -- registering its
        // SurfaceHolder.Callback, loading its native lib, registering
        // native surface handlers (AGDK GameActivity does all three) -- is
        // guaranteed in place before surfaceCreated. Our dispatch arrives
        // from the Cocoa render thread, which can (and for AppCompat-
        // wrapped GL activities like Minecraft's GameActivity, did) land
        // mid-onCreate: the single-shot latch then consumed surfaceCreated
        // with zero listeners and the AGDK native loop never received
        // APP_CMD_INIT_WINDOW. When not ready (or when a callback rejects
        // the delivery -- see macapkTryFireHolderCreated) we record the
        // size and macapkDrawFrame retries per frame. Window focus is
        // delivered inside macapkTryFireHolderCreated, preserving the
        // surface-before-focus ordering Unity depends on.
        if (!holderSurfaceCreatedFired) {
            if (macapkRecordArrivalDuringPostedCreate(widthPx, heightPx)) {
                // A posted surfaceCreated is in flight (SV_UI_THREAD_CALLBACKS):
                // this arrival is delivered as its own surfaceChanged right after
                // that create, in AOSP order. The size is also kept pending so a
                // create that is REJECTED (not latched) retries at the latest size.
                pendingSurfaceW = widthPx;
                pendingSurfaceH = heightPx;
            } else if (macapkHolderDispatchReady()
                    && macapkTryFireHolderCreated(widthPx, heightPx)) {
                NativeLog.i(TAG, "macapkSurfaceChanged " + widthPx + "x"
                    + heightPx + " on " + getClass().getName()
                    + " (first; fired surfaceCreated)");
            } else {
                pendingSurfaceW = widthPx;
                pendingSurfaceH = heightPx;
                long n = ++surfaceDeferLogCount;
                if (n == 1 || n == 60 || (n % 600) == 0) {
                    NativeLog.i(TAG, "macapkSurfaceChanged " + widthPx + "x"
                        + heightPx + " on " + getClass().getName()
                        + " deferred #" + n + " (listeners/native not ready; "
                        + "surfaceCreated follows onResume on real Android)");
                }
            }
        } else {
            // Every native arrival still fires, as before (MCPE's handover
            // depends on the SECOND arrival reaching its holder); only the
            // size is the holder's (== widthPx x heightPx for a view that
            // fills its window, and with MACAPK_SV_VIEW_SIZE=0).
            int[] s = macapkHolderSurfaceSize(widthPx, heightPx);
            holder.fireChanged(0, s[0], s[1]);

            // v1.0.164 (+ v1.0.572 resumed gate): surface is live; deliver
            // window focus once the launch burst is done (re-delivery safe:
            // once-guarded per activity inside ActivityHost).
            pendingFocusDelivery = true;
            macapkDeliverFocusIfResumed();
        }
    }


    @Override
    public void macapkDrawFrame() {
        // v1.0.70: drive the Canvas overlay (if registered by
        // Activity.setContentView when the rootView contains both a
        // GL view and plain widgets) BEFORE the GL render. The
        // overlay's painted bitmap goes to its own CALayer above the
        // MTKView, so ordering between GL paint and overlay paint
        // doesn't affect visibility -- but doing the overlay first
        // means a heavy GL onDrawFrame doesn't starve it. Skips
        // cheaply when no overlay is set.
        //
        // v1.0.742 FIX: this used to pass NativeCocoa.phoneWidth()/
        // phoneHeight() -- the LOGICAL/design phone size in POINTS -- while
        // Activity.macapkInstallContentDirect seeds the SAME overlay with
        // phoneWidth()*2/phoneHeight()*2 (an attempt at PIXELS). Since
        // points never equal pixels on a Retina (2x+) display,
        // macapkDriveOverlay's "did the size change" check tripped on
        // (essentially) every frame and re-fired the overlay's
        // macapkSurfaceChanged with the wrong, much-too-small POINTS value.
        // For any app whose GL SurfaceView shares a ViewGroup with sibling
        // widgets (an ad banner, a button panel -- the "Bouncy" pattern
        // this overlay exists for), that re-lays-out the GL view itself at
        // the wrong size, firing its Android-standard onSizeChanged and
        // corrupting the engine's own viewport down to a small rect pinned
        // at the GL origin (bottom-left) -- HCR 1.8.1's "renders in the
        // bottom-left corner until resized" (docs/hcr181-viewport-corner-
        // bug_2026-07-23.md). Use THIS surface's own last known real pixel
        // size (populated by our own macapkSurfaceChanged from the native
        // side's actual drawable pixels) instead of re-deriving one from
        // the points-space globals -- a single authoritative source that
        // can't drift out of sync with itself. Before the real surface
        // size is known yet (holder.frame still zeroed), fall back to the
        // SAME best-effort guess Activity.java seeds the overlay with, so
        // the two never disagree during the brief startup window either.
        // Same derived surface size feeds BOTH the ViewCanvasDriver overlay and
        // the WindowManager overlay below -- deriving it twice is how the two
        // drifted into a points-vs-pixels disagreement in the first place.
        int[] overlaySize = macapkDriveOverlayForCurrentSurface();
        int overlayW = overlaySize[0];
        int overlayH = overlaySize[1];

        // Composite any WindowManager overlay (Dialog/AlertDialog/PopupWindow)
        // shown on top of this surface (upstream's window overlays; AOSP's
        // views compose their windows themselves). Without it a dialog was
        // measured, laid out and hit-tested, but drawn nowhere. Since WindowOverlayStack.deliverTouch
        // consumes an outside touch on a modal window (AOSP's default until
        // FLAG_NOT_TOUCH_MODAL is set), that invisible window swallowed 100% of
        // touch input with no log line at all -- HCR 1.8.1's AdManager rating
        // dialog killed every tap on a perfectly-rendering menu.
        //
        // The overlay paints into its own
        // CALayer above the MTKView, so ordering against the GL paint does not
        // affect visibility -- same reasoning as macapkDriveOverlay above.
        // Cheap when the overlay stack is empty (one boolean check).
        try {
            com.macapk.loader.WindowOverlayStack.presentForGl(overlayW, overlayH);
        } catch (Throwable t) {
            NativeLog.w(TAG, "presentForGl threw: " + t);
        }

        // Drain Handler.GLOBAL_QUEUE every frame BEFORE the renderer
        // gate below (v1.0.345; it used to sit after the early-return).
        // On real Android the main looper runs whether or not any GL
        // renderer exists, so posted main-thread runnables MUST fire
        // even while no renderer is discovered. Gating the drain on
        // renderer discovery deadlocked Plague Inc's boot: its
        // SplashActivity.onCreate never calls setContentView (the
        // splash is the window-background drawable on real Android) and
        // just does delayHandler.postDelayed(goToMainActivity, 500) --
        // goToMainActivity is what loads the engine and starts
        // MainActivity (which creates the REAL GL view + renderer). With
        // the drain below the renderer-null return, the runnable sat in
        // GLOBAL_QUEUE forever: no renderer -> no drain -> no
        // MainActivity -> no renderer. Any app whose splash posts its
        // continuation to the main looper hits the same cycle.
        //
        // Original placement rationale (still honored): the drain must
        // run AFTER the surface exists. macapkDrawFrame only fires once
        // drawInMTKView is live, so draining here -- before the
        // renderer gate, after surface creation -- satisfies both. See
        // the v1.0.64 note below for why GLSurfaceView.queueEvent-style
        // deferred GL work also needs this per-frame drain.
        try {
            android.os.Handler.drainGlobal();
        } catch (Throwable t) {
            NativeLog.e(TAG, "Handler.drainGlobal threw: " + t);
        }
        // v1.0.575: while the macapk-activity main loop is parked in a blocking
        // AGDK lifecycle handshake (onStartNative/onResumeNative), it is NOT
        // draining the real main queue. Drain the DUE messages here on the OS-main
        // render thread so a cross-thread runOnUiThread post that a blocked
        // android_main is waiting on (Minecraft's runNativeCallbackOnUiThread
        // FutureTask) completes -> android_main unblocks -> consumes APP_CMD_START
        // -> the handshake returns. Gated on the handshake flag so there is never
        // a double-drain with macapk-activity's own Looper.loop. No-op unless

        // The holder's callbacks and window focus. A renderer is not driven from here: a
        // GLSurfaceView's (or an app's own) GLThread draws into its window surface itself.
        macapkConvergeSurfaceLifecycle(0, 0);
    }

    /** Holder used so AndEngine et al. can register a surfaceCreated /
     *  surfaceChanged callback. We fire those synchronously from the
     *  Activity's setContentView path so the renderer thinks the
     *  surface was created and sized. */
    // ------------------------------------------------------------------
    // Video-sink registry: WHICH SurfaceView the decoded video belongs to,
    // and WHERE it is on screen right now.
    //
    // On real Android this association is structural: the Surface a
    // SurfaceHolder vends is a producer handle onto the SurfaceView's own
    // SurfaceControl, and the compositor scales every queued buffer to the view's
    // on-screen rect -- SurfaceView.onSetSurfacePositionAndScale
    // (aosp-mirror/master core/java/android/view/SurfaceView.java, fetched
    // 2026-08-17):
    //     transaction.setPosition(surface, positionLeft, positionTop);
    //     transaction.setMatrix(surface, postScaleX /*dsdx*/, 0f, 0f,
    //                           postScaleY /*dsdy*/);
    // with postScale = mScreenRect.width() / (float) mSurfaceWidth (resp.
    // height), and mScreenRect built in updateSurface from
    // getLocationInWindow(mLocation) + getWidth()/getHeight(). The default
    // codec scaling mode composes with that: MediaCodec.
    // VIDEO_SCALING_MODE_SCALE_TO_FIT = 1, "The content is scaled to the
    // surface dimensions", and setVideoScalingMode's javadoc: "The default is
    // 'scale to fit'" (aosp-mirror/master media/java/android/media/
    // MediaCodec.java:5052-5092, fetched 2026-08-17). Net: decoded frame
    // stretches to the surface, surface stretches to the view rect.
    //
    // Mac.apk's equivalent: MediaCodec.configure/setOutputSurface register
    // the Surface's owner SurfaceView here; cocoa_bridge.mm's video layer
    // (CocoaPresentVideoFrame + the per-frame drawInMTKView refresh) PULLS
    // macapkActiveVideoRectPacked() and sets the CAMetalLayer's frame to the
    // rect, so the presenter's stretch-to-layer blit reproduces the composed
    // AOSP scaling exactly. PULLED, not pushed, for the same reason
    // StatusBarDriver.macapkContentInsetsPacked is: the pull happens on the
    // Cocoa main thread, which is also the thread ViewCanvasDriver lays the
    // tree out on, so the rect can never be stale by more than a frame and
    // there is no cross-thread invalidation protocol to get wrong.
    //
    // A WeakReference so a dismissed player screen's tree can be collected;
    // the codec outliving the view then reads "hidden" (0-sentinel cases
    // below), which is also what a released SurfaceControl shows on a device.
    private static volatile java.lang.ref.WeakReference<SurfaceView>
            sVideoSinkView;
    /** The Surface whose registration produced sVideoSinkView, so a codec
     *  released AFTER another codec re-registered does not clear the newer
     *  sink (release() passes its own surface here). */
    private static volatile android.view.Surface sVideoSinkSurface;

    /** MediaCodec.configure/setOutputSurface hand every non-null output
     *  Surface here. A SurfaceView-owned Surface becomes the active sink; any
     *  other Surface (SurfaceTexture-backed -- the TextureView path handles
     *  those -- or app-constructed) clears it, so there is exactly one notion
     *  of "current sink" and it always reflects the codec's LATEST binding. */
    public static void macapkRegisterVideoSink(android.view.Surface s) {
        SurfaceView owner = (s != null) ? s.macapkGetOwnerSurfaceView() : null;
        if (owner != null) {
            sVideoSinkView = new java.lang.ref.WeakReference<>(owner);
            sVideoSinkSurface = s;
            NativeLog.i(TAG, "video sink registered: " + owner
                + " bounds=" + owner.getWidth() + "x" + owner.getHeight());
            // One-shot structural dump: the sink's SIBLINGS decide what
            // composites over the video (media3's shutter is a sibling View
            // painted opaque black until onRenderedFirstFrame hides it), so
            // when a player box reads black on screen this line separates
            // "our plane is mispositioned" from "the app is still showing its
            // shutter" without another instrumented build.
            try {
                ViewParent p = owner.getParent();
                if (p instanceof ViewGroup) {
                    ViewGroup pg = (ViewGroup) p;
                    StringBuilder sb = new StringBuilder("video sink siblings in ")
                        .append(pg.getClass().getSimpleName()).append(':');
                    for (int i = 0; i < pg.getChildCount(); i++) {
                        View c = pg.getChildAt(i);
                        sb.append(" [").append(i).append(']')
                          .append(c.getClass().getSimpleName())
                          .append(" vis=").append(c.getVisibility())
                          .append(' ').append(c.getWidth()).append('x')
                          .append(c.getHeight())
                          .append(c == owner ? "<==sink" : "");
                    }
                    NativeLog.i(TAG, sb.toString());
                }
            } catch (Throwable ignored) {}
            // Re-record the interop draw. A Compose-hosted view's content is
            // RECORDED into a RenderNode/GraphicsLayer and REPLAYED until the
            // view invalidates; the recording that exists right now was made
            // BEFORE this registration, so draw() -- and therefore the hole
            // punch -- never re-executes without this. Measured 2026-08-17:
            // no "video sink punch" line ever fired on PlayTube until the
            // invalidate was added. postInvalidate: registration runs on the
            // app's playback/codec thread, never the UI thread.
            try { owner.postInvalidate(); } catch (Throwable ignored) {}
        } else {
            sVideoSinkView = null;
            sVideoSinkSurface = null;
            if (s != null) {
                // Diagnostic breadcrumb: names WHICH kind of non-view Surface
                // the codec was bound to (SurfaceTexture-backed goes through
                // the TextureView latch; a bare Surface keeps the legacy
                // whole-window plane).
                NativeLog.i(TAG, "video sink: non-SurfaceView Surface ("
                    + (s.macapkGetSurfaceTexture() != null
                        ? "SurfaceTexture-backed" : "bare")
                    + ") -- whole-window video plane");
            }
        }
    }
    /** Codec teardown: clear only if OUR surface is still the registered one. */
    public static void macapkUnregisterVideoSink(android.view.Surface s) {
        if (s != null && s == sVideoSinkSurface) {
            SurfaceView old = sVideoSinkView != null ? sVideoSinkView.get() : null;
            sVideoSinkView = null;
            sVideoSinkSurface = null;
            // Same re-record reasoning as registration: the punch must stop
            // replaying once the sink is gone.
            if (old != null) {
                try { old.postInvalidate(); } catch (Throwable ignored) {}
            }
        }
    }

    /** Win.apk (windows/host): a decoded video picture for the holder Surface
     *  {@code s} -- or, with null, for the registered video sink -- shown
     *  through the owning view's own per-surface layer at its rect, below
     *  the window, which then punches its hole (the lockCanvas route of
     *  macapkPresentThroughSurfaceLayer; the Mac shows these through its video
     *  plane instead). False when no SurfaceView owns the Surface or the
     *  runtime has no per-surface layer: the caller shows it another way. */
    public static boolean macapkPresentVideoFrame(android.view.Surface s, int[] argb, int w, int h) {
        SurfaceView v = s != null ? s.macapkGetOwnerSurfaceView() : null;
        if (v == null && s == null) {
            java.lang.ref.WeakReference<SurfaceView> ref = sVideoSinkView;
            v = ref != null ? ref.get() : null;
        }
        return v != null && v.macapkPresentThroughSurfaceLayer(argb, w, h);
    }

    /** The active video sink's on-screen rect in SURFACE pixels, packed
     *  (x << 48) | (y << 32) | (w << 16) | h with each field a SIGNED 16-bit
     *  value (x/y go negative when the view is scrolled partly off-screen).
     *
     *  Sentinels: 0 = no SurfaceView sink registered (caller keeps its legacy
     *  fullscreen behaviour -- a bare-Surface producer has no view geometry);
     *  -1 = a sink IS registered but currently has no visible rect (detached,
     *  not shown, or zero-sized) -- the video plane must HIDE, exactly as a
     *  device stops showing a SurfaceControl whose view is gone.
     *
     *  COORDINATE SPACE: the full display surface -- the same space
     *  ViewCanvasDriver's canvas bitmap lives in. The driver draws the root
     *  translated down by StatusBarDriver.heightPx (canvas.translate(0,
     *  statusBarH); rootView.draw(canvas)), so a view's window coords map to
     *  surface coords by adding that same per-window inset. Using the SAME
     *  accessor the driver uses keeps the two from drifting (the
     *  measure-and-layout-must-share-one-height lesson of v1.0.865). */
    /** Last pull STATE (0=none, 1=hidden, 2=visible), logged on transition so
     *  a black player box is attributable from the log alone. */
    private static int sSinkPullState = -1;
    private static void macapkNoteSinkState(int st, String detail) {
        if (st != sSinkPullState) {
            sSinkPullState = st;
            NativeLog.i(TAG, "video sink state -> "
                + (st == 0 ? "NONE" : st == 1 ? "HIDDEN" : "VISIBLE")
                + (detail != null ? (" (" + detail + ")") : ""));
        }
    }

    /** DIAGNOSTIC (gated OFF by default via MACAPK_VIDEO_SINK_TRACE=1):
     *  periodic structural dump of the video sink's PLAYER SUBTREE -- every
     *  ancestor/sibling/descendant's visibility, alpha, bounds and (for
     *  TextViews) live text.
     *
     *  It exists to separate two explanations of a black player box that are
     *  INDISTINGUISHABLE FROM PIXELS, and that the one-shot dump at
     *  registration structurally cannot tell apart:
     *    (a) the app's shutter overlay is still VISIBLE -- the first-frame
     *        notification never reached the player UI, so the app is
     *        deliberately covering its own video, exactly as a device would;
     *    (b) the shutter is INVISIBLE and the clock text has advanced in the
     *        TREE, yet the window still shows the old pixels -- a repaint /
     *        re-record failure on OUR side, not an app-state failure.
     *  Registration runs at codec configure, strictly BEFORE any frame is
     *  rendered, so the flag it reads is always the pre-first-frame one; only
     *  a dump taken DURING steady playback can answer the question.
     *
     *  Runs on whichever thread pulls the rect (the Cocoa main thread), which
     *  is the same thread ViewCanvasDriver lays the tree out on, so it observes
     *  exactly the state the compositor is about to draw. */
    private static final boolean VIDEO_SINK_TRACE =
            "1".equals(System.getenv("MACAPK_VIDEO_SINK_TRACE"));
    private static final long VIDEO_SINK_TRACE_MS = 3000L;
    private static long sLastSinkTrace;
    /** draw() punch count, so the dump reports whether the interop subtree is
     *  re-drawn every frame or frozen on a stale recording. */
    private static long sPunchDraws;

    private static void macapkTraceSinkTree(SurfaceView sink) {
        if (!VIDEO_SINK_TRACE || sink == null) return;
        long now = android.os.SystemClock.uptimeMillis();
        if (now - sLastSinkTrace < VIDEO_SINK_TRACE_MS) return;
        sLastSinkTrace = now;
        try {
            // Climb three levels: that spans media3's
            // SurfaceView -> AspectRatioFrameLayout -> PlayerView, where the
            // shutter, the artwork overlays AND the transport controls (with
            // the position/duration TextViews) all live. Generic climb, not a
            // class-name match -- any player built the same way dumps the same.
            View root = sink;
            for (int i = 0; i < 3; i++) {
                ViewParent p = root.getParent();
                if (!(p instanceof View)) break;
                root = (View) p;
            }
            StringBuilder sb = new StringBuilder("video sink tree (punchDraws=")
                .append(sPunchDraws).append("):\n");
            macapkDumpSinkSubtree(root, sink, 0, sb);
            NativeLog.i(TAG, sb.toString());
        } catch (Throwable ignored) {}
    }

    private static void macapkDumpSinkSubtree(View v, View sink, int depth,
            StringBuilder sb) {
        if (v == null || depth > 6) return;
        for (int i = 0; i < depth; i++) sb.append("  ");
        sb.append(v.getClass().getSimpleName())
          .append(" vis=").append(v.getVisibility())
          .append(" a=").append(v.getAlpha())
          .append(' ').append(v.getLeft()).append(',').append(v.getTop())
          .append(' ').append(v.getWidth()).append('x').append(v.getHeight());
        if (v == sink) sb.append(" <==SINK");
        if (v instanceof android.widget.TextView) {
            CharSequence t = ((android.widget.TextView) v).getText();
            if (t != null && t.length() > 0) sb.append(" text='").append(t).append('\'');
        }
        sb.append('\n');
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++)
                macapkDumpSinkSubtree(g.getChildAt(i), sink, depth + 1, sb);
        }
    }

    public static long macapkActiveVideoRectPacked() {
        java.lang.ref.WeakReference<SurfaceView> ref = sVideoSinkView;
        if (ref == null) { macapkNoteSinkState(0, null); return 0L; }
        SurfaceView v = ref.get();
        if (v == null) { macapkNoteSinkState(1, "collected"); return -1L; }
        int w, h;
        try {
            if (!v.macapkAttached || !v.isShown()) {
                macapkNoteSinkState(1, "attached=" + v.macapkAttached
                    + " shown=" + v.isShown());
                return -1L;
            }
            w = v.getWidth();
            h = v.getHeight();
            if (w <= 0 || h <= 0) { macapkNoteSinkState(1, "zero-size"); return -1L; }
            // v1.0.997 CORRECTION -- this used to be
            //     y = loc[1] + StatusBarDriver.heightPx(root)
            // which DOUBLE-COUNTS the status bar. getLocationInWindow sums
            // getLeft()/getTop() up the parent chain INCLUDING the root view's
            // own top, and the root view's top already IS the status-bar height;
            // ViewCanvasDriver then applies that same inset a second time as a
            // canvas translate (canvas.translate(0, statusBarH); rootView.draw).
            // So subtract the root's own offset before adding the driver's, which
            // is correct whether or not the two happen to be equal.
            //
            // MEASURED 2026-08-18 on PlayTube: the pull answered y=96 and y=121
            // while the hole SurfaceView.draw actually punched -- read off the
            // frame canvas's own CTM -- landed at y=48 and y=73. Exactly 48px
            // apart, every time: the video plane was presenting a status bar's
            // height BELOW the hole cut for it, so the top of every frame was
            // hidden under the canvas and the bottom of the hole showed the
            // content behind it. The hole and the plane must come from ONE
            // number (the measure-and-layout-must-share-one-height lesson of
            // v1.0.865); this is that number. (2026-09-26: it lives in
            // macapkWindowOrigin now, the ONE formula shared with the
            // placement of a lockCanvas surface; unchanged.)
            int[] origin = new int[2];
            v.macapkWindowOrigin(origin);
            int x = origin[0];
            int y = origin[1];
            macapkNoteSinkState(2, x + "," + y + " " + w + "x" + h);
            macapkTraceSinkTree(v);
            return ((long) (x & 0xffffL) << 48)
                 | ((long) (y & 0xffffL) << 32)
                 | ((long) (w & 0xffffL) << 16)
                 |  (long) (h & 0xffffL);
        } catch (Throwable t) {
            // A half-torn-down tree mid-pull must degrade to "hidden", never
            // throw across JNI into the compositor.
            macapkNoteSinkState(1, "threw " + t.getClass().getSimpleName());
            return -1L;
        }
    }

    /** Is THIS view the registered video sink right now? (State-gated, never
     *  identity-gated: any app whose codec output is bound to a SurfaceView
     *  qualifies.) */
    private boolean macapkIsActiveVideoSink() {
        java.lang.ref.WeakReference<SurfaceView> ref = sVideoSinkView;
        return ref != null && ref.get() == this;
    }

    /** AOSP SurfaceView.draw punches a transparent hole in the window at the
     *  view's bounds so the surface BEHIND the window shows through --
     *  aosp-mirror/master core/java/android/view/SurfaceView.java (fetched
     *  2026-08-17): draw()/dispatchDraw() call clearSurfaceViewPort(canvas),
     *  which is `canvas.punchHole(0f, 0f, getWidth(), getHeight(), ...)`.
     *  Siblings drawn after this view (ExoPlayer's transport controls) then
     *  composite on top of the hole, which is the whole inline-player model.
     *
     *  Gated to the ACTIVE VIDEO SINK deliberately: for a full-window GL
     *  SurfaceView the ViewCanvasDriver already clears the whole canvas
     *  transparent (its CASE A), so a punch is a visual no-op there, and
     *  punching unconditionally would change every app hosting an idle
     *  SurfaceView (camera placeholders and the like) in one step -- the
     *  fully-AOSP unconditional punch is noted as an open question in
     *  docs/video-player-geometry_2026-08-17.md pending a fleet A/B. */
    /* The CLEAR-xfermode Paint that used to live here is gone: v1.0.997 calls
     * Canvas.punchHole, which is the op AOSP actually uses and which takes the
     * same explicit (0, 0, getWidth(), getHeight()) bounds the old comment
     * argued for (drawColor would have covered the whole CLIP instead). */

    /** First-fire flag for the punch log line below (diagnosis: "was
     *  SurfaceView.draw reached through the app's draw path at all?"). */
    private static boolean sPunchLogged;

    @Override
    public void draw(android.graphics.Canvas canvas) {
        if (macapkIsActiveVideoSink()) {
            // v1.0.997: AOSP's own op, not a hand-rolled CLEAR drawRect.
            // SurfaceView.clearSurfaceViewPort is
            //   canvas.punchHole(0f, 0f, getWidth(), getHeight(), 0f, 0f, alpha)
            // (aosp-mirror/master core/java/android/view/SurfaceView.java:727-745,
            // fetched 2026-08-18), and the alpha argument is load-bearing: HWUI
            // punches with DST_OUT modulated by the View's alpha, so a fading
            // SurfaceView fades its hole rather than switching it off. The old
            // CLEAR drawRect dropped that, and -- more importantly -- could not
            // escape an interposed RenderNode offscreen; see Canvas.punchHole.
            canvas.punchHole(0f, 0f, getWidth(), getHeight(), 0f, 0f, getAlpha());
            sPunchDraws++;
            if (!sPunchLogged) {
                sPunchLogged = true;
                // Is this the ROOT frame canvas, or an interposed offscreen? A
                // CLEAR/DST_OUT recorded into an offscreen that is later
                // composited SrcOver leaves the frame untouched, so the hole
                // never appears no matter how correct the punch is.
                android.graphics.Canvas root =
                        com.macapk.loader.ViewCanvasDriver.macapkRootCanvas;
                NativeLog.i(TAG, "video sink punch: first draw() reached, "
                    + getWidth() + "x" + getHeight()
                    + " canvas=@" + Integer.toHexString(System.identityHashCode(canvas))
                    + " rootCanvas=@" + (root == null ? "none"
                        : Integer.toHexString(System.identityHashCode(root)))
                    + " isRoot=" + (root != null && root == canvas));
            }
        } else if (macapkSurfaceLayerDrawn && !mZOrderOnTop) {
            // A lockCanvas surface shown through its own layer BELOW the view
            // tree: the same hole, on AOSP's own condition (android14 draw()
            // :512-521, `if (mDrawFinished && !isAboveParent())` ->
            // clearSurfaceViewPort). Without it any opaque background a parent
            // paints under this view would hide the surface. Only reachable once
            // the runtime has a per-surface layer (macapkPresentThroughSurfaceLayer).
            canvas.punchHole(0f, 0f, getWidth(), getHeight(), 0f, 0f, getAlpha());
        } else if (macapkNativeWindowDrawFinished && !mZOrderOnTop && macapkIsNativeWindowOwner()) {
            // The native-EGL window layer sits BELOW the window for this view
            // (macapkNativeWindowLayerZ == 1): the same AOSP hole, on the same
            // condition (draw() :512-521). Without it the window's own opaque
            // background -- drawn before this view -- covers the producer's
            // frames (the Fruit Ninja black-out of the first z-below attempt).
            canvas.punchHole(0f, 0f, getWidth(), getHeight(), 0f, 0f, getAlpha());
            if (!macapkNativeHoleLogged) {
                macapkNativeHoleLogged = true;
                NativeLog.i(TAG, "native window hole: first punch " + getWidth() + "x"
                    + getHeight() + " by " + getClass().getName() + " (MACAPK_SV_GUEST_BELOW)");
            }
        } else if (MACAPK_ATTRS_TO_VIEW && !mZOrderOnTop && getNativeHandle() != 0) {
            // AOSP's views: ViewRootImpl's window is composited over this view's GL frames
            // (AospWindows.showSurfaceBelow), so the window gets AOSP's hole here, on its
            // condition (draw() :512-521, `!isAboveParent()` -> clearSurfaceViewPort).
            canvas.punchHole(0f, 0f, getWidth(), getHeight(), 0f, 0f, getAlpha());
        }
        super.draw(canvas);
    }

    private final SurfaceHolderImpl holder = new SurfaceHolderImpl(this);

    /** Legacy entry point: was originally meant to be called by
     *  Activity.setContentView. In v1.0.69 the same dispatch now lives
     *  in macapkSurfaceChanged (which MTKView already calls), so this
     *  is currently unused. Kept around in case a future caller needs
     *  to force the dispatch out-of-band. Uses the same single-shot
     *  latch as macapkSurfaceChanged so doubled invocations won't
     *  produce two surfaceCreated callbacks. */
    public final void macapkNotifySurfaceReady(int widthPx, int heightPx) {
        if (!holderSurfaceCreatedFired) {
            holder.fireCreated();
            holderSurfaceCreatedFired = true;
        }
        holder.fireChanged(0, widthPx, heightPx);
    }

    private static final class SurfaceHolderImpl implements SurfaceHolder {
        /** Owning SurfaceView, for the retroactive-delivery check below. */
        private final SurfaceView owner;
        SurfaceHolderImpl(SurfaceView owner) {
            this.owner = owner;
            // Structural on real Android (the Surface IS a handle onto this
            // view's buffer queue); here the link is what lets MediaCodec
            // recognise a SurfaceView-bound output -- see the video-sink
            // registry block above.
            surface.macapkSetOwnerSurfaceView(owner);
        }
        private final java.util.ArrayList<Callback> callbacks = new java.util.ArrayList<>();
        private final android.view.Surface surface = new android.view.Surface();
        private final android.graphics.Rect frame = new android.graphics.Rect();
        // Persistent backing bitmap. lockCanvas() returns a Canvas
        // wrapping this; unlockCanvasAndPost() is a no-op for now (no
        // Metal upload path yet) but the bitmap stays around so apps
        // can detect "next frame == still painting" via getPixel()-
        // style probes. Lazily allocated at first lockCanvas() call so
        // we can use the actual surface dimensions.
        private android.graphics.Bitmap drawBitmap;
        private android.graphics.Canvas drawCanvas;

        @Override public void addCallback(Callback cb) {
            if (cb == null || callbacks.contains(cb)) return;  // AOSP dedupes
            callbacks.add(cb);
            // v1.0.572: retroactive delivery. Our surface is live from the
            // first drawInMTKView, so a callback registered AFTER the
            // (single-shot) surfaceCreated dispatch would otherwise never
            // hear it. On real Android this ordering can't happen at boot
            // (surfaceCreated is a post-onResume main-looper message), so
            // retro-firing here reconstructs the ordering guarantee the app
            // was written against rather than inventing new behavior.
            if (owner != null && owner.holderSurfaceCreatedFired) {
                NativeLog.i(TAG, "addCallback after surfaceCreated: retro-"
                    + "delivering to " + cb.getClass().getName());
                try {
                    cb.surfaceCreated(this);
                    if (frame.width() > 0 && frame.height() > 0) {
                        cb.surfaceChanged(this, 0, frame.width(), frame.height());
                    }
                } catch (Throwable t) {
                    NativeLog.e(TAG, "retroactive surfaceCreated threw: " + t);
                }
            }
        }
        @Override public void removeCallback(Callback cb) { callbacks.remove(cb); }
        @Override public boolean isCreating()            { return false; }
        @Override public android.view.Surface getSurface() { return surface; }
        @Override public android.graphics.Rect getSurfaceFrame() { return frame; }
        @Override public void setFormat(int f)            {}
        @Override public void setKeepScreenOn(boolean s)  {}
        @Override public void setFixedSize(int w, int h)  { frame.set(0, 0, w, h); macapkFixedSize = true; }
        @Override public void setSizeFromLayout()         { macapkFixedSize = false; }
        /** 2026-09-26: this holder has served a lockCanvas, so its consumer
         *  draws with a Canvas and its frames are placed at the view's rect;
         *  until then it may be a GL/EGL consumer (macapkHolderSurfaceSize).
         *  Sticky: a surface does not change consumers. Written by the
         *  drawing thread, read by the render thread. */
        volatile boolean macapkLockedCanvas;
        /** v1.0.1637: the app pinned the buffer size (setFixedSize). AOSP's
         *  surface then keeps that size across a window resize, so the resize
         *  re-fire in macapkConvergeSurfaceLifecycle skips it. */
        boolean macapkFixedSize;
        @Override public void setType(int type)           {}
        @Override public android.graphics.Canvas lockCanvas() {
            macapkLockedCanvas = true;
            int w = frame.width(), h = frame.height();
            if (w <= 0 || h <= 0) { w = 800; h = 600; }   // sensible default
            if (drawBitmap == null || drawBitmap.getWidth() != w
                                       || drawBitmap.getHeight() != h) {
                drawBitmap = com.macapk.loader.gfx.GfxBackend.get().createFrameBitmap(w, h);
                drawCanvas = new android.graphics.Canvas(drawBitmap);
            }
            return drawCanvas;
        }
        @Override public android.graphics.Canvas lockCanvas(android.graphics.Rect dirty) {
            return lockCanvas();
        }
        @Override public android.graphics.Canvas lockHardwareCanvas() { return lockCanvas(); }
        @Override public void unlockCanvasAndPost(android.graphics.Canvas canvas) {
            // Push the painted bitmap to the screen via the
            // Cocoa-side CALayer overlay (sibling of the ANGLE Metal
            // layer). Read ARGB pixels via macapkArgbBuffer() which
            // extracts from the AWT BufferedImage that Canvas painted
            // into. v1.0.51 first wiring of the Canvas-to-Metal blit.
            if (drawBitmap == null) return;
            // v1.0.1648: the canvas's own live pixels when available (no 24 MB copy
            // at a full-screen window); presentBitmap copies synchronously on this,
            // the drawing thread, which is the contract macapkLiveArgbOrNull states.
            int[] pixels = GfxBackend.get().liveArgbOrNull(drawBitmap);
            if (pixels == null) pixels = GfxBackend.get().argbBuffer(drawBitmap);
            if (pixels == null) return;
            int pw = drawBitmap.getWidth(), ph = drawBitmap.getHeight();
            if (owner != null) {
                // 2026-09-26: show the frame WHERE its view is (see "where a
                // lockCanvas frame is shown" in SurfaceView): its own layer when
                // the runtime has one, else composited at the view's rect when
                // it is not display-sized; otherwise unchanged below.
                if (owner.macapkPresentThroughSurfaceLayer(pixels, pw, ph)) return;
                int[] placed = owner.macapkPlaceInDisplay(pixels, pw, ph);
                if (placed != null) {
                    pixels = placed;
                    pw = owner.macapkCompositeW;
                    ph = owner.macapkCompositeH;
                }
            }
            try {
                com.macapk.loader.NativeCocoa.presentBitmap(pixels, pw, ph);
            } catch (UnsatisfiedLinkError e) {
                // Native method not registered (headless build / older
                // runtime). Painting still happens to memory; just no
                // visible output. Logged once.
                if (!com.macapk.loader.SurfaceCanvasState.warnedMissingNative) {
                    com.macapk.loader.SurfaceCanvasState.warnedMissingNative = true;
                    System.err.println(
                        "[surfaceview] presentBitmap not registered; "
                        + "Canvas paints invisibly: " + e.getMessage());
                }
            }
        }

        /** The latest bitmap a lockCanvas consumer drew. */
        android.graphics.Bitmap macapkDrawBitmap() { return drawBitmap; }

        boolean hasCallbacks() { return !callbacks.isEmpty(); }

        void fireCreated() { fireCreatedTolerant(); }

        /** v1.0.572: per-callback tolerant surfaceCreated dispatch. Returns
         *  false when any callback threw -- the signal that the app is not
         *  ready to accept the surface yet (AGDK GameActivity's
         *  surfaceCreated calls a native method that only exists after
         *  initializeNativeCode ran; before that the call raises
         *  UnsatisfiedLinkError). The caller retries next frame WITHOUT
         *  latching, so the delivery converges instead of being consumed
         *  by a half-initialized listener. */
        boolean fireCreatedTolerant() {
            // v1.0.158 DIAGNOSTIC: log which callbacks receive
            // surfaceCreated. For Unity this is where com.unity3d.player.S
            // (the engine's SurfaceHolder.Callback) should fire and then
            // call nativeRecreateGfxState. If this logs 0 callbacks, Unity
            // never registered on the SurfaceView our framework is driving.
            java.util.ArrayList<Callback> snap = new java.util.ArrayList<>(callbacks);
            StringBuilder sb = new StringBuilder();
            for (Callback c : snap) {
                if (sb.length() > 0) sb.append(", ");
                sb.append(c.getClass().getName());
            }
            NativeLog.i(TAG, "holder.fireCreated: " + snap.size()
                + " callback(s): [" + sb + "]");
            boolean ok = true;
            for (Callback c : snap) {
                try {
                    c.surfaceCreated(this);
                } catch (Throwable t) {
                    ok = false;
                    NativeLog.e(TAG, "surfaceCreated rejected by "
                        + c.getClass().getName() + ": " + t
                        + " (will retry next frame)");
                }
            }
            return ok;
        }
        void fireChanged(int format, int w, int h) {
            // The frame is the holder's STATE and is written where the change is
            // decided (getSurfaceFrame() answers at once, and the per-frame size
            // comparisons do not re-fire); the CALLBACKS go to the UI thread when
            // this runs off it (SV_UI_THREAD_CALLBACKS, above), behind any posted
            // surfaceCreated, so the order stays created -> changed.
            frame.set(0, 0, w, h);
            if (macapkCallbackOffUiThread()) {
                macapkPostToUiThread(() -> {
                    try {
                        for (Callback c : new java.util.ArrayList<>(callbacks)) c.surfaceChanged(this, format, w, h);
                    } catch (Throwable t) {
                        NativeLog.e(TAG, "UI-thread surfaceChanged threw: " + t);
                    }
                });
                return;
            }
            for (Callback c : new java.util.ArrayList<>(callbacks)) c.surfaceChanged(this, format, w, h);
        }
        void fireDestroyed() {
            if (macapkCallbackOffUiThread()) {
                macapkPostToUiThread(() -> {
                    try {
                        for (Callback c : new java.util.ArrayList<>(callbacks)) c.surfaceDestroyed(this);
                    } catch (Throwable t) {
                        NativeLog.e(TAG, "UI-thread surfaceDestroyed threw: " + t);
                    }
                });
                return;
            }
            for (Callback c : new java.util.ArrayList<>(callbacks)) c.surfaceDestroyed(this);
        }
    }

    // The AttachedSurfaceControl of the window this SurfaceView roots into.
    // Mac.apk has one composited window and no SurfaceControl transaction
    // pipeline, so this is a stable no-op host (applyTransactionOnDraw defaults
    // to "not deferred"). API 30.
    private final AttachedSurfaceControl mRootSurfaceControl = new AttachedSurfaceControl() { };

    public AttachedSurfaceControl getRootSurfaceControl() { return mRootSurfaceControl; }

    public boolean onTrackballEvent(android.view.MotionEvent event) { return false; }

    /** AOSP View.onPointerCaptureChange(boolean): "Called when the window
     *  has just acquired or lost pointer capture." Empty in AOSP itself --
     *  it exists purely as an overridable hook that
     *  View.dispatchPointerCaptureChanged() forwards to. Declared directly
     *  on SurfaceView, not the android.view.View base it extends, because
     *  that base is out of this change's scope; a SurfaceView subclass
     *  (openttd's engine wrapper) that overrides this and calls
     *  super.onPointerCaptureChange(hasCapture) -- the normal AOSP-idiom
     *  override -- needs some class in its supertype chain to resolve the
     *  super call against. */
    public void onPointerCaptureChange(boolean hasCapture) {}

    // ======================================================================
    //  AOSP API-36 surface completion (Mac.apk API-gap lane, android.view).
    // ======================================================================

    /** AOSP SurfaceView.getSurfaceControl(): the SurfaceControl backing this
     *  view's dedicated compositor layer. Mac.apk draws SurfaceView content into
     *  the SAME ANGLE/Metal layer as the rest of the window rather than a
     *  separate SurfaceFlinger layer, so there is no layer of its own -- but the
     *  handle must be non-null and STABLE, because the standard idiom is
     *  `new SurfaceControl.Transaction().setPosition(sv.getSurfaceControl(), ...)`
     *  and a null there is an immediate NPE inside the app. The transaction is a
     *  recording no-op (see SurfaceControl.Transaction), so a caller gets the
     *  same "queued and applied" answer it would on a device; what it does not
     *  get is a visible effect, which is already true of every other
     *  SurfaceControl operation in this tree. Referenced by YouTube. */
    private SurfaceControl mSurfaceControl;

    /** AOSP onSetSurfacePositionAndScale (hidden; InlineContentView overrides it and calls it): the
     *  surface's place and scale over the view, in the transaction. AOSP's body. */
    protected void onSetSurfacePositionAndScale(SurfaceControl.Transaction transaction, SurfaceControl surface,
            int positionLeft, int positionTop, float postScaleX, float postScaleY) {
        transaction.setPosition(surface, positionLeft, positionTop);
        transaction.setMatrix(surface, postScaleX, 0f, 0f, postScaleY);
    }

    public SurfaceControl getSurfaceControl() {
        if (mSurfaceControl == null) {
            mSurfaceControl = new SurfaceControl.Builder()
                    .setName("SurfaceView[" + getClass().getSimpleName() + "]")
                    .build();
        }
        return mSurfaceControl;
    }

    /** AOSP SurfaceView.applyTransactionToFrame(SurfaceControl.Transaction):
     *  apply the transaction atomically WITH the next buffer this view posts.
     *  With no compositor to synchronise against, applying it now is the same
     *  observable outcome -- and dropping it would strand any committed
     *  listener the caller attached. */
    public void applyTransactionToFrame(SurfaceControl.Transaction t) {
        if (t != null) t.apply();
    }

    /** AOSP SurfaceView.setChildSurfacePackage(SurfacePackage) /
     *  getChildSurfacePackage() / clearChildSurfacePackage(): embed another
     *  process's SurfaceControlViewHost hierarchy inside this view. Mac.apk is
     *  a single process with nothing to embed (see SurfaceControlViewHost's
     *  header); the package round-trips so a caller reads back what it set, and
     *  nothing is composited -- the same as an embedded package whose remote
     *  end never produced a frame. */
    private SurfaceControlViewHost.SurfacePackage mChildSurfacePackage;
    public void setChildSurfacePackage(SurfaceControlViewHost.SurfacePackage p) {
        mChildSurfacePackage = p;
    }
    public SurfaceControlViewHost.SurfacePackage getChildSurfacePackage() {
        return mChildSurfacePackage;
    }
    public void clearChildSurfacePackage() { mChildSurfacePackage = null; }

    /** AOSP SurfaceView.setCompositionOrder(int) / getCompositionOrder()
     *  (API 36): the z-order of this view's layer relative to the window,
     *  superseding setZOrderOnTop / setZOrderMediaOverlay. Negative is behind
     *  the window, positive in front; AOSP's default is 0. */
    private int mCompositionOrder;
    public void setCompositionOrder(int order) {
        mCompositionOrder = order;
        // Keep the pre-existing z-order flags this class already honours in
        // agreement with the new API, rather than leaving two disagreeing
        // sources: AOSP documents setCompositionOrder as the replacement for
        // exactly these two calls.
        setZOrderOnTop(order > 0);
    }
    public int getCompositionOrder() { return mCompositionOrder; }

    /** AOSP SurfaceView.getHostToken(), deprecated at API 35: the IBinder a
     *  remote SurfaceControlViewHost would be created against. Nothing embeds
     *  into this view (see setChildSurfacePackage), so AOSP's own "not attached"
     *  answer -- null -- is the honest one. */
    @Deprecated
    public android.os.IBinder getHostToken() { return null; }

    /** AOSP SurfaceView.setDesiredHdrHeadroom(float) (API 35): validation is
     *  AOSP's own; the value is stored and, as on an SDR display, has no effect. */
    private float mDesiredHdrHeadroom;
    public void setDesiredHdrHeadroom(float desiredHeadroom) {
        if (!Float.isFinite(desiredHeadroom)) {
            throw new IllegalArgumentException("desiredHeadroom must be finite: "
                    + desiredHeadroom);
        }
        if (desiredHeadroom < 0 || desiredHeadroom > 10000.0f) {
            throw new IllegalArgumentException("desiredHeadroom must be in the range "
                    + "[0, 10000.0f], received: " + desiredHeadroom);
        }
        mDesiredHdrHeadroom = desiredHeadroom;
    }
}
