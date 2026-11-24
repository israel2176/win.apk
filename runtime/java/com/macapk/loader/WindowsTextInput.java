// WindowsTextInput -- the focused editor for Windows' text services and touch keyboard
// (Win.apk; windows/host/win32_window.h "text input", native_text_input.cpp).
//
// On Android an input method works on the focused editor through its InputConnection. On
// Windows the input methods are TSF text services working on a document the window keeps
// (windows/host/tsf_store.h). This class is the editor's side of that document, on the editor's
// thread (TouchDispatcher.hostImePost):
//
//   - when the input method manager binds an editor (ImeService.startInputOrWindowGainedFocus,
//     editorChanged), the window is told its kind (EditorInfo.inputType and imeOptions: the
//     document's input scopes, a password's keyboard disabled) and is sent a copy of it;
//   - every change to the editor sends a fresh copy, with no polling: a TextWatcher and a
//     SpanWatcher on its Editable (the selection's spans) say when; for an editor with no
//     Editable to watch (a Compose field) the window asks after each key and touch (hostSync);
//   - where the editor drew its characters (Layout.fillCharacterBounds, right to left too),
//     the caret and the editor's bounds, measured after each change and whenever the view tree
//     lays out or scrolls, so that a candidate window sits under the composition;
//   - the text services' edits (hostEdit: one per TSF lock, numbered) are applied as an Android
//     input method makes them, in one batch edit: setComposingRegion / setComposingText for a
//     composition (with the text service's display attributes as spans), commitText for what
//     it commits, setSelection; the copy sent after names the newest edit applied, so the
//     window never takes an older copy over its own edit.
//
// A password's text is never sent. Positions are the editor's UTF-16 indices.
package com.macapk.loader;

import android.text.Editable;
import android.text.Layout;
import android.text.NoCopySpan;
import android.text.Selection;
import android.text.SpanWatcher;
import android.text.Spannable;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.TextWatcher;
import android.text.style.BackgroundColorSpan;
import android.text.style.ForegroundColorSpan;
import android.text.style.UnderlineSpan;
import android.view.View;
import android.view.ViewTreeObserver;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ExtractedText;
import android.view.inputmethod.ExtractedTextRequest;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.SurroundingText;
import android.widget.TextView;

public final class WindowsTextInput {
    private static final String TAG = "Win.apk.TextInput";

    // text_input_model.h's Run flags.
    static final int RUN_UNDERLINE = 1, RUN_THICK = 2, RUN_FOREGROUND = 4, RUN_BACKGROUND = 8, RUN_TARGET = 16;
    /** How far each side of the selection the document reaches. */
    static final int REACH = 4096;
    /** How many characters each side of the selection and composition are measured. */
    static final int MEASURED = 64;

    private static native boolean nativeFocus(int epoch, int inputType, int imeOptions, boolean watched, String text,
            int offset, int anchor, int caret);
    private static native void nativeBlur(int epoch);
    private static native void nativeDocument(int epoch, int acked, String text, int offset, int anchor, int caret);
    private static native void nativeLayout(int epoch, int acked, float[] editor, float[] caret, int first,
            float[] boxes, boolean[] rtl);
    private static native void nativeSoftInput(boolean show);

    private static volatile boolean sNoHost;  // no natives here (a plain JVM, a test)

    // The editor's thread only.
    private static int sEpoch;
    private static boolean sBound;
    private static InputConnection sConnection;
    private static EditorInfo sInfo;
    private static View sView;
    private static Editable sEditable;
    private static int sApplied;
    private static boolean sApplying, sSnapshotQueued, sLayoutQueued;
    private static final Watcher sWatcher = new Watcher();

    private WindowsTextInput() {}

    // ---- the app's side --------------------------------------------------------------------

    /** The input method manager bound another editor, or none. Any thread. */
    public static void editorChanged() {
        if (sNoHost) return;
        final TouchDispatcher d = TouchDispatcher.macapkInstance();
        if (d != null) d.hostImePost(() -> bind(d));
    }

    /** InputMethodManager.showSoftInput / hideSoftInput: Windows' touch keyboard. Any thread. */
    public static void softInput(boolean show) {
        if (sNoHost) return;
        try {
            nativeSoftInput(show);
        } catch (UnsatisfiedLinkError e) {
            sNoHost = true;
        }
    }

    // ---- the window's side (its thread) -----------------------------------------------------

    /** What a text service did in one TSF lock (text_input_model.h Edit). runs: five ints each. */
    static void hostEdit(final int epoch, final int seq, final int start, final int end, final String text,
            final int compStart, final int compEnd, final int anchor, final int caret, final int[] runs) {
        final TouchDispatcher d = TouchDispatcher.macapkInstance();
        if (d == null) return;
        d.hostImePost(() -> {
            if (!sBound || epoch != sEpoch || seq <= sApplied) return;
            sApplying = true;
            try {
                apply(sConnection, start, end, text, compStart, compEnd, anchor, caret, runs);
            } catch (RuntimeException e) {
                NativeLog.w(TAG, "a text service's edit: " + e);
            } finally {
                sApplying = false;
            }
            sApplied = seq;
            sendDocument();
            sendLayout(d);
        });
    }

    /** A key or a touch went to an editor this class cannot watch: it may have changed. */
    static void hostSync(final int epoch) {
        final TouchDispatcher d = TouchDispatcher.macapkInstance();
        if (d == null) return;
        d.hostImePost(() -> {
            if (!sBound || epoch != sEpoch) return;
            sendDocument();
            sendLayout(d);
        });
    }

    // ---- binding ----------------------------------------------------------------------------

    private static void bind(TouchDispatcher d) {
        InputConnection ic = null;
        EditorInfo ei = null;
        View view = null;
        try {
            ic = d.hostImeConnection();
            ei = ic != null ? d.hostImeEditorInfo() : null;
            view = d.hostImeView();
        } catch (RuntimeException e) {
            ic = null;
        }
        if (sBound && ic == sConnection && ei == sInfo) {
            // The same editor (its window took the focus again): only where it is may differ.
            watchView(view);
            sendDocument();
            sendLayout(d);
            return;
        }
        unbind();
        if (ic == null || ei == null) return;
        sEpoch++;
        sApplied = 0;
        sConnection = ic;
        sInfo = ei;
        watchEditable();
        watchView(view);
        final Copy c = read(ic, sEditable, ei.inputType);
        boolean window;
        try {
            window = nativeFocus(sEpoch, ei.inputType, ei.imeOptions, sEditable != null, c.text, c.offset, c.anchor,
                    c.caret);
        } catch (UnsatisfiedLinkError e) {
            sNoHost = true;
            window = false;
        }
        if (!window) {  // no window to type in (headless)
            unwatch();
            sConnection = null;
            sInfo = null;
            return;
        }
        sBound = true;
        NativeLog.i(TAG, "editor " + sEpoch + ": " + ic.getClass().getName() + ", inputType 0x"
                + Integer.toHexString(ei.inputType) + (sEditable != null ? "" : ", not watched"));
        sendLayout(d);
    }

    private static void unbind() {
        if (sBound) {
            try {
                nativeBlur(sEpoch);
            } catch (UnsatisfiedLinkError e) {
                sNoHost = true;
            }
        }
        sBound = false;
        unwatch();
        sConnection = null;
        sInfo = null;
    }

    private static void watchEditable() {
        Editable e = sConnection instanceof BaseInputConnection b ? b.getEditable() : null;
        if (e == sEditable) return;
        if (sEditable != null) sEditable.removeSpan(sWatcher);
        sEditable = e;
        if (e != null) e.setSpan(sWatcher, 0, e.length(), Spanned.SPAN_INCLUSIVE_INCLUSIVE);
    }

    private static void watchView(View v) {
        if (v == sView) return;
        if (sView != null) {
            ViewTreeObserver o = sView.getViewTreeObserver();
            o.removeOnGlobalLayoutListener(sWatcher);
            o.removeOnScrollChangedListener(sWatcher);
        }
        sView = v;
        if (v != null) {
            ViewTreeObserver o = v.getViewTreeObserver();
            o.addOnGlobalLayoutListener(sWatcher);
            o.addOnScrollChangedListener(sWatcher);
        }
    }

    private static void unwatch() {
        if (sEditable != null) sEditable.removeSpan(sWatcher);
        sEditable = null;
        watchView(null);
    }

    /** The editor changed (its text or selection; with layout false) or moved (true). */
    private static void changed(boolean layoutOnly) {
        if (!sBound || sApplying) return;  // an edit being applied sends its copy itself
        if (layoutOnly ? (sLayoutQueued || sSnapshotQueued) : sSnapshotQueued) return;
        final TouchDispatcher d = TouchDispatcher.macapkInstance();
        if (d == null) return;
        if (layoutOnly) sLayoutQueued = true; else sSnapshotQueued = true;
        // After the change is done (a batch edit, the rest of a layout pass): one copy for all of it.
        d.hostImePost(() -> {
            final boolean text = sSnapshotQueued;
            sSnapshotQueued = sLayoutQueued = false;
            if (!sBound) return;
            if (text) sendDocument();
            sendLayout(d);
        });
    }

    private static final class Watcher implements TextWatcher, SpanWatcher, NoCopySpan,
            ViewTreeObserver.OnGlobalLayoutListener, ViewTreeObserver.OnScrollChangedListener {
        @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
        @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
        @Override public void afterTextChanged(Editable s) { changed(false); }
        @Override public void onSpanAdded(Spannable text, Object what, int start, int end) { span(what); }
        @Override public void onSpanRemoved(Spannable text, Object what, int start, int end) { span(what); }
        @Override public void onSpanChanged(Spannable text, Object what, int ostart, int oend, int nstart, int nend) {
            span(what);
        }
        private void span(Object what) {
            if (what == Selection.SELECTION_START || what == Selection.SELECTION_END) changed(false);
        }
        @Override public void onGlobalLayout() {
            // TextView.setText puts a new Editable in: watch that one.
            if (sBound && sConnection instanceof BaseInputConnection b && b.getEditable() != sEditable) {
                watchEditable();
                changed(false);
            } else {
                changed(true);
            }
        }
        @Override public void onScrollChanged() { changed(true); }
    }

    // ---- the editor, copied and measured ----------------------------------------------------

    static final class Copy {
        String text = "";
        int offset, anchor, caret;
    }

    /** A password's text stays in the app (text_input_model.cpp, field_policy). */
    static boolean secret(int inputType) {
        final int cls = inputType & EditorInfo.TYPE_MASK_CLASS, variation = inputType & EditorInfo.TYPE_MASK_VARIATION;
        if (cls == EditorInfo.TYPE_CLASS_TEXT) {
            return variation == EditorInfo.TYPE_TEXT_VARIATION_PASSWORD
                    || variation == EditorInfo.TYPE_TEXT_VARIATION_WEB_PASSWORD
                    || variation == EditorInfo.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD;
        }
        return cls == EditorInfo.TYPE_CLASS_NUMBER && variation == EditorInfo.TYPE_NUMBER_VARIATION_PASSWORD;
    }

    /** The editor's text around its selection: from its Editable (the selection's direction
     *  kept), else getSurroundingText, else getExtractedText. */
    static Copy read(InputConnection ic, Editable editable, int inputType) {
        final Copy c = new Copy();
        if (secret(inputType)) return c;
        if (editable != null) {
            final int n = editable.length();
            int a = Selection.getSelectionStart(editable), b = Selection.getSelectionEnd(editable);
            if (a < 0 || b < 0) a = b = n;
            int lo = Math.max(0, Math.min(a, b) - REACH), hi = Math.min(n, Math.max(a, b) + REACH);
            if (lo > 0 && Character.isLowSurrogate(editable.charAt(lo))) lo--;  // not half an emoji
            if (hi < n && Character.isLowSurrogate(editable.charAt(hi))) hi++;
            c.text = editable.subSequence(lo, hi).toString();
            c.offset = lo;
            c.anchor = a;
            c.caret = b;
            return c;
        }
        try {
            final SurroundingText s = ic.getSurroundingText(REACH, REACH, 0);
            if (s != null && s.getText() != null && s.getOffset() >= 0) {
                c.text = s.getText().toString();
                c.offset = s.getOffset();
                c.anchor = c.offset + s.getSelectionStart();
                c.caret = c.offset + s.getSelectionEnd();
                return c;
            }
        } catch (RuntimeException | AbstractMethodError e) {
            // an InputConnection from before API 31 that throws: the extracted text then
        }
        final ExtractedText e = ic.getExtractedText(new ExtractedTextRequest(), 0);
        if (e != null && e.text != null) {
            c.text = e.text.toString();
            c.offset = e.startOffset;
            c.anchor = e.startOffset + e.selectionStart;
            c.caret = e.startOffset + e.selectionEnd;
        }
        return c;
    }

    private static void sendDocument() {
        if (!sBound) return;
        final Copy c = read(sConnection, sEditable, sInfo.inputType);
        nativeDocument(sEpoch, sApplied, c.text, c.offset, c.anchor, c.caret);
    }

    private static void sendLayout(TouchDispatcher d) {
        if (!sBound) return;
        final View v = sView;
        float[] editor = null, caret = null, boxes = null;
        boolean[] rtl = null;
        int first = 0;
        if (v != null && v.isAttachedToWindow()) {
            final int[] at = new int[2];
            v.getLocationOnScreen(at);
            final float x0 = at[0], y0 = at[1] + d.hostImeSurfaceTop();
            editor = new float[] {x0, y0, x0 + v.getWidth(), y0 + v.getHeight()};
            if (v instanceof TextView t && !secret(sInfo.inputType)) {
                final Layout l = t.getLayout();
                if (l != null) {
                    final float dx = x0 + t.getTotalPaddingLeft() - t.getScrollX();
                    final float dy = y0 + t.getTotalPaddingTop() - t.getScrollY();
                    final Measured m = measure(l, t.getSelectionStart(), t.getSelectionEnd(), dx, dy);
                    if (m != null) {
                        first = m.first;
                        boxes = m.boxes;
                        rtl = m.rtl;
                        caret = m.caret;
                    }
                }
            }
        }
        nativeLayout(sEpoch, sApplied, editor, caret, first, boxes, rtl);
    }

    static final class Measured {
        int first;
        float[] boxes;  // left, top, right, bottom per character
        boolean[] rtl;
        float[] caret;
    }

    /** The characters around the selection and the composition, and the caret, as `l` drew
     *  them, moved by (dx, dy). */
    static Measured measure(Layout l, int anchor, int caret, float dx, float dy) {
        final CharSequence text = l.getText();
        final int n = text.length();
        if (anchor < 0 || caret < 0) anchor = caret = 0;
        anchor = Math.min(anchor, n);
        caret = Math.min(caret, n);
        int lo = Math.min(anchor, caret), hi = Math.max(anchor, caret);
        if (text instanceof Spannable s) {
            final int cs = BaseInputConnection.getComposingSpanStart(s), ce = BaseInputConnection.getComposingSpanEnd(s);
            if (cs >= 0 && ce >= cs) {
                lo = Math.min(lo, cs);
                hi = Math.max(hi, ce);
            }
        }
        final Measured m = new Measured();
        m.first = Math.max(0, lo - MEASURED);
        final int last = Math.min(n, hi + MEASURED);
        final int count = last - m.first;
        m.boxes = new float[count * 4];
        m.rtl = new boolean[count];
        try {
            if (count > 0) l.fillCharacterBounds(m.first, last, m.boxes, 0);
            for (int i = 0; i < count; i++) {
                m.boxes[i * 4] += dx;
                m.boxes[i * 4 + 1] += dy;
                m.boxes[i * 4 + 2] += dx;
                m.boxes[i * 4 + 3] += dy;
                m.rtl[i] = l.isRtlCharAt(m.first + i);
            }
            final int line = l.getLineForOffset(caret);
            final float x = dx + l.getPrimaryHorizontal(caret);
            m.caret = new float[] {x, dy + l.getLineTop(line), x + 1, dy + l.getLineBottom(line)};
        } catch (RuntimeException | LinkageError e) {
            return null;  // a Layout that cannot say: the editor's bounds only
        }
        return m;
    }

    // ---- the text services' edits ----------------------------------------------------------

    /** One edit (text_input_model.h Edit) as an input method makes it: [start, end) of the
     *  editor's text before becomes `text`, of which [compStart, compEnd) stays composing (-1:
     *  none, the composition is finished); then the selection. Package-private for the test. */
    static void apply(InputConnection ic, int start, int end, String text, int compStart, int compEnd, int anchor,
            int caret, int[] runs) {
        ic.beginBatchEdit();
        try {
            final boolean replaces = start != end || !text.isEmpty();
            if (compStart >= 0) {
                if (replaces) {
                    final int head = compStart - start, tail = start + text.length() - compEnd;
                    final CharSequence composing = styled(text.substring(head, text.length() - tail), runs, compStart);
                    if (head > 0 || tail > 0) {
                        // Committed text beside the composition (a clause committed, the rest
                        // still composing): committed first, then composed between.
                        ic.finishComposingText();
                        ic.setSelection(start, end);
                        ic.commitText(text.substring(0, head) + text.substring(text.length() - tail), 1);
                        ic.setSelection(compStart, compStart);
                    } else if (start < end) {
                        ic.setComposingRegion(start, end);
                    } else {
                        ic.finishComposingText();
                        ic.setSelection(start, start);
                    }
                    ic.setComposingText(composing, 1);
                }
                // else the composition stays as the editor has it
            } else if (replaces) {
                if (start < end) {
                    ic.setComposingRegion(start, end);  // commitText replaces it, as a commit does
                } else {
                    ic.finishComposingText();
                    ic.setSelection(start, start);
                }
                ic.commitText(text, 1);
            } else {
                ic.finishComposingText();
            }
            ic.setSelection(anchor, caret);
        } finally {
            ic.endBatchEdit();
        }
    }

    /** The composition's text with the text service's look as spans (base: the editor position
     *  of its first character); no runs: plain, and the editor underlines it as it does any
     *  composition. */
    static CharSequence styled(String text, int[] runs, int base) {
        if (runs == null || runs.length < 5) return text;
        final SpannableString s = new SpannableString(text);
        for (int i = 0; i + 4 < runs.length; i += 5) {
            final int a = Math.max(0, Math.min(text.length(), runs[i] - base));
            final int b = Math.max(a, Math.min(text.length(), runs[i + 1] - base));
            final int flags = runs[i + 2];
            if (a == b) continue;
            if ((flags & RUN_UNDERLINE) != 0) s.setSpan(new UnderlineSpan(), a, b, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            if ((flags & RUN_FOREGROUND) != 0) {
                s.setSpan(new ForegroundColorSpan(runs[i + 3]), a, b, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            }
            if ((flags & RUN_BACKGROUND) != 0) {
                s.setSpan(new BackgroundColorSpan(runs[i + 4]), a, b, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            }
        }
        return s;
    }
}
