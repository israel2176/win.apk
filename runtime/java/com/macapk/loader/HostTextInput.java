// HostTextInput -- the focused editor for Windows' text services, and the touch keyboard
// (Win.apk; windows/host/win32_window.h, "text input"; the editor side of
// windows/host/tsf_text_store.h).
//
// Windows' input methods are TSF text services that read and edit the focused window's
// document. That document is a copy of the focused editor's InputConnection, which this class
// makes on the editor's thread (TouchDispatcher.hostImePost) and sends to the window:
//   - which editor has the focus (a new one, a new epoch), and whether it takes compositions
//     (a text field that is not a password: the other runtime's rule, after Android's IMEs);
//   - its text and selection (getExtractedText), and, for a TextView, its caret and the boxes
//     of the characters around it from its Layout, in surface pixels (right-to-left text is
//     laid out by the TextView itself);
// whenever it may have changed: the binding changed (InputMethodManager's startInput, AOSP's
// startInputOrWindowGainedFocus), a key, a character or a touch went to the app, a text
// service's edit was applied, and twice a second while an editor has the focus (an app that
// changes its text itself). The window answers with the text services' edits, which are
// applied here as an Android input method makes them through the InputConnection
// (setComposingRegion + setComposingText, setSelection, finishComposingText, and a
// replacement as InputConnection.replaceText's default does it).
//
// Showing the soft input (InputMethodManager.showSoftInput, a tap on an editable TextView, as
// Android's Editor does) shows Windows' touch keyboard; hiding it, or the editor going, hides
// it. The window decides whether this PC has a touch keyboard to show.
//
// The state is the editor thread's; nothing here blocks. WINAPK_TSF=0 turns it all off.
//
// Adapted in part from the other runtime (https://example.invalid/other-runtime),
// app/java/com/other/app/input/WindowsInputMethod.java (allowsCompositions, the order of the
// edits), Copyright (C) 2026 its authors, SPDX-License-Identifier: Apache-2.0.
// Modified for Win.apk.
package com.macapk.loader;

import android.os.Handler;
import android.os.Looper;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ExtractedText;
import android.view.inputmethod.ExtractedTextRequest;
import android.view.inputmethod.InputConnection;

import java.util.Arrays;
import java.util.concurrent.atomic.AtomicBoolean;

public final class HostTextInput {
    private static final String TAG = "Win.apk.TextInput";
    static final boolean ON = !"0".equals(System.getenv("WINAPK_TSF"));

    /** How long an editor's text may be for its character boxes to go with it. */
    private static final int GEOMETRY_MAX = 512;
    private static final long POLL_MS = 500;

    // tsf_text_store.h DocumentEdit::Kind.
    static final int EDIT_REPLACE = 0, EDIT_SELECT = 1, EDIT_COMPOSITION_RANGE = 2, EDIT_COMPOSITION_END = 3,
            EDIT_COMPOSITION_TEXT = 4;

    /** False when there is no window to type in (headless): nothing more is sent then. */
    private static native boolean nativeFocus(boolean active, boolean ime, int epoch);
    private static native void nativeDocument(String text, int start, int end, int offset, int epoch, int applied,
            boolean available, boolean reversed, float[] caret, int geometryStart, float[] geometry);
    private static native void nativeSoftInput(boolean show);

    private static volatile boolean sNoHost;  // no natives (a plain JVM, the Mac host)
    private static final AtomicBoolean sSyncQueued = new AtomicBoolean();

    // The editor thread's.
    private static InputConnection sConnection;
    private static boolean sActive, sIme, sPolling, sWindow;
    private static int sEpoch, sApplied;
    private static Object[] sLast;  // the last copy sent, to send only a changed one
    private static Handler sPoll;

    private HostTextInput() {}

    // ---- from the framework ----------------------------------------------------------------

    /** The binding may have changed (an editor got or lost the focus). Any thread. */
    public static void editorChanged() {
        TouchDispatcher d = TouchDispatcher.macapkInstance();
        if (d != null) requestSync(d);
    }

    /** InputMethodManager.showSoftInput (true) and hideSoftInput (false). Any thread. */
    public static void softInput(boolean show) {
        if (!ON || sNoHost) return;
        try {
            nativeSoftInput(show);
        } catch (UnsatisfiedLinkError e) {
            sNoHost = true;
        }
        if (show) editorChanged();
    }

    // ---- from the window (TouchDispatcher.hostImeSync, hostImeEdit) -------------------------

    static void requestSync(final TouchDispatcher d) {
        if (!ON || sNoHost) return;
        if (!sSyncQueued.compareAndSet(false, true)) return;
        d.hostImePost(() -> {
            sSyncQueued.set(false);
            sync(d);
        });
    }

    static void edit(final TouchDispatcher d, final int kind, final String text, final int start, final int end,
            final int epoch, final int revision) {
        if (!ON) return;
        d.hostImePost(() -> {
            if (epoch != sEpoch || revision <= sApplied) return;
            InputConnection ic = d.hostImeConnection();
            if (ic == null || ic != sConnection) return;
            try {
                apply(ic, kind, text != null ? text : "", start, end);
            } catch (RuntimeException e) {
                NativeLog.w(TAG, "edit " + kind + ": " + e);
            }
            sApplied = revision;
            sync(d);
        });
    }

    /** One text service edit, as an Android input method makes it. */
    static void apply(InputConnection ic, int kind, String text, int start, int end) {
        switch (kind) {
            case EDIT_REPLACE:
                // InputConnection.replaceText's default (API 34), which older editors lack.
                ic.beginBatchEdit();
                ic.finishComposingText();
                ic.setSelection(start, end);
                ic.commitText(text, 1);
                ic.endBatchEdit();
                break;
            case EDIT_SELECT:
                ic.setSelection(start, end);  // end before start: the caret at the start
                break;
            case EDIT_COMPOSITION_RANGE:
                if (end > start) ic.setComposingRegion(start, end);
                else ic.finishComposingText();
                break;
            case EDIT_COMPOSITION_END:
                ic.finishComposingText();
                break;
            case EDIT_COMPOSITION_TEXT:
                // A replacement would commit the composing text; inside a composition the text
                // is the composition's.
                ic.beginBatchEdit();
                ic.setComposingRegion(start, end);
                ic.setComposingText(text, 1);
                ic.endBatchEdit();
                break;
            default:
                break;
        }
    }

    /** the other runtime's rule (WindowsInputMethod.allowsCompositions): a text field, not a password. */
    static boolean allowsCompositions(EditorInfo e) {
        if (e == null) return false;
        if ((e.imeOptions & EditorInfo.IME_FLAG_FORCE_ASCII) != 0) return false;
        int cls = e.inputType & EditorInfo.TYPE_MASK_CLASS;
        if (cls != EditorInfo.TYPE_CLASS_TEXT) return false;
        int variation = e.inputType & EditorInfo.TYPE_MASK_VARIATION;
        return variation != EditorInfo.TYPE_TEXT_VARIATION_PASSWORD
                && variation != EditorInfo.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                && variation != EditorInfo.TYPE_TEXT_VARIATION_WEB_PASSWORD;
    }

    // ---- the copy ---------------------------------------------------------------------------

    /** On the editor's thread: tells the window of a new binding, and sends a changed copy. */
    static void sync(TouchDispatcher d) {
        if (sNoHost) return;
        InputConnection ic = null;
        EditorInfo ei = null;
        try {
            ic = d.hostImeConnection();
            if (ic != null) ei = d.hostImeEditorInfo();
        } catch (RuntimeException e) {
            ic = null;
        }
        boolean active = ic != null, ime = active && allowsCompositions(ei);
        try {
            if (ic != sConnection || active != sActive || ime != sIme) {
                sConnection = ic;
                sActive = active;
                sIme = ime;
                sEpoch++;
                sApplied = 0;
                sLast = null;
                NativeLog.i(TAG, active ? "editor " + sEpoch + ": " + ic.getClass().getName()
                        + (ime ? ", takes compositions" : ", no compositions (inputType 0x"
                        + Integer.toHexString(ei != null ? ei.inputType : 0) + ")") : "no editor");
                sWindow = nativeFocus(active, ime, sEpoch);
            } else if (!sWindow && active) {
                sWindow = nativeFocus(active, ime, sEpoch);  // the window came after the editor
            }
            poll(d, active && ime && sWindow);
            if (!ime || !sWindow) return;
            sendDocument(d, ic);
        } catch (UnsatisfiedLinkError e) {
            sNoHost = true;
        }
    }

    private static void poll(final TouchDispatcher d, boolean on) {
        if (on == sPolling) return;
        sPolling = on;
        if (!on) return;
        if (sPoll == null) sPoll = new Handler(Looper.getMainLooper());
        sPoll.postDelayed(new Runnable() {
            @Override public void run() {
                if (!sPolling) return;
                requestSync(d);
                sPoll.postDelayed(this, POLL_MS);
            }
        }, POLL_MS);
    }

    private static void sendDocument(TouchDispatcher d, InputConnection ic) {
        String text = "";
        int start = 0, end = 0, offset = 0;
        boolean available = false;
        try {
            ExtractedText et = ic.getExtractedText(new ExtractedTextRequest(), 0);
            if (et != null && et.text != null && et.startOffset >= 0) {
                text = et.text.toString();
                int a = et.selectionStart, b = et.selectionEnd;
                if (a >= 0 && b >= 0 && a <= text.length() && b <= text.length()) {
                    start = a;
                    end = b;
                    offset = et.startOffset;
                    available = true;
                }
            }
        } catch (RuntimeException e) {
            available = false;
        }
        if (!available) text = "";
        final boolean reversed = start > end;
        final int lo = Math.min(start, end), hi = Math.max(start, end);
        float[] caret = null, geometry = null;
        int geometryStart = 0;
        if (available) {
            android.widget.TextView t = null;
            try { t = d.hostImeTextView(); } catch (RuntimeException ignored) { }
            if (t != null && t.isAttachedToWindow()) {
                try {
                    float top = d.hostImeSurfaceTop();
                    caret = caret(t, offset + (reversed ? lo : hi), top);
                    if (text.length() <= GEOMETRY_MAX) {
                        geometry = characters(t, offset, text.length(), top);
                        geometryStart = offset;
                    }
                } catch (RuntimeException e) {
                    caret = null;
                    geometry = null;
                }
            }
        }
        Object[] copy = {text, lo, hi, offset, sApplied, available, reversed,
                caret != null ? Arrays.toString(caret) : null, geometryStart,
                geometry != null ? Arrays.hashCode(geometry) : 0};
        if (Arrays.equals(copy, sLast)) return;
        sLast = copy;
        nativeDocument(text, lo, hi, offset, sEpoch, sApplied, available, reversed, caret, geometryStart, geometry);
    }

    /** The TextView's origin on the surface, in pixels. */
    private static float[] origin(android.widget.TextView t, float top) {
        int[] loc = new int[2];
        t.getLocationOnScreen(loc);
        return new float[] {loc[0] + t.getTotalPaddingLeft() - t.getScrollX(),
                loc[1] + t.getTotalPaddingTop() - t.getScrollY() + top};
    }

    /** The caret at `offset`, a line high and a pixel wide. */
    private static float[] caret(android.widget.TextView t, int offset, float top) {
        android.text.Layout l = t.getLayout();
        if (l == null) return null;
        offset = Math.max(0, Math.min(offset, l.getText().length()));
        float[] o = origin(t, top);
        int line = l.getLineForOffset(offset);
        float x = o[0] + l.getPrimaryHorizontal(offset);
        return new float[] {x, o[1] + l.getLineTop(line), x + 1, o[1] + l.getLineBottom(line)};
    }

    /** The box of each of `count` characters from `offset`, four floats each; null when the
     *  layout does not hold that text. A character's box spans its two edges on its line,
     *  whichever way the line runs (Hebrew and Arabic right to left). */
    private static float[] characters(android.widget.TextView t, int offset, int count, float top) {
        android.text.Layout l = t.getLayout();
        if (l == null || count == 0 || offset + count > l.getText().length()) return null;
        float[] o = origin(t, top);
        float[] out = new float[count * 4];
        for (int i = 0; i < count; i++) {
            int at = offset + i;
            int line = l.getLineForOffset(at);
            float a = l.getPrimaryHorizontal(at);
            float b = at + 1 <= l.getLineEnd(line) && l.getLineForOffset(at + 1) == line
                    ? l.getPrimaryHorizontal(at + 1)
                    : (l.getParagraphDirection(line) < 0 ? l.getLineLeft(line) : l.getLineRight(line));
            float left = Math.min(a, b), right = Math.max(a, b);
            if (right - left < 1f) right = left + 1f;  // a line break, a zero-width mark
            out[i * 4] = o[0] + left;
            out[i * 4 + 1] = o[1] + l.getLineTop(line);
            out[i * 4 + 2] = o[0] + right;
            out[i * 4 + 3] = o[1] + l.getLineBottom(line);
        }
        return out;
    }
}
