// IInputMethodManager for AOSP's InputMethodManager, over Win.apk's host input method
// (docs/research/aosp-views.md, step V2). On Android, InputMethodManagerService binds the focused
// editor's IRemoteInputConnection to the input method, which then calls commitText,
// setComposingText and the rest on it. Here the input method is the host's (the keyboard and
// Windows' IME, through AospInput): startInputOrWindowGainedFocus keeps the editor that has the
// focus and its connection, and the host's text and composition go to that connection's
// InputConnection on the main looper, as an input method's calls arrive there through
// RemoteInputConnectionImpl; Windows' text services work on that connection too (Win.apk's
// WindowsTextInput). There is no on-screen keyboard: the answer is "no input method"
// (InputBindResult.NO_IME, as on a device with none enabled), showSoftInput shows nothing in the
// app (Windows' touch keyboard on a PC with a touchscreen: WindowsTextInput.softInput), and
// the editor's own key handling takes the keyboard's keys, as on a phone with a hardware keyboard.
package com.winapk.aospviews;

import android.os.IBinder;
import android.os.ResultReceiver;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodSubtype;
import android.window.ImeOnBackInvokedDispatcher;

import com.android.internal.inputmethod.IInputMethodClient;
import com.android.internal.inputmethod.IRemoteAccessibilityInputConnection;
import com.android.internal.inputmethod.IRemoteInputConnection;
import com.android.internal.inputmethod.InputBindResult;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

public final class ImeService {
    private static volatile IRemoteInputConnection sConnection;
    private static volatile EditorInfo sEditor;

    public List<InputMethodInfo> getInputMethodList(int userId, int directBootAwareness) { return new ArrayList<>(); }
    public List<InputMethodInfo> getEnabledInputMethodList(int userId) { return new ArrayList<>(); }
    public List<InputMethodSubtype> getEnabledInputMethodSubtypeList(String imiId,
            boolean allowsImplicitlyEnabledSubtypes, int userId) {
        return new ArrayList<>();
    }

    public InputBindResult startInputOrWindowGainedFocus(int startInputReason, IInputMethodClient client,
            IBinder windowToken, int startInputFlags, int softInputMode, int windowFlags, EditorInfo editorInfo,
            IRemoteInputConnection inputConnection, IRemoteAccessibilityInputConnection remoteAccessibilityInputConnection,
            int unverifiedTargetSdkVersion, int userId, ImeOnBackInvokedDispatcher imeDispatcher) {
        // A window gaining focus with no editor in it unbinds (inputConnection null), as the
        // service unbinds the served view.
        sConnection = inputConnection;
        sEditor = inputConnection != null ? editorInfo : null;
        com.macapk.loader.WindowsTextInput.editorChanged();  // Windows' text services follow the editor
        return InputBindResult.NO_IME;
    }

    public boolean showSoftInput(IInputMethodClient client, IBinder windowToken,
            android.view.inputmethod.ImeTracker.Token statsToken, int flags, int lastClickToolType,
            ResultReceiver resultReceiver, int reason) {
        // Windows' touch keyboard stands for the soft keyboard (WindowsTextInput); there is still
        // no Android input method, so nothing is shown in the app's window.
        com.macapk.loader.WindowsTextInput.softInput(true);
        return false;
    }

    public boolean hideSoftInput(IInputMethodClient client, IBinder windowToken,
            android.view.inputmethod.ImeTracker.Token statsToken, int flags, ResultReceiver resultReceiver,
            int reason) {
        com.macapk.loader.WindowsTextInput.softInput(false);
        return false;
    }

    /** The bound editor's EditorInfo, or null. */
    static EditorInfo editor() { return sEditor; }

    /** The bound editor's InputConnection, or null: RemoteInputConnectionImpl.getInputConnection
     *  (null once the connection is finished). */
    static InputConnection connection() {
        IRemoteInputConnection c = sConnection;
        if (c == null) return null;
        try {
            Method m = c.getClass().getMethod("getInputConnection");
            m.setAccessible(true);
            return (InputConnection) m.invoke(c);
        } catch (ReflectiveOperationException | RuntimeException e) {
            return null;
        }
    }

    /** Whether an editor that takes text is bound (the host then shows its composition inline). */
    static boolean bound() {
        EditorInfo e = sEditor;
        return e != null && e.inputType != android.text.InputType.TYPE_NULL && connection() != null;
    }

    /** An input method's commitText. Main thread. False with no editor bound. */
    static boolean commitText(String text) {
        InputConnection ic = bound() ? connection() : null;
        if (ic == null) return false;
        ic.commitText(text, 1);
        return true;
    }

    /** An input method's setComposingText, or finishComposingText for null. Main thread. */
    static void composingText(String text) {
        InputConnection ic = bound() ? connection() : null;
        if (ic == null) return;
        if (text == null) ic.finishComposingText();
        else ic.setComposingText(text, 1);
    }

    /** The caret of the focused editor in display pixels, x in the high half; -1 with none. */
    static long anchor() {
        try {
            android.view.View v = InputRouter.focusedView();
            if (!(v instanceof android.widget.TextView t) || !t.isAttachedToWindow()) return -1L;
            int[] loc = new int[2];
            t.getLocationOnScreen(loc);
            float x = loc[0], y = loc[1] + t.getHeight();
            android.text.Layout l = t.getLayout();
            int sel = t.getSelectionEnd();
            if (l != null && sel >= 0) {
                x = loc[0] + t.getTotalPaddingLeft() - t.getScrollX() + l.getPrimaryHorizontal(sel);
                y = loc[1] + t.getTotalPaddingTop() - t.getScrollY() + l.getLineBottom(l.getLineForOffset(sel));
            }
            return ((long) Math.round(Math.max(0f, x)) << 32) | (Math.round(Math.max(0f, y)) & 0xffffffffL);
        } catch (RuntimeException e) {
            return -1L;
        }
    }
}
