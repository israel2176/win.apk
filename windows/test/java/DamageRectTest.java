// DamageRectTest -- checks what an invalidation damages (android.view.ViewRootImpl,
// "FRAMESPEED S1"; View.macapkReportDamage) with the real View and ViewGroup
// classes, and that a frame drawn clipped to that damage has the pixels of a
// frame drawn whole. It plays ViewCanvasDriver's part (S2) the way the driver
// does: take the damage with macapkConsumeDamage, clip the frame canvas to the
// rectangle, clear, bracket root.draw with macapkBeginDraw / macapkEndDraw, and
// end the frame with macapkEndFrameDamage.
//
//   javac -cp "<runtime>/lib/android-framework.jar;<runtime>/lib/macapk-loader.jar" -d out \
//         windows/test/java/DamageRectTest.java
//   MACAPK_FRAMESPEED_DAMAGE=1 MACAPK_LOG_LEVEL=7 java -Djava.awt.headless=true \
//         -cp "<runtime>/lib/android-framework.jar;<runtime>/lib/macapk-loader.jar;out" DamageRectTest
//
// Exits 0 when every check passes (and at once, with a note, when the flag is
// not set: partial frames are then off and there is nothing to check). No
// window, no native runtime: MACAPK_LOG_LEVEL=7 keeps the framework from
// logging, which needs the runtime's NativeLog.

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewRootImpl;

import java.util.Arrays;

public final class DamageRectTest {
    private static final int W = 400, H = 600, CLEAR = 0xff101010;
    private static final int NONE = 0, RECT = 1, ALL = 2;
    private static int failures;

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "[PASS] " : "[FAIL] ") + what);
        if (!ok) failures++;
    }

    /** A view that fills its bounds (and, with `spill`, 30 px around them) with a color. */
    static class Box extends View {
        int color;
        boolean spill, invalidateInDraw;
        Runnable inDraw;
        final Paint paint = new Paint();
        Box(int color) { super(null); this.color = color; }
        @Override protected void onDraw(Canvas canvas) {
            paint.setColor(color);
            if (spill) canvas.drawRect(-30, -30, getWidth() + 30, getHeight() + 30, paint);
            else canvas.drawRect(0, 0, getWidth(), getHeight(), paint);
            if (invalidateInDraw) invalidate();
            if (inDraw != null) inDraw.run();
        }
    }

    /** A group that places its children where the test put them, over its own color. */
    static class Group extends ViewGroup {
        int color;
        View drawsItself;   // a child this group draws with child.draw(canvas), not drawChild
        final Paint paint = new Paint();
        Group(int color) { super(null); this.color = color; setWillNotDraw(false); }
        @Override protected void onLayout(boolean changed, int l, int t, int r, int b) { }
        @Override protected void onDraw(Canvas canvas) {
            if (color == 0) return;
            paint.setColor(color);
            canvas.drawRect(0, 0, getWidth(), getHeight(), paint);
        }
        @Override protected boolean drawChild(Canvas canvas, View child, long time) {
            if (child != drawsItself) return super.drawChild(canvas, child, time);
            canvas.save();
            canvas.translate(child.getLeft() + 200, child.getTop());
            child.draw(canvas);
            canvas.restore();
            return false;
        }
    }

    static Box box(Group parent, int color, int l, int t, int r, int b) {
        Box v = new Box(color);
        parent.addView(v);
        v.layout(l, t, r, b);
        return v;
    }
    static Group group(Group parent, int color, int l, int t, int r, int b) {
        Group g = new Group(color);
        if (parent != null) parent.addView(g);
        g.layout(l, t, r, b);
        return g;
    }

    // ---- the driver's part ----
    static final ViewRootImpl vri = ViewRootImpl.getInstance();
    static Group root;
    static Bitmap frame;
    static Canvas canvas;
    static final int[] dmg = new int[4];
    static int lastKind;
    static boolean lastPartial, lastFollowUp;

    static void clearFlags() { vri.consumeTraversalScheduled(); vri.consumeLayoutRequested(); }

    /** One frame as the driver draws it: partial when the damage is a rectangle
     *  and no layout was asked for. Returns the damage kind it was given. */
    static int frame() {
        final boolean scheduled = vri.consumeTraversalScheduled();
        final boolean layout = vri.consumeLayoutRequested();
        lastKind = vri.macapkConsumeDamage(dmg);
        final boolean partial = scheduled && !layout && lastKind == RECT
                && Math.min(W, dmg[2]) > Math.max(0, dmg[0]) && Math.min(H, dmg[3]) > Math.max(0, dmg[1]);
        final int[] rect = { Math.max(0, dmg[0]), Math.max(0, dmg[1]), Math.min(W, dmg[2]), Math.min(H, dmg[3]) };
        final int save = canvas.save();
        if (partial) canvas.macapkClipChildRect(rect[0], rect[1], rect[2], rect[3]);
        canvas.drawColor(CLEAR);
        ViewRootImpl.macapkBeginDraw(root, canvas, partial, lastKind, partial ? rect : null);
        try { root.draw(canvas); } finally { ViewRootImpl.macapkEndDraw(); }
        canvas.restoreToCount(save);
        vri.consumeTraversalScheduled();
        lastFollowUp = vri.macapkEndFrameDamage(partial ? rect : null);
        if (lastFollowUp) vri.macapkScheduleSameDamage();
        lastPartial = partial;
        return lastKind;
    }

    /** The tree drawn whole on a canvas of its own (no record is touched). */
    static int[] reference() {
        Bitmap b = Bitmap.createBitmapWithCanvasBacking(W, H);
        Canvas c = new Canvas(b);
        c.drawColor(CLEAR);
        root.draw(c);
        return b.macapkArgbBuffer().clone();
    }

    static boolean framePixelsAre(int[] want, String what) {
        int[] got = frame.macapkArgbBuffer();
        int bad = 0, x0 = W, y0 = H, x1 = -1, y1 = -1;
        for (int i = 0; i < W * H; i++) {
            if (got[i] != want[i]) {
                bad++;
                x0 = Math.min(x0, i % W); x1 = Math.max(x1, i % W);
                y0 = Math.min(y0, i / W); y1 = Math.max(y1, i / W);
            }
        }
        if (bad != 0) {
            System.out.println("       " + what + ": " + bad + " px differ, spanning " + x0 + "," + y0 + "-"
                    + (x1 + 1) + "," + (y1 + 1));
        }
        return bad == 0;
    }

    /** Draw the frame the change asks for and compare it with the tree drawn whole. */
    static void frameMatches(String what, boolean wantPartial) {
        frame();
        final boolean same = framePixelsAre(reference(), what);
        check(same && lastPartial == wantPartial, what + ": " + (lastPartial ? "partial " + Arrays.toString(dmg)
                : "full") + " frame equals the tree drawn whole" + (lastPartial == wantPartial ? "" : " (expected "
                + (wantPartial ? "a partial" : "a full") + " frame)"));
    }

    static boolean covers(int l, int t, int r, int b) {
        return dmg[0] <= l && dmg[1] <= t && dmg[2] >= r && dmg[3] >= b;
    }
    /** The damage is the rectangle, give or take the pixel of slack a record has. */
    static boolean about(int l, int t, int r, int b) {
        return covers(l, t, r, b) && dmg[0] >= l - 1 && dmg[1] >= t - 1 && dmg[2] <= r + 1 && dmg[3] <= b + 1;
    }
    static int peek() {   // the damage, without drawing
        clearFlags();
        return vri.macapkConsumeDamage(dmg);
    }
    static void fullFrame() {
        root.invalidate();
        vri.requestLayoutTraversal();
        frame();
        if (lastFollowUp) frame();
    }

    public static void main(String[] args) throws Exception {
        if (!ViewRootImpl.MACAPK_DAMAGE) {
            System.out.println("DamageRectTest: MACAPK_FRAMESPEED_DAMAGE is not 1, partial frames are off;"
                    + " nothing to check");
            return;
        }
        android.os.Looper.prepareMainLooper();
        frame = Bitmap.createBitmapWithCanvasBacking(W, H);
        canvas = new Canvas(frame);

        root = group(null, 0xff202840, 0, 0, W, H);
        Box a = box(root, 0xffc03030, 20, 20, 120, 100);            // opaque
        Box glass = box(root, 0x8030c030, 80, 60, 200, 160);        // translucent, over a
        Group scroller = group(root, 0xff404040, 20, 200, 220, 400);
        Box row1 = box(scroller, 0xff3050c0, 10, 10, 190, 90);
        Box row2 = box(scroller, 0xffc0a030, 10, 110, 190, 190);
        Box row3 = box(scroller, 0xff30c0c0, 10, 210, 190, 290);    // below the fold
        Group turned = group(root, 0xff505020, 240, 220, 380, 360);
        turned.setRotation(30f);
        Box inTurned = box(turned, 0xffe0e0e0, 20, 20, 80, 80);
        Group card = group(root, 0xff606060, 240, 20, 380, 180);
        Box raised = box(card, 0xffd08020, 30, 30, 110, 110);
        raised.setElevation(12f);
        Box plain = box(card, 0xff8020d0, 10, 120, 60, 150);
        Group loose = group(root, 0, 20, 420, 380, 580);            // does not clip its children
        loose.setClipChildren(false);
        Box spiller = box(loose, 0xff20a060, 60, 60, 120, 100);
        spiller.spill = true;
        Box hidden = box(root, 0xffffffff, 300, 500, 380, 580);
        hidden.setVisibility(View.INVISIBLE);
        root.macapkDispatchAttachedToWindow();
        vri.setView(root);

        // The first frame is whole whatever was asked.
        check(frame() == ALL && !lastPartial, "the first frame's damage is everything");
        check(framePixelsAre(reference(), "first frame"), "the first frame equals the tree drawn whole");
        fullFrame();
        check(peek() == NONE, "after a frame, and no invalidation, the damage is none");

        // A view invalidates itself: its bounds in the window.
        a.invalidate();
        check(peek() == RECT && about(20, 20, 120, 100), "a child invalidating itself damages its bounds: "
                + Arrays.toString(dmg));
        check(peek() == NONE, "taking the damage resets it");
        fullFrame();

        // A sub-rectangle: the whole view, as on a device since API 21.
        a.invalidate(new Rect(10, 10, 20, 20));
        check(peek() == RECT && about(20, 20, 120, 100), "invalidate(Rect) damages the whole view: "
                + Arrays.toString(dmg));
        fullFrame();
        a.postInvalidate();
        check(peek() == RECT && about(20, 20, 120, 100), "postInvalidate damages the view");
        fullFrame();

        // Two in one frame: the union.
        a.invalidate();
        row2.invalidate();
        check(peek() == RECT && about(20, 20, 210, 390), "two invalidations in a frame damage their union: "
                + Arrays.toString(dmg));
        fullFrame();

        // Content changes under and over a translucent sibling.
        a.color = 0xff30c0ff;
        a.invalidate();
        frameMatches("an opaque view under a translucent one changes", true);
        glass.color = 0x80ff40ff;
        glass.invalidate();
        frameMatches("a translucent view over others changes", true);

        // requestLayout: everything.
        a.requestLayout();
        check(peek() == ALL, "requestLayout damages everything");
        fullFrame();

        // A hidden view asks for nothing.
        hidden.invalidate();
        check(!vri.isTraversalScheduled() && peek() == NONE, "a hidden view's invalidation asks for no frame");
        // Becoming visible is everything.
        hidden.setVisibility(View.VISIBLE);
        check(peek() == ALL, "a visibility change damages everything");
        hidden.setVisibility(View.INVISIBLE);
        fullFrame();

        // A scrolled parent: the row where the scroll put it, cut by the parent.
        scroller.scrollTo(0, 60);
        fullFrame();
        row2.invalidate();
        check(peek() == RECT && about(30, 250, 210, 330), "a child of a scrolled parent is damaged where it is drawn: "
                + Arrays.toString(dmg));
        fullFrame();
        row1.invalidate();
        check(peek() == RECT && about(30, 200, 210, 230), "... and only the part its parent shows: "
                + Arrays.toString(dmg));
        fullFrame();

        // Scrolling damages the scroller, and the frame after knows the new places.
        scroller.scrollTo(0, 100);
        check(vri.isTraversalScheduled(), "scrollTo asks for a frame");
        frameMatches("a scroll", true);
        check(about(20, 200, 220, 400), "a scroll damages the scrolling view's own clip: " + Arrays.toString(dmg));
        row2.color = 0xffffffff;
        row2.invalidate();
        frameMatches("a row invalidated after a partial scroll frame", true);
        check(about(30, 210, 210, 290), "... is damaged at its new place: " + Arrays.toString(dmg));
        row3.color = 0xff000000;   // came into view with that scroll
        row3.invalidate();
        frameMatches("a row the scroll brought into view", true);
        check(about(30, 310, 210, 390), "... is damaged where it now is: " + Arrays.toString(dmg));

        // A translated view: its parent's clip, which holds the old and the new place.
        fullFrame();
        a.setTranslationX(150f);
        check(peek() == RECT && covers(20, 20, 270, 100), "a translated view damages old and new bounds: "
                + Arrays.toString(dmg));
        fullFrame();
        a.setTranslationX(40f);
        frameMatches("a translation", true);
        a.setTranslationX(60f);
        frameMatches("a second translation, after a partial frame", true);
        a.color = 0xffff8000;
        a.invalidate();
        frameMatches("the translated view changes after partial frames", true);
        a.offsetTopAndBottom(15);
        a.invalidate();
        frameMatches("offsetTopAndBottom", true);
        a.setAlpha(0.5f);
        frameMatches("an alpha change", true);
        a.setScaleX(1.5f);
        frameMatches("a scale change", true);
        a.setScaleX(1f);
        a.setAlpha(1f);
        fullFrame();

        // Inside a rotated ancestor no rectangle of the view exists: the damage
        // is the clip the rotated view's parent draws under.
        inTurned.color = 0xff000000;
        inTurned.invalidate();
        frameMatches("a view inside a rotated ancestor", true);
        check(covers(0, 0, W, H), "... damages the rotated ancestor's parent: " + Arrays.toString(dmg));
        turned.setRotation(50f);
        frameMatches("the rotation changes", true);

        // A view that casts a shadow: its parent draws that, so the parent's clip.
        fullFrame();
        raised.color = 0xff20d080;
        raised.invalidate();
        frameMatches("a view with elevation", true);
        check(about(240, 20, 380, 180), "... damages its parent's clip: " + Arrays.toString(dmg));
        plain.color = 0xffffff00;
        plain.invalidate();
        frameMatches("a sibling of it", true);
        check(about(250, 140, 300, 170), "... damages only itself: " + Arrays.toString(dmg));

        // clipChildren=false: the child's clip is its parent's, and so is its damage.
        spiller.color = 0xffff2060;
        spiller.invalidate();
        frameMatches("a view that draws outside its bounds in a group that does not clip", true);
        check(covers(50, 450, 170, 550), "... damages what it can reach: " + Arrays.toString(dmg));

        // A view its parent draws by hand, somewhere else: no rectangle.
        fullFrame();
        Box byHand = box(root, 0xff80ff80, 10, 110, 40, 140);
        root.drawsItself = byHand;
        fullFrame();
        byHand.color = 0xffff80ff;
        byHand.invalidate();
        check(peek() == ALL, "a view drawn by child.draw(canvas) instead of drawChild damages everything");
        fullFrame();

        // From another thread: everything.
        Thread t = new Thread(a::invalidate);
        t.start();
        t.join();
        check(peek() == ALL, "an invalidation from another thread damages everything");
        fullFrame();

        // A view that invalidates while it is drawn asks for no frame of its own,
        // but the next frame draws it again, as every full frame did.
        plain.invalidateInDraw = true;
        fullFrame();
        check(!vri.isTraversalScheduled(), "an invalidation during the draw schedules no frame");
        row2.invalidate();
        check(peek() == RECT && covers(250, 140, 300, 170) && covers(30, 210, 210, 290),
                "... and is part of the next frame's damage: " + Arrays.toString(dmg));
        plain.invalidateInDraw = false;
        fullFrame();

        // A change made while a partial frame is drawn, outside what it draws
        // (an OnPreDrawListener, computeScroll): one more frame, which shows it.
        row2.inDraw = () -> { a.color = 0xff00ffff; a.invalidate(); };
        row2.invalidate();
        frame();
        row2.inDraw = null;
        check(lastPartial && lastFollowUp && vri.isTraversalScheduled(),
                "a change made during a partial frame, outside it, asks for one more frame");
        frameMatches("that frame", true);
        check(!lastFollowUp && !vri.isTraversalScheduled(), "... and no third");

        // A long run of mixed changes, each frame against the tree drawn whole.
        fullFrame();
        java.util.Random rnd = new java.util.Random(114);
        Box[] boxes = { a, glass, row1, row2, row3, inTurned, raised, plain, spiller };
        boolean runOk = true;
        int partials = 0;
        for (int i = 0; i < 400 && runOk; i++) {
            Box v = boxes[rnd.nextInt(boxes.length)];
            switch (rnd.nextInt(9)) {
                case 0: v.setTranslationX(rnd.nextInt(60) - 30); break;
                case 1: v.setTranslationY((rnd.nextInt(80) - 40) / 2f); break;   // half pixels too
                case 2: scroller.scrollTo(0, rnd.nextInt(120)); break;
                case 3: v.setAlpha(0.25f * (1 + rnd.nextInt(4))); break;
                case 4: turned.setRotation(rnd.nextInt(90)); break;
                case 5: v.offsetLeftAndRight(rnd.nextInt(7) - 3); v.invalidate(); break;
                case 6: v.setElevation(rnd.nextInt(3) * 6f); break;
                case 7: v.setScaleY(0.5f + rnd.nextInt(4) * 0.25f); break;
                default: break;
            }
            v.color = 0xff000000 | rnd.nextInt(0x1000000);
            v.invalidate();
            if (i % 37 == 36) vri.requestLayoutTraversal();   // the driver's heartbeat
            frame();
            if (lastPartial) partials++;
            runOk = framePixelsAre(reference(), "mixed change " + i);
            if (lastFollowUp) frame();
        }
        check(runOk && partials > 300, "400 mixed changes, " + partials + " of them partial frames, each equal to"
                + " the tree drawn whole");

        System.out.println(failures == 0 ? "DamageRectTest: all checks passed" : "DamageRectTest: " + failures
                + " check(s) FAILED");
        if (failures != 0) System.exit(1);
    }
}
