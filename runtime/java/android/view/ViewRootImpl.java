// android.view.ViewRootImpl -- Tier 3 M6: the schedule authority that lets the
// view tree be traversed (measured/laid out/drawn) ONLY WHEN A VIEW ASKS,
// instead of brute-forcing a full measure->layout->draw every single frame.
//
// Real Android: ViewRootImpl owns the window's root view + attach/insets/focus,
// and a scheduleTraversals() that invalidate()/requestLayout() call to request a
// traversal on the next Choreographer CALLBACK_TRAVERSAL; performTraversals()
// (measure -> layout -> draw) runs ONLY when mTraversalScheduled was set, then
// clears it (see frameworks/base ViewRootImpl.requestLayout/invalidate/
// scheduleTraversals/doTraversal). Idle window -> zero traversals.
//
// In Mac.apk the traversal BODY already lives in ViewCanvasDriver (it runs
// measure->layout->draw->present on the ONE main loop each Metal vsync). What
// was missing is the GATE: ViewCanvasDriver traversed unconditionally every
// frame (the ~15 enforce/fixup hacks re-assert geometry every frame on top of
// it). This class is that gate -- "has anyone asked for a traversal since the
// last frame?" -- so the driver can skip the whole pass when the tree is clean.
//
// Single-window model (one NSWindow, one root) -> a process singleton. Every
// call site is gated by MACAPK_REAL_LOOPER, so flag-off keeps the historical
// every-frame traversal byte-for-byte.
//
// Scope note (M6 is the first increment): this owns the traversal SCHEDULE.
// Owning attach/insets/focus and routing through Choreographer.CALLBACK_TRAVERSAL
// for animation timing migrate here in M7; retiring the per-frame hacks that the
// gate makes redundant is M8.
package android.view;

import java.util.concurrent.atomic.AtomicBoolean;

public final class ViewRootImpl {
    private static ViewRootImpl sInstance;

    public static synchronized ViewRootImpl getInstance() {
        if (sInstance == null) sInstance = new ViewRootImpl();
        return sInstance;
    }

    // Start scheduled so the very first frame always traverses (initial layout).
    private final AtomicBoolean mTraversalScheduled = new AtomicBoolean(true);
    // Tier 3 (scroll perf): a traversal that needs measure+layout, not just a
    // redraw. AOSP ViewRootImpl.performTraversals runs performMeasure/perform-
    // Layout ONLY when mLayoutRequested (set by View.requestLayout); a plain
    // invalidate() (set by scroll's offsetChildrenVertical, animations, ripples)
    // schedules a DRAW pass and leaves the stable layout untouched. We mirror
    // that split so a RecyclerView/ScrollView scroll frame is draw-only (O(visible)
    // redraw) instead of a full-tree re-measure+re-layout every frame. Starts true
    // so the first frame lays out. See ViewCanvasDriver.doFrame.
    private final AtomicBoolean mLayoutRequested = new AtomicBoolean(true);
    private View mView;

    private ViewRootImpl() {}

    /** Set/replace the window's root view (a new content view needs a full
     *  measure+layout traversal). */
    public void setView(View root) {
        mView = root;
        macapkDamageAll();
        mTraversalScheduled.set(true);
        mLayoutRequested.set(true);
    }
    public View getView() { return mView; }

    /** invalidate() (and touch / surface-change redraw) calls this to ask for a
     *  measure-skipping DRAW on the next frame. Coalesced / idempotent. */
    public void scheduleTraversal() { macapkDamageAll(); mTraversalScheduled.set(true); }

    /** View.requestLayout (and setVisibility / setPadding / adapter changes) calls
     *  this to ask for a full measure+layout+draw traversal on the next frame.
     *  Implies a traversal too. AOSP: requestLayout sets mLayoutRequested AND
     *  scheduleTraversals. */
    public void requestLayoutTraversal() {
        macapkDamageAll();
        mLayoutRequested.set(true);
        mTraversalScheduled.set(true);
    }

    public boolean isTraversalScheduled() { return mTraversalScheduled.get(); }
    public boolean isLayoutRequested()   { return mLayoutRequested.get(); }

    /** The driver calls this once per frame: returns true AND clears the flag iff
     *  a traversal is pending. (AOSP doTraversal: clear mTraversalScheduled, then
     *  performTraversals.) */
    public boolean consumeTraversalScheduled() { return mTraversalScheduled.getAndSet(false); }

    /** The driver calls this each traversal: returns true AND clears the flag iff a
     *  measure+layout was requested since the last frame. False => a draw-only frame
     *  is sufficient (AOSP performTraversals gates measure/layout on this). */
    public boolean consumeLayoutRequested() { return mLayoutRequested.getAndSet(false); }

    /** Convenience for the single-window case: schedule a DRAW (invalidate). */
    public static void schedule() { if (INVAL_CENSUS) census(""); getInstance().scheduleTraversal(); }
    /** An invalidation that asked for no frame because the view cannot be drawn
     *  (View.macapkCannotBeDrawn). Only counted: the census lists it as
     *  `skipped <entry> | <app frame>`, so a run shows what it saved. */
    static void macapkInvalidateSkipped() { if (INVAL_CENSUS) census("skipped "); }
    // MACAPK_TRACE_INVALIDATE=1 (diagnostic): WHO keeps the window dirty. Every
    // schedule() is attributed to its framework entry (View.invalidate,
    // Drawable.invalidateSelf ...) and the first APP frame that called it, and every
    // 5 s the 8 commonest pairs are logged as `[inval-census]` (tag Mac.apk.Inval).
    // A pair that starts with `skipped` asked for no frame (macapkInvalidateSkipped).
    // An app that redraws every frame on a static screen shows its animators here:
    // Telegram's chat list (2026-10-01) is a SpoilerEffect cell, ActionBar
    // .setTranslationY and MainTabsActivity's blur fade, 36 each per 5 s -- small
    // views, so the whole-window repaint each of them causes is OUR cost (no damage
    // rects yet: docs/damage-aware-partial-redraw-design_2026-08-30.md S1).
    static final boolean INVAL_CENSUS = "1".equals(System.getenv("MACAPK_TRACE_INVALIDATE"));
    private static final java.util.HashMap<String, int[]> sCensus = new java.util.HashMap<>();
    private static long sCensusT0;
    private static void census(String prefix) {
        StackTraceElement[] st = new Throwable().getStackTrace();
        String entry = "?", app = "?";
        for (int i = 2; i < st.length; i++) {
            String c = st[i].getClassName();
            if (st[i].getMethodName().equals("macapkScheduleRedraw")) continue;   // name its caller
            boolean fw = c.startsWith("android.") || c.startsWith("com.macapk.") || c.startsWith("java.") || c.startsWith("androidx.");
            if (entry.equals("?")) entry = c.substring(c.lastIndexOf('.') + 1) + "." + st[i].getMethodName();
            if (!fw) { app = c + "." + st[i].getMethodName() + ":" + st[i].getLineNumber()
                    + (i + 1 < st.length ? " <- " + st[i + 1].getClassName() + "." + st[i + 1].getMethodName() : ""); break; }
        }
        long now = System.nanoTime();
        synchronized (sCensus) {
            sCensus.computeIfAbsent(prefix + entry + " | " + app, k -> new int[1])[0]++;
            if (sCensusT0 == 0) sCensusT0 = now;
            if (now - sCensusT0 > 5_000_000_000L) {
                StringBuilder sb = new StringBuilder("[inval-census] 5s:");
                sCensus.entrySet().stream().sorted((a, b) -> b.getValue()[0] - a.getValue()[0]).limit(8)
                    .forEach(e -> sb.append("\n  ").append(e.getValue()[0]).append("  ").append(e.getKey()));
                com.macapk.loader.NativeLog.i("Mac.apk.Inval", sb.toString());
                sCensus.clear(); sCensusT0 = now;
            }
        }
    }
    /** Convenience for the single-window case: schedule a measure+layout+draw. */
    public static void scheduleLayout() { getInstance().requestLayoutTraversal(); }

    // ------------------------------------------------------------------
    // FRAMESPEED S1: WHERE the window is dirty, for ViewCanvasDriver's
    // damage-clipped draw-only frames (S2; its FRAMESPEED_DAMAGE block states
    // the contract). AOSP's software path does the same thing with
    // ViewRootImpl.mDirty and Surface.lockCanvas(dirty).
    //
    // The damage of a frame is NONE, one device rectangle, or ALL. It is ALL
    // unless every request for the frame said where: scheduleTraversal(),
    // requestLayoutTraversal() and setView() -- what everything but
    // View.invalidate calls -- make it ALL, and only View.macapkScheduleRedraw
    // asks through macapkScheduleDamage with a rectangle.
    //
    // The rectangle of an invalidated view is not computed from the tree. It is
    // what the last frame saw: ViewGroup.drawChild records, just before
    // child.draw, the clip the child was drawn under on the frame canvas, as
    // device pixels (View.macapkRecordDrawn). Whatever the child and its
    // subtree put on the frame lies inside that clip, since a clip is only ever
    // narrowed (Canvas has no REPLACE or UNION clip op), so redrawing the tree
    // clipped to it shows everything a change inside the view can change, with
    // no reasoning about transforms, clipChildren, overlap or translucency.
    // View.macapkReportDamage states when a record may be used and which
    // ancestor's is used instead when the view's own may not.
    //
    // A FULL frame (sMacapkEpoch counts them) records every view it draws; a
    // record from an older epoch is void. A PARTIAL frame draws under the damage
    // clip, so what a view sees there is its clip cut by the damage: only views
    // inside a view whose record made up the damage (a "source", mMacapkDamageSeq)
    // are recorded again, because their true clip lies inside the damage and is
    // therefore seen whole. That keeps the records right through a scroll or an
    // animation that redraws only its container.
    //
    // MACAPK_FRAMESPEED_DAMAGE (the driver's flag; "1" = on, anything else off
    // -- the default until a sweep with MACAPK_DAMAGE_VALIDATE=1 is clean) turns
    // both halves off together: no record is taken and every frame is ALL.
    // MACAPK_TRACE_DAMAGE=1 logs every 5 s what the frames' damage was.
    // ------------------------------------------------------------------
    public static final boolean MACAPK_DAMAGE = macapkDamageGate(System.getenv("MACAPK_FRAMESPEED_DAMAGE"));
    /** The flag's reading; ViewCanvasDriver gates S2 on the same answer. */
    public static boolean macapkDamageGate(String env) { return "1".equals(env); }
    static final boolean TRACE_DAMAGE = "1".equals(System.getenv("MACAPK_TRACE_DAMAGE"));
    private static final int DAMAGE_NONE = 0, DAMAGE_RECT = 1, DAMAGE_ALL = 2;   // S2's DAMAGE_*

    private final Object mDamageLock = new Object();
    private int mDamageKind = DAMAGE_ALL;   // the first frame draws everything
    private int mDamageL, mDamageT, mDamageR, mDamageB;
    private int mDragDamageFrames;
    // What the last frame's traversal raised: see macapkEndFrameDamage.
    private int mCarryKind = DAMAGE_NONE, mCarryL, mCarryT, mCarryR, mCarryB;

    // The draw side. UI thread only: an invalidate from another thread is ALL.
    /** Counts FULL frame draws; a view's record is good for the epoch it was taken in. */
    static int sMacapkEpoch = 1;
    /** Counts frame draws, so that a view drawn twice in one frame is seen as such. */
    static int sMacapkDrawSeq = 1;
    /** The damage being collected; advanced when the driver takes it. */
    static int sMacapkDamageSeq = 1;
    /** The damage the frame being drawn was given (its sources carry this number). */
    static int sMacapkDrawSourceSeq = -1;
    /** The frame canvas while the driver draws the root view, else null. */
    static android.graphics.Canvas sMacapkFrameCanvas;
    static boolean sMacapkPartial;
    /** How many sources the view being drawn is inside of (partial frames). */
    static int sMacapkUnderSource;
    /** The view whose draw(Canvas) its parent's drawChild is about to call. */
    static View sMacapkDrawTarget;
    static final int[] sMacapkScratch6 = new int[6];
    private static final int[] sDamageStats = new int[5];   // frames taken: none, rect, all; rect requests; follow-ups
    private static long sDamageStatsArea, sDamageStatsT0;

    private void macapkDamageAll() {
        if (!MACAPK_DAMAGE) return;
        synchronized (mDamageLock) { mDamageKind = DAMAGE_ALL; }
    }

    /** Whether a rectangle can still narrow this frame: false once it is ALL. */
    boolean macapkDamageOpen() {
        if (!MACAPK_DAMAGE) return false;
        synchronized (mDamageLock) { return mDamageKind != DAMAGE_ALL; }
    }

    /** View.invalidate's request for a frame that redraws this device rectangle. */
    void macapkScheduleDamage(int l, int t, int r, int b) {
        if (INVAL_CENSUS) census("");
        synchronized (mDamageLock) {
            macapkUnion(DAMAGE_RECT, l, t, r, b);
            if (TRACE_DAMAGE) sDamageStats[3]++;
        }
        mTraversalScheduled.set(true);
    }

    /** A view whose damage is already in this frame's rectangle asks again. */
    public void macapkScheduleSameDamage() {
        if (INVAL_CENSUS) census("");
        mTraversalScheduled.set(true);
    }

    /** What ViewCanvasDriver (S2) takes with the traversal flag at the top of a
     *  frame: the damage since the last frame. Returns 0 = none, 1 = out4 holds
     *  {left, top, right, bottom} in pixels of the frame bitmap, 2 = everything;
     *  and starts collecting anew. What the last frame's own traversal raised
     *  (macapkEndFrameDamage) is part of the answer. */
    public int macapkConsumeDamage(int[] out4) {
        if (!MACAPK_DAMAGE) return DAMAGE_ALL;
        int kind;
        synchronized (mDamageLock) {
            macapkUnion(mCarryKind, mCarryL, mCarryT, mCarryR, mCarryB);
            mCarryKind = DAMAGE_NONE;
            kind = mDamageKind;
            if (kind == DAMAGE_RECT) {
                if (out4 == null || out4.length < 4) kind = DAMAGE_ALL;
                else { out4[0] = mDamageL; out4[1] = mDamageT; out4[2] = mDamageR; out4[3] = mDamageB; }
            }
            mDamageKind = DAMAGE_NONE;
            sMacapkDrawSourceSeq = sMacapkDamageSeq++;
        }
        // A drag's shadow is drawn over the tree wherever the pointer is, by no
        // view; the two frames after the drag take it away.
        if (MacApkDragSession.activeRoot() != null) mDragDamageFrames = 3;
        if (mDragDamageFrames > 0) { mDragDamageFrames--; kind = DAMAGE_ALL; }
        return kind;
    }

    /** mDamage |= the given damage. Caller holds mDamageLock. */
    private void macapkUnion(int kind, int l, int t, int r, int b) {
        if (kind == DAMAGE_NONE || mDamageKind == DAMAGE_ALL) return;
        if (kind == DAMAGE_ALL) { mDamageKind = DAMAGE_ALL; return; }
        if (mDamageKind == DAMAGE_NONE) {
            mDamageKind = DAMAGE_RECT;
            mDamageL = l; mDamageT = t; mDamageR = r; mDamageB = b;
        } else {
            mDamageL = Math.min(mDamageL, l); mDamageT = Math.min(mDamageT, t);
            mDamageR = Math.max(mDamageR, r); mDamageB = Math.max(mDamageB, b);
        }
    }

    /** The end of a frame. The driver drops the traversal flag the frame's own
     *  traversal raised (a view that invalidates in onDraw must not keep the
     *  window drawing for ever); when every frame drew the whole tree, dropping
     *  the request lost nothing, because the next frame, whenever it came, drew
     *  that view again. To keep that true the damage raised since
     *  macapkConsumeDamage is not dropped: it waits, asking for no frame, and is
     *  part of the next frame's damage.
     *
     *  `drawn` is the rectangle a partial frame was clipped to, null after a full
     *  one. A partial frame can leave such a change half drawn or not drawn at
     *  all -- an OnPreDrawListener moved a view, a scroller advanced in
     *  computeScroll -- where a full frame would have shown it. So when the
     *  damage reaches outside `drawn` the answer is true: the driver asks for one
     *  more frame, whose damage also covers `drawn`, so a second one is never
     *  needed for the same change. */
    public boolean macapkEndFrameDamage(int[] drawn) {
        if (!MACAPK_DAMAGE) return false;
        synchronized (mDamageLock) {
            final boolean escaped = drawn != null && (mDamageKind == DAMAGE_ALL || (mDamageKind == DAMAGE_RECT
                    && (mDamageL < drawn[0] || mDamageT < drawn[1] || mDamageR > drawn[2] || mDamageB > drawn[3])));
            if (escaped) macapkUnion(DAMAGE_RECT, drawn[0], drawn[1], drawn[2], drawn[3]);
            mCarryKind = mDamageKind;
            mCarryL = mDamageL; mCarryT = mDamageT; mCarryR = mDamageR; mCarryB = mDamageB;
            mDamageKind = DAMAGE_NONE;
            if (TRACE_DAMAGE && escaped) sDamageStats[4]++;
            return escaped;
        }
    }

    /** The driver is about to call root.draw(canvas) for a frame: `partial` when
     *  the canvas is clipped to the damage. Paired with macapkEndDraw. */
    public static void macapkBeginDraw(View root, android.graphics.Canvas canvas, boolean partial, int kind,
                                       int[] rect) {
        if (!MACAPK_DAMAGE || root == null || canvas == null) return;
        sMacapkFrameCanvas = canvas;
        sMacapkPartial = partial;
        sMacapkDrawSeq++;
        if (!partial) sMacapkEpoch++;
        sMacapkUnderSource = 0;
        root.macapkBeforeDraw(canvas);
        if (TRACE_DAMAGE) damageStats(partial, kind, rect, canvas);
    }

    public static void macapkEndDraw() {
        sMacapkFrameCanvas = null;
        sMacapkDrawTarget = null;
        sMacapkUnderSource = 0;
    }

    /** `[damage] 5s: frames none=.. rect=.. all=.. drawn-partial=.. (x% of the frame each), rect requests=..,
     *  follow-up frames=..` */
    private static int sDamageStatsPartial;
    private static void damageStats(boolean partial, int kind, int[] rect, android.graphics.Canvas canvas) {
        final long now = System.nanoTime();
        if (kind >= 0 && kind <= 2) sDamageStats[kind]++;
        if (partial && rect != null) {
            sDamageStatsPartial++;
            sDamageStatsArea += (long) (rect[2] - rect[0]) * (rect[3] - rect[1]);
        }
        if (sDamageStatsT0 == 0) sDamageStatsT0 = now;
        if (now - sDamageStatsT0 < 5_000_000_000L) return;
        final double frame = Math.max(1.0, (double) canvas.getWidth() * canvas.getHeight());
        com.macapk.loader.NativeLog.i("Mac.apk.Damage", "[damage] 5s: frames none=" + sDamageStats[0]
            + " rect=" + sDamageStats[1] + " all=" + sDamageStats[2] + " drawn-partial=" + sDamageStatsPartial
            + (sDamageStatsPartial > 0 ? String.format(" (%.1f%% of the frame each)",
                    100.0 * sDamageStatsArea / sDamageStatsPartial / frame) : "")
            + ", rect requests=" + sDamageStats[3] + ", follow-up frames=" + sDamageStats[4]);
        java.util.Arrays.fill(sDamageStats, 0);
        sDamageStatsPartial = 0; sDamageStatsArea = 0; sDamageStatsT0 = now;
    }

    // ------------------------------------------------------------------
    // TOUCH MODE (AOSP ViewRootImpl.ensureTouchMode / enterTouchMode /
    // leaveTouchMode, android14-release :6088-6195).
    //
    // A device is in touch mode after a touch and leaves it on a D-pad,
    // arrow, Tab or typing key; outside touch mode every FOCUSABLE view can
    // take focus (not only focusableInTouchMode ones) and focus moves with the
    // arrows. Since Android 14 the state is per DISPLAY and shared by every
    // window on it (WindowManager.setInTouchMode(inTouchMode, displayId)), and
    // we have one display, so it is one process-wide value. It starts at
    // config_defaultInTouchMode, which is TRUE on a phone or tablet (false on
    // a TV); we present a normal touch device, so it starts true.
    //
    // ⚠ View.isInTouchMode() was a constant true, so the arrow keys could
    // never put focus on a view that is focusable but not focusableInTouchMode
    // -- every androidx.leanback card, row and header, and every plain Button.
    // A TV app was unusable from the keyboard: SmartTube took every arrow and
    // returned false from dispatchKeyEvent because nothing held focus.
    // ------------------------------------------------------------------
    private static volatile boolean sInTouchMode = true;

    /** AOSP View.isInTouchMode reads mAttachInfo.mInTouchMode for an attached
     *  view, which is this. */
    public static boolean macapkInTouchMode() { return sInTouchMode; }

    /** AOSP ensureTouchMode(inTouchMode) for the window whose root is `root`:
     *  returns true when the mode changed AND focus changed as a result (the
     *  caller then consumes the key that caused it). */
    public static boolean macapkEnsureTouchMode(View root, boolean inTouchMode) {
        if (sInTouchMode == inTouchMode) return false;
        sInTouchMode = inTouchMode;
        if (root == null) return false;
        try { root.getViewTreeObserver().dispatchOnTouchModeChanged(inTouchMode); }
        catch (Throwable ignored) { }
        return inTouchMode ? enterTouchMode(root) : leaveTouchMode(root);
    }

    /** AOSP enterTouchMode: a focused view that cannot hold focus in touch
     *  mode hands it to an ancestor that wants it after its descendants and
     *  is focusable in touch mode, or loses it with nothing in its place. */
    private static boolean enterTouchMode(View root) {
        if (!root.hasFocus()) return false;
        View focused = root.findFocus();
        if (focused == null || focused.isFocusableInTouchMode()) return false;
        ViewGroup ancestor = findAncestorToTakeFocusInTouchMode(focused);
        if (ancestor != null) return ancestor.requestFocus();
        focused.clearFocus();
        return true;
    }

    private static ViewGroup findAncestorToTakeFocusInTouchMode(View focused) {
        ViewParent parent = focused.getParent();
        while (parent instanceof ViewGroup vg) {
            if (vg.getDescendantFocusability() == ViewGroup.FOCUS_AFTER_DESCENDANTS
                    && vg.isFocusableInTouchMode()) {
                return vg;
            }
            if (vg.isRootNamespace()) return null;
            parent = vg.getParent();
        }
        return null;
    }

    /** AOSP leaveTouchMode: a focused leaf (or a group that does not prefer
     *  its children) keeps focus; otherwise the window's default focus is
     *  restored, which in non-touch mode reaches every focusable view. */
    private static boolean leaveTouchMode(View root) {
        if (root.hasFocus()) {
            View focused = root.findFocus();
            if (!(focused instanceof ViewGroup fg)) return false;
            if (fg.getDescendantFocusability() != ViewGroup.FOCUS_AFTER_DESCENDANTS) return false;
        }
        return root.restoreDefaultFocus();
    }
}
