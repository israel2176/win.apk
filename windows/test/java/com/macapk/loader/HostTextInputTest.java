// HostTextInputTest -- a TSF text service's edits (windows/host/tsf_text_store.h DocumentEdit),
// as HostTextInput applies them to an editor's InputConnection, on a plain JVM: the editor is a
// BaseInputConnection over an Editable, as Win.apk's MacApkEditorInputConnection is.
//
//   javac -cp "<runtime>/lib/android-framework.jar;<runtime>/lib/macapk-loader.jar" -d out \
//         windows/test/java/com/macapk/loader/HostTextInputTest.java
//   java  -cp "<runtime>/lib/android-framework.jar;<runtime>/lib/macapk-loader.jar;out" \
//         com.macapk.loader.HostTextInputTest
//
// Checks the sequences the text store makes (windows/test/tsf_test.cpp checks the store's
// side): a Japanese composition growing and committed, a Hebrew one after Hebrew text, a
// replacement outside the composition, a reversed selection, a composition range moved and
// dropped; and which editors take compositions (the other runtime's rule: a text field, not a password).
// Exits 0 when every check passes.
package com.macapk.loader;

import android.text.Editable;
import android.text.InputType;
import android.text.Selection;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;

public final class HostTextInputTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "PASS  " : "FAIL  ") + what);
        if (!ok) failures++;
    }

    private static BaseInputConnection editor(String text, int caret) {
        BaseInputConnection ic = new BaseInputConnection(null, false);
        Editable e = ic.getEditable();
        e.append(text);
        Selection.setSelection(e, caret);
        return ic;
    }

    private static String state(BaseInputConnection ic) {
        Editable e = ic.getEditable();
        return e.toString() + "|" + Selection.getSelectionStart(e) + "-" + Selection.getSelectionEnd(e) + "|"
                + BaseInputConnection.getComposingSpanStart(e) + "-" + BaseInputConnection.getComposingSpanEnd(e);
    }

    public static void main(String[] args) {
        // A composition grows at the caret (OnStartComposition, SetText in it), then ends.
        BaseInputConnection ic = editor("ab", 2);
        HostTextInput.apply(ic, HostTextInput.EDIT_COMPOSITION_TEXT, "に", 2, 2);
        check(state(ic).equals("abに|3-3|2-3"), "a composition starts at the caret: " + state(ic));
        HostTextInput.apply(ic, HostTextInput.EDIT_COMPOSITION_TEXT, "にほん", 2, 3);
        check(state(ic).equals("abにほん|5-5|2-5"), "it grows in place: " + state(ic));
        HostTextInput.apply(ic, HostTextInput.EDIT_COMPOSITION_TEXT, "日本", 2, 5);
        check(state(ic).equals("ab日本|4-4|2-4"), "a conversion replaces it: " + state(ic));
        HostTextInput.apply(ic, HostTextInput.EDIT_COMPOSITION_END, "日本", 2, 4);
        check(state(ic).equals("ab日本|4-4|-1--1"), "its end commits it: " + state(ic));

        // Hebrew: a composition after Hebrew text, UTF-16 positions as the store gives them.
        ic = editor("שלום ", 5);
        HostTextInput.apply(ic, HostTextInput.EDIT_COMPOSITION_TEXT, "ע", 5, 5);
        HostTextInput.apply(ic, HostTextInput.EDIT_COMPOSITION_TEXT, "עולם", 5, 6);
        HostTextInput.apply(ic, HostTextInput.EDIT_COMPOSITION_END, "עולם", 5, 9);
        check(state(ic).equals("שלום עולם|9-9|-1--1"),
                "a Hebrew composition after Hebrew text: " + state(ic));

        // A replacement (reconversion, a correction): the range takes the text, the caret after it.
        ic = editor("hello wrld", 10);
        HostTextInput.apply(ic, HostTextInput.EDIT_REPLACE, "world", 6, 10);
        check(state(ic).equals("hello world|11-11|-1--1"), "a replacement: " + state(ic));

        // A replacement outside a composition, then its range again (the store sends both).
        ic = editor("xy", 2);
        HostTextInput.apply(ic, HostTextInput.EDIT_COMPOSITION_TEXT, "ab", 2, 2);
        HostTextInput.apply(ic, HostTextInput.EDIT_REPLACE, "Z", 0, 1);
        HostTextInput.apply(ic, HostTextInput.EDIT_COMPOSITION_RANGE, "", 2, 4);
        check(state(ic).equals("Zyab|1-1|2-4"), "a replacement beside a composition keeps it: " + state(ic));
        HostTextInput.apply(ic, HostTextInput.EDIT_COMPOSITION_RANGE, "", 3, 3);
        check(state(ic).equals("Zyab|1-1|-1--1"), "an empty composition range drops it: " + state(ic));

        // A reversed selection: the caret at its start.
        ic = editor("abcdef", 0);
        HostTextInput.apply(ic, HostTextInput.EDIT_SELECT, "", 4, 1);
        Editable e = ic.getEditable();
        check(Selection.getSelectionStart(e) == 4 && Selection.getSelectionEnd(e) == 1,
                "a reversed selection keeps its anchor and caret: " + state(ic));

        // Which editors take compositions.
        EditorInfo text = new EditorInfo();
        text.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE;
        EditorInfo password = new EditorInfo();
        password.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD;
        EditorInfo number = new EditorInfo();
        number.inputType = InputType.TYPE_CLASS_NUMBER;
        EditorInfo ascii = new EditorInfo();
        ascii.inputType = InputType.TYPE_CLASS_TEXT;
        ascii.imeOptions = EditorInfo.IME_FLAG_FORCE_ASCII;
        check(HostTextInput.allowsCompositions(text) && !HostTextInput.allowsCompositions(password)
                        && !HostTextInput.allowsCompositions(number) && !HostTextInput.allowsCompositions(ascii)
                        && !HostTextInput.allowsCompositions(null),
                "a text field takes compositions; a password, a number, a forced-ASCII field do not");

        System.out.println(failures + " check(s) failed");
        System.exit(failures == 0 ? 0 : 1);
    }
}
