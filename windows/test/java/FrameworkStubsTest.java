// FrameworkStubsTest -- framework methods that used to be stubs (Completeness, under
// windows/test/apicheck, lists the ones that answer a constant or do nothing where Android's
// do something), checked against what Android's do, so that none quietly turns back into one.
// One check per method fixed; add the next one here when another stub gets its body.
//
//   javac -cp "<runtime>/lib/android-framework.jar;<runtime>/lib/macapk-loader.jar" -d out \
//         windows/test/java/FrameworkStubsTest.java
//   MACAPK_LOG_LEVEL=7 java -Djava.awt.headless=true \
//         -cp "<runtime>/lib/android-framework.jar;<runtime>/lib/macapk-loader.jar;out" FrameworkStubsTest
//
// Exits 0 when every check passes. No window, no native runtime: MACAPK_LOG_LEVEL=7 keeps the
// framework from logging, which needs the runtime's NativeLog. (TextView's baseline setters
// need a themed Context and are checked by the UI probe's text_baselines case instead.)

import android.animation.AnimatorSet;
import android.animation.TimeInterpolator;
import android.animation.ValueAnimator;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.view.View;
import android.view.ViewGroup;

public final class FrameworkStubsTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "ok   " : "FAIL ") + what);
        if (!ok) failures++;
    }

    static class Box extends View {
        final Paint paint = new Paint();
        Box() { super(null); }
        @Override protected void onDraw(Canvas canvas) {
            paint.setColor(0xff000000);
            canvas.drawRect(0, 0, getWidth(), getHeight(), paint);
        }
    }

    static class Group extends ViewGroup {
        final Paint paint = new Paint();
        Group() { super(null); setWillNotDraw(false); }
        @Override protected void onLayout(boolean changed, int l, int t, int r, int b) { }
        @Override protected void onDraw(Canvas canvas) {
            paint.setColor(0xffffffff);
            canvas.drawRect(0, 0, getWidth(), getHeight(), paint);
        }
    }

    /** AOSP AnimatorSet.setInterpolator keeps the curve and gives it to every child at start. */
    static void animatorSetInterpolator() {
        TimeInterpolator curve = new android.view.animation.LinearInterpolator();
        ValueAnimator a = ValueAnimator.ofFloat(0f, 1f), b = ValueAnimator.ofFloat(0f, 1f);
        AnimatorSet set = new AnimatorSet();
        set.playTogether(a, b);
        check(set.getInterpolator() == null, "AnimatorSet: no interpolator until one is set");
        set.setInterpolator(curve);
        check(set.getInterpolator() == curve, "AnimatorSet.getInterpolator answers the one set");
        try { set.start(); } catch (Throwable ignored) { /* no Looper here: the children's own start */ }
        check(a.getInterpolator() == curve && b.getInterpolator() == curve,
              "AnimatorSet.start gives its interpolator to every child");
    }

    /** AOSP View.setClipBounds / getClipBounds, and the parent cutting the child's drawing to it. */
    static void viewClipBounds() {
        Box box = new Box();
        check(box.getClipBounds() == null, "View.getClipBounds() is null until one is set");
        check(!box.getClipBounds(new Rect()), "View.getClipBounds(Rect) is false until one is set");
        Rect r = new Rect(0, 0, 50, 50);
        box.setClipBounds(r);
        Rect got = box.getClipBounds();
        check(r.equals(got) && got != r, "View.getClipBounds() answers a copy of the rect set");
        Rect out = new Rect();
        check(box.getClipBounds(out) && r.equals(out), "View.getClipBounds(Rect) fills the rect set");
        box.setClipBounds(null);
        check(box.getClipBounds() == null, "View.setClipBounds(null) clears it");

        // A black 100x100 child cut to its top-left 50x50, on a white parent.
        Group root = new Group();
        Box child = new Box();
        root.addView(child);
        root.measure(View.MeasureSpec.makeMeasureSpec(200, View.MeasureSpec.EXACTLY),
                     View.MeasureSpec.makeMeasureSpec(200, View.MeasureSpec.EXACTLY));
        root.layout(0, 0, 200, 200);
        child.measure(View.MeasureSpec.makeMeasureSpec(100, View.MeasureSpec.EXACTLY),
                      View.MeasureSpec.makeMeasureSpec(100, View.MeasureSpec.EXACTLY));
        child.layout(20, 20, 120, 120);
        child.setClipBounds(new Rect(0, 0, 50, 50));
        Bitmap bmp = Bitmap.createBitmapWithCanvasBacking(200, 200);
        root.draw(new Canvas(bmp));
        check((bmp.getPixel(30, 30) & 0xffffff) == 0x000000, "inside the clip bounds the child draws");
        check((bmp.getPixel(100, 100) & 0xffffff) == 0xffffff, "outside the clip bounds the child does not draw");
        check((bmp.getPixel(100, 30) & 0xffffff) == 0xffffff, "right of the clip bounds the child does not draw");
    }

    public static void main(String[] args) {
        animatorSetInterpolator();
        viewClipBounds();
        System.out.println(failures == 0 ? "all passed" : failures + " failed");
        System.exit(failures == 0 ? 0 : 1);
    }
}
