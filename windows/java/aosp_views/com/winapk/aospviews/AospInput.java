// The host's input entries with AOSP's views (docs/research/aosp-views.md, step V2): the window
// (win32_window.cpp, pointer_input.h) calls these methods of the registered dispatcher
// (NativeCocoa.registerTouchDispatcher; ViewHost.newTouchDispatcher gives this subclass of
// Win.apk's TouchDispatcher), on its UI thread. Each event is made here as Android's
// InputReader makes it (TouchInputMapper: SOURCE_TOUCHSCREEN, the contacts as pointers;
// CursorInputMapper: SOURCE_MOUSE, DOWN then BUTTON_PRESS, BUTTON_RELEASE then UP, hover between;
// a wheel step ACTION_SCROLL with AXIS_VSCROLL / AXIS_HSCROLL; KeyboardInputMapper:
// SOURCE_KEYBOARD, the repeat count), in display pixels (the host's points: origin at the
// bottom left, times the backing scale), then handed to InputRouter on the main looper, which
// delivers it to a window's own receiver. Nothing of TouchDispatcher's own delivery runs.
//
// Text an input method made, and its composition, go to the focused editor's InputConnection
// (ImeService) as an input method's commitText / setComposingText would; with no editor bound,
// each character is a key, as the host's keyboard sends it.
package com.winapk.aospviews;

import android.os.Handler;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;

import com.macapk.loader.NativeCocoa;
import com.macapk.loader.TouchDispatcher;
import com.macapk.loader.view.ViewHost;

public final class AospInput extends TouchDispatcher {

    private static final int POINTER_DOWN_BUTTONS = MotionEvent.BUTTON_PRIMARY | MotionEvent.BUTTON_SECONDARY
            | MotionEvent.BUTTON_TERTIARY;

    private long mTouchDownTime, mMouseDownTime;
    private int mMouseButtons;
    private final long[] mKeyDownTimes = new long[512];
    private final int[] mKeyRepeats = new int[512];

    static volatile AospInput sInstance;

    public AospInput() { sInstance = this; }

    private static Handler main() { return AospWindows.main(); }

    static float scale() {
        try {
            float s = NativeCocoa.backingScale();
            if (s > 0f) return s;
        } catch (Throwable ignored) {
            // A plain JVM.
        }
        return AospWindows.metrics().density;
    }

    private static float px(float x) { return x * scale(); }
    private static float py(float y, float viewH) { return (viewH - y) * scale(); }

    static void post(MotionEvent ev) { main().post(() -> InputRouter.motion(ev)); }

    // ---- touch ------------------------------------------------------------------------------

    /** One finger (the host's single-pointer entry). */
    @Override public void dispatch(int action, float x, float y, float viewW, float viewH) {
        touchAt(action, px(x), py(y, viewH));
    }

    void touchAt(int action, float sx, float sy) {
        long now = SystemClock.uptimeMillis();
        if (action == MotionEvent.ACTION_DOWN) mTouchDownTime = now;
        MotionEvent ev = MotionEvent.obtain(mTouchDownTime, now, action, sx, sy, 0);
        ev.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        post(ev);
    }

    @Override public void dispatchHostTouch(int action, int index, int[] ids, float[] xs, float[] ys,
            float[] pressures, int[] tools, int buttonState, float viewW, float viewH) {
        if (ids == null || xs == null || ys == null || pressures == null || tools == null) return;
        final int n = ids.length;
        if (n == 0 || xs.length < n || ys.length < n || pressures.length < n || tools.length < n) return;
        long now = SystemClock.uptimeMillis();
        if (action == MotionEvent.ACTION_DOWN) mTouchDownTime = now;
        MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[n];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[n];
        boolean stylus = false;
        for (int i = 0; i < n; i++) {
            props[i] = new MotionEvent.PointerProperties();
            props[i].id = ids[i];
            props[i].toolType = tools[i];
            stylus |= tools[i] == MotionEvent.TOOL_TYPE_STYLUS || tools[i] == MotionEvent.TOOL_TYPE_ERASER;
            coords[i] = new MotionEvent.PointerCoords();
            coords[i].x = px(xs[i]);
            coords[i].y = py(ys[i], viewH);
            coords[i].pressure = pressures[i];
            coords[i].size = 0f;
        }
        int act = action;
        if (action == MotionEvent.ACTION_POINTER_DOWN || action == MotionEvent.ACTION_POINTER_UP) {
            act = action | (index << MotionEvent.ACTION_POINTER_INDEX_SHIFT);
        }
        MotionEvent ev = MotionEvent.obtain(mTouchDownTime, now, act, n, props, coords, 0, buttonState, 1f, 1f,
                0, 0, stylus ? InputDevice.SOURCE_STYLUS | InputDevice.SOURCE_TOUCHSCREEN : InputDevice.SOURCE_TOUCHSCREEN,
                0);
        post(ev);
    }

    /** Ctrl+wheel, a precision touchpad's pinch: two fingers either side of the center. */
    @Override public void dispatchHostPinch(int phase, float cx, float cy, float span, float viewW, float viewH) {
        float x = px(cx), y = py(cy, viewH), half = Math.max(1f, px(span) / 2f);
        long now = SystemClock.uptimeMillis();
        MotionEvent.PointerProperties[] props = {new MotionEvent.PointerProperties(), new MotionEvent.PointerProperties()};
        props[0].id = 0;
        props[1].id = 1;
        props[0].toolType = props[1].toolType = MotionEvent.TOOL_TYPE_FINGER;
        MotionEvent.PointerCoords[] two = {new MotionEvent.PointerCoords(), new MotionEvent.PointerCoords()};
        two[0].x = x - half;
        two[1].x = x + half;
        two[0].y = two[1].y = y;
        two[0].pressure = two[1].pressure = 1f;
        if (phase == 0) {
            mTouchDownTime = now;
            post(MotionEvent.obtain(now, now, MotionEvent.ACTION_DOWN, 1, new MotionEvent.PointerProperties[] {props[0]},
                    new MotionEvent.PointerCoords[] {two[0]}, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0));
            post(MotionEvent.obtain(now, now, MotionEvent.ACTION_POINTER_DOWN | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT),
                    2, props, two, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0));
        } else if (phase == 1) {
            post(MotionEvent.obtain(mTouchDownTime, now, MotionEvent.ACTION_MOVE, 2, props, two, 0, 0, 1f, 1f, 0, 0,
                    InputDevice.SOURCE_TOUCHSCREEN, 0));
        } else {
            post(MotionEvent.obtain(mTouchDownTime, now, MotionEvent.ACTION_POINTER_UP | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT),
                    2, props, two, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0));
            post(MotionEvent.obtain(mTouchDownTime, now, MotionEvent.ACTION_UP, 1, new MotionEvent.PointerProperties[] {props[0]},
                    new MotionEvent.PointerCoords[] {two[0]}, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0));
        }
    }

    // ---- mouse ------------------------------------------------------------------------------

    private static MotionEvent mouseEvent(long downTime, long now, int action, float x, float y, int buttons,
            int meta, int actionButton) {
        MotionEvent.PointerProperties p = new MotionEvent.PointerProperties();
        p.id = 0;
        p.toolType = MotionEvent.TOOL_TYPE_MOUSE;
        MotionEvent.PointerCoords c = new MotionEvent.PointerCoords();
        c.x = x;
        c.y = y;
        c.pressure = (buttons & POINTER_DOWN_BUTTONS) != 0 ? 1f : 0f;
        MotionEvent e = MotionEvent.obtain(downTime, now, action, 1, new MotionEvent.PointerProperties[] {p},
                new MotionEvent.PointerCoords[] {c}, meta, buttons, 1f, 1f, 0, 0, InputDevice.SOURCE_MOUSE, 0);
        if (actionButton != 0) e.setActionButton(actionButton);
        return e;
    }

    /** CursorInputMapper's stream: the buttons pressed after this event, the pointer in points. */
    @Override public void dispatchHostMouse(int buttonState, int metaState, float x, float y, float viewW,
            float viewH) {
        final float sx = px(x), sy = py(y, viewH);
        final long now = SystemClock.uptimeMillis();
        final int last = mMouseButtons;
        mMouseButtons = buttonState;
        final boolean wasDown = (last & POINTER_DOWN_BUTTONS) != 0, down = (buttonState & POINTER_DOWN_BUTTONS) != 0;
        final int pressed = buttonState & ~last, released = last & ~buttonState;
        if (!wasDown && down) mMouseDownTime = now;
        int step = last;
        for (int b = 1; b != 0 && b <= released; b <<= 1) {
            if ((released & b) == 0) continue;
            step &= ~b;
            post(mouseEvent(mMouseDownTime, now, MotionEvent.ACTION_BUTTON_RELEASE, sx, sy, step, metaState, b));
        }
        if (!wasDown && down) {
            post(mouseEvent(now, now, MotionEvent.ACTION_HOVER_EXIT, sx, sy, last, metaState, 0));
            post(mouseEvent(mMouseDownTime, now, MotionEvent.ACTION_DOWN, sx, sy, buttonState, metaState, 0));
        } else if (wasDown && !down) {
            post(mouseEvent(mMouseDownTime, now, MotionEvent.ACTION_UP, sx, sy, buttonState, metaState, 0));
            post(mouseEvent(now, now, MotionEvent.ACTION_HOVER_ENTER, sx, sy, buttonState, metaState, 0));
        } else if (down) {
            post(mouseEvent(mMouseDownTime, now, MotionEvent.ACTION_MOVE, sx, sy, buttonState, metaState, 0));
        } else {
            post(mouseEvent(now, now, MotionEvent.ACTION_HOVER_MOVE, sx, sy, buttonState, metaState, 0));
        }
        for (int b = 1; b != 0 && b <= pressed; b <<= 1) {
            if ((pressed & b) == 0) continue;
            step |= b;
            post(mouseEvent(mMouseDownTime, now, MotionEvent.ACTION_BUTTON_PRESS, sx, sy, step, metaState, b));
        }
    }

    @Override public void dispatchHostMouseExit(float x, float y, float viewW, float viewH) {
        long now = SystemClock.uptimeMillis();
        post(mouseEvent(now, now, MotionEvent.ACTION_HOVER_EXIT, -1f, -1f, mMouseButtons, 0, 0));
    }

    /** A wheel step: ACTION_SCROLL at the pointer, as CursorInputMapper's scroll does. */
    @Override public void dispatchHostWheel(float x, float y, float viewW, float viewH, float vscroll, float hscroll,
            float slopX, float slopY, float dragX, float dragY) {
        long now = SystemClock.uptimeMillis();
        MotionEvent.PointerProperties p = new MotionEvent.PointerProperties();
        p.id = 0;
        p.toolType = MotionEvent.TOOL_TYPE_MOUSE;
        MotionEvent.PointerCoords c = new MotionEvent.PointerCoords();
        c.x = px(x);
        c.y = py(y, viewH);
        c.setAxisValue(MotionEvent.AXIS_VSCROLL, vscroll);
        c.setAxisValue(MotionEvent.AXIS_HSCROLL, hscroll);
        post(MotionEvent.obtain(now, now, MotionEvent.ACTION_SCROLL, 1, new MotionEvent.PointerProperties[] {p},
                new MotionEvent.PointerCoords[] {c}, 0, mMouseButtons, 1f, 1f, 0, 0, InputDevice.SOURCE_MOUSE, 0));
    }

    // ---- keys -------------------------------------------------------------------------------

    @Override public void dispatchKey(int action, int keyCode, int unicodeChar, int metaState) {
        dispatchHostKey(action, keyCode, unicodeChar, metaState, false);
    }

    @Override public void dispatchHostKey(int action, int keyCode, int unicodeChar, int metaState, boolean autoRepeat) {
        KeyEvent ev = keyEvent(action, keyCode, unicodeChar, metaState, autoRepeat);
        main().post(() -> InputRouter.key(ev));
    }

    /** KeyboardInputMapper's event: a down's time held by its up, the repeat count of a held key. */
    KeyEvent keyEvent(int action, int keyCode, int unicodeChar, int metaState, boolean autoRepeat) {
        long now = SystemClock.uptimeMillis();
        int k = keyCode >= 0 && keyCode < mKeyDownTimes.length ? keyCode : 0;
        int repeat = 0;
        if (action == KeyEvent.ACTION_DOWN) {
            if (autoRepeat) {
                repeat = ++mKeyRepeats[k];
            } else {
                mKeyDownTimes[k] = now;
                mKeyRepeats[k] = 0;
            }
        }
        long downTime = mKeyDownTimes[k] != 0 ? mKeyDownTimes[k] : now;
        KeyEvent ev = new KeyEvent(downTime, now, action, keyCode, repeat, metaState, KeyCharacterMap.VIRTUAL_KEYBOARD,
                0, KeyEvent.FLAG_FROM_SYSTEM, InputDevice.SOURCE_KEYBOARD);
        if (unicodeChar > 0) ViewHost.winApk().setKeyUnicodeChar(ev, unicodeChar);
        return ev;
    }

    // ---- the host's input method ------------------------------------------------------------

    @Override public void dispatchHostText(final String text) {
        if (text == null || text.isEmpty()) return;
        main().post(() -> {
            if (ImeService.commitText(text)) return;
            for (int i = 0; i < text.length(); i++) {
                char ch = text.charAt(i);
                InputRouter.key(keyEvent(KeyEvent.ACTION_DOWN, 0, ch, 0, false));
                InputRouter.key(keyEvent(KeyEvent.ACTION_UP, 0, ch, 0, false));
            }
        });
    }

    @Override public boolean hostImeInline() { return ImeService.bound(); }

    @Override public void dispatchHostComposingText(final String text) {
        main().post(() -> ImeService.composingText(text));
    }

    /** Where the host's candidate window goes: under the caret of the focused editor, in display
     *  pixels, x in the high half; -1 with none. */
    @Override public long hostImeAnchor() { return ImeService.anchor(); }
}
