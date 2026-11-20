// The in-process window manager's input dispatcher (docs/research/aosp-views.md, step V2): what
// InputDispatcher does with an event read from the device, for the host's events (AospInput) and
// the gamepad's (GamepadBridge), into the windows' own input path: each window's
// WindowInputEventReceiver (ViewRootImpl's), through NativeInputEventReceiver, on the main looper.
//
// As InputDispatcher does, simplified to one display and one task:
//   - a touch gesture goes to the window it went down in, top first in the compositor's order:
//     a window that is touchable (no FLAG_NOT_TOUCHABLE) and either contains the point or is
//     touch-modal (neither FLAG_NOT_FOCUSABLE nor FLAG_NOT_TOUCH_MODAL: a dialog takes every touch,
//     and its own Dialog.onTouchEvent cancels it on a touch outside, as on a phone); a window above
//     it that asked for FLAG_WATCH_OUTSIDE_TOUCH gets ACTION_OUTSIDE (a PopupWindow dismisses on
//     it). The gesture stays with that window until UP or CANCEL;
//   - a hover and a scroll go to the window under the pointer by the same test, and a window the
//     pointer leaves gets HOVER_EXIT;
//   - a key and a joystick go to the focused window: the topmost visible, focusable one (no
//     FLAG_NOT_FOCUSABLE). When that window changes, the old one hears onFocusEvent(false) and the
//     new one onFocusEvent(true) (ViewRootImpl.windowFocusChanged: what starts the IME's input);
//   - an event reaches a window in its own coordinates: the display's, less its frame's origin.
// Each window gets its own copy, which its receiver recycles when it finishes it.
package com.winapk.aospviews;

import android.view.InputEvent;
import android.view.InputEventReceiver;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.WindowManager;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

final class InputRouter {
    private InputRouter() {}

    private static AospWindows.Win sTouchTarget, sHoverTarget, sFocused;

    /** The visible windows with a receiver, top first (the compositor's order, reversed). */
    static List<AospWindows.Win> topFirst() {
        List<AospWindows.Win> order = new ArrayList<>();
        for (AospWindows.Win w : AospWindows.sWindows.values()) {
            if (w.visible && w.attrs != null && NativeInputEventReceiver.receiverFor(w.inputChannel) != null) {
                order.add(w);
            }
        }
        order.sort(Comparator.comparingInt((AospWindows.Win w) -> Compositor.band(w.attrs.type))
                .thenComparingLong(w -> w.added).reversed());
        return order;
    }

    private static boolean touchable(AospWindows.Win w) {
        return (w.attrs.flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) == 0;
    }

    private static boolean touchModal(AospWindows.Win w) {
        return (w.attrs.flags & (WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL)) == 0;
    }

    /** The window a pointer at (x, y) in display pixels lands in; with `outside`, the windows above
     *  it that watch outside touches. */
    private static AospWindows.Win windowAt(float x, float y, List<AospWindows.Win> outside) {
        for (AospWindows.Win w : topFirst()) {
            if (!touchable(w)) continue;
            boolean inside = w.frame.contains((int) x, (int) y);
            if (inside || touchModal(w)) return w;
            if (outside != null && (w.attrs.flags & WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH) != 0) {
                outside.add(w);
            }
        }
        return null;
    }

    /** The window keys go to. */
    static AospWindows.Win focused() {
        for (AospWindows.Win w : topFirst()) {
            if ((w.attrs.flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) == 0) return w;
        }
        return null;
    }

    /** The windows changed (one came, went, showed or hid): moves the focus if it must. Main
     *  thread. */
    static void windowsChanged() {
        AospWindows.Win now = focused();
        if (now == sFocused) return;
        AospWindows.Win was = sFocused;
        sFocused = now;
        if (was != null) focus(was, false);
        if (now != null) focus(now, true);
        if (sTouchTarget != null && !AospWindows.sWindows.containsValue(sTouchTarget)) sTouchTarget = null;
        if (sHoverTarget != null && !AospWindows.sWindows.containsValue(sHoverTarget)) sHoverTarget = null;
    }

    private static void focus(AospWindows.Win w, boolean hasFocus) {
        InputEventReceiver r = NativeInputEventReceiver.receiverFor(w.inputChannel);
        if (r == null) return;
        try {
            r.onFocusEvent(hasFocus);
        } catch (RuntimeException e) {
            System.err.println(AospWindows.TAG + "focus to " + w.attrs.getTitle() + ": " + e);
        }
    }

    private static void deliver(AospWindows.Win w, InputEvent e) {
        InputEventReceiver r = NativeInputEventReceiver.receiverFor(w.inputChannel);
        if (r == null) return;
        if (Compositor.TRACE && e instanceof KeyEvent) System.err.println(AospWindows.TAG + "key to " + w.attrs.getTitle() + ": " + e);
        try {
            NativeInputEventReceiver.dispatch(r, e);
        } catch (RuntimeException ex) {
            System.err.println(AospWindows.TAG + "input to " + w.attrs.getTitle() + " threw:");
            ex.printStackTrace();
        }
    }

    private static MotionEvent inWindow(MotionEvent display, AospWindows.Win w) {
        MotionEvent e = MotionEvent.obtain(display);
        e.offsetLocation(-w.frame.left, -w.frame.top);
        return e;
    }

    /** A pointer event in display pixels. Main thread. */
    static void motion(MotionEvent ev) {
        windowsChanged();
        final int action = ev.getActionMasked();
        if (ev.isFromSource(android.view.InputDevice.SOURCE_CLASS_POINTER) && !ev.isHoverEvent()
                && action != MotionEvent.ACTION_SCROLL) {
            if (action == MotionEvent.ACTION_DOWN) {
                List<AospWindows.Win> outside = new ArrayList<>();
                sTouchTarget = windowAt(ev.getX(), ev.getY(), outside);
                for (AospWindows.Win w : outside) {
                    MotionEvent o = inWindow(ev, w);
                    o.setAction(MotionEvent.ACTION_OUTSIDE);
                    deliver(w, o);
                }
            }
            AospWindows.Win t = sTouchTarget;
            if (t != null) deliver(t, inWindow(ev, t));
            if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) sTouchTarget = null;
            return;
        }
        if (ev.isFromSource(android.view.InputDevice.SOURCE_CLASS_POINTER)) {
            AospWindows.Win t = windowAt(ev.getX(), ev.getY(), null);
            if (ev.isHoverEvent()) {
                if (sHoverTarget != null && sHoverTarget != t) {
                    MotionEvent exit = inWindow(ev, sHoverTarget);
                    exit.setAction(MotionEvent.ACTION_HOVER_EXIT);
                    deliver(sHoverTarget, exit);
                }
                sHoverTarget = action == MotionEvent.ACTION_HOVER_EXIT ? null : t;
            }
            if (t != null) deliver(t, inWindow(ev, t));
            return;
        }
        // A joystick, a trackball: the focused window.
        AospWindows.Win f = focused();
        if (f != null) deliver(f, MotionEvent.obtain(ev));
    }

    /** A key, to the focused window. Main thread. */
    static boolean key(KeyEvent ev) {
        windowsChanged();
        AospWindows.Win f = focused();
        if (f == null) return false;
        deliver(f, ev);
        return true;
    }

    /** The focused window's focused view (the IME's anchor). Main thread only. */
    static android.view.View focusedView() {
        AospWindows.Win f = focused();
        InputEventReceiver r = f != null ? NativeInputEventReceiver.receiverFor(f.inputChannel) : null;
        if (r == null) return null;
        for (android.view.View root : rootsOf()) {
            android.view.ViewRootImpl vri = root.getViewRootImpl();
            if (vri != null && root.hasWindowFocus()) {
                android.view.View v = root.findFocus();
                if (v != null) return v;
            }
        }
        return null;
    }

    private static List<android.view.View> rootsOf() {
        try {
            return new ArrayList<>(android.view.WindowManagerGlobal.getInstance().getWindowViews());
        } catch (RuntimeException e) {
            return new ArrayList<>();
        }
    }
}
