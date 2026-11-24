// WindowsTextInputTest -- the editor's side of Windows' text services
// (runtime/java/com/macapk/loader/WindowsTextInput.java) on a plain JVM: the text services'
// edits (windows/host/text_input_model.h Edit, as windows/test/text_input_model_test.cpp makes
// them) applied to an editor's InputConnection as an Android input method makes them, and the
// copy of the editor that goes back. The editor is a BaseInputConnection over an Editable.
//
//   javac -cp "<build>/java/android-framework.jar;<stage>/lib/macapk-loader.jar" -d out \
//         windows/test/java/com/macapk/loader/WindowsTextInputTest.java
//   java  -cp "<build>/java/android-framework.jar;<stage>/lib/macapk-loader.jar;out" \
//         com.macapk.loader.WindowsTextInputTest
//
// Exits 0 when every check passes.
package com.macapk.loader;

import android.text.Editable;
import android.text.InputType;
import android.text.Selection;
import android.text.SpannableStringBuilder;
import android.text.style.BackgroundColorSpan;
import android.text.style.UnderlineSpan;
import android.view.inputmethod.BaseInputConnection;

public final class WindowsTextInputTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "PASS  " : "FAIL  ") + what);
        if (!ok) failures++;
    }

    /** An editor: an InputConnection over an Editable, as an EditText's is. */
    private static final class Editor extends BaseInputConnection {
        final Editable text = new SpannableStringBuilder();

        Editor(String initial, int anchor, int caret) {
            super(null, true);
            text.append(initial);
            Selection.setSelection(text, anchor, caret);
        }

        @Override public Editable getEditable() { return text; }

        String state() {
            return text + "|" + Selection.getSelectionStart(text) + "-" + Selection.getSelectionEnd(text) + "|"
                    + getComposingSpanStart(text) + "-" + getComposingSpanEnd(text);
        }
    }

    private static Editor editor(String text, int caret) { return new Editor(text, caret, caret); }

    private static void apply(Editor e, int start, int end, String text, int cs, int ce, int anchor, int caret) {
        WindowsTextInput.apply(e, start, end, text, cs, ce, anchor, caret, null);
    }

    public static void main(String[] args) {
        compositions();
        commits();
        selections();
        looks();
        copies();
        System.out.println(failures + " text input check(s) failed");
        System.exit(failures == 0 ? 0 : 1);
    }

    private static void compositions() {
        // A Japanese composition at the caret: typed, grown, converted, finished.
        Editor e = editor("ab", 2);
        apply(e, 2, 2, "に", 2, 3, 3, 3);
        check(e.state().equals("abに|3-3|2-3"), "a composition starts at the caret: " + e.state());
        apply(e, 2, 3, "にほん", 2, 5, 5, 5);
        check(e.state().equals("abにほん|5-5|2-5"), "it grows in place: " + e.state());
        apply(e, 2, 5, "日本", 2, 4, 4, 4);
        check(e.state().equals("ab日本|4-4|2-4"), "a conversion replaces it: " + e.state());
        apply(e, 4, 4, "", -1, -1, 4, 4);
        check(e.state().equals("ab日本|4-4|-1--1"), "its end finishes it, the text kept: " + e.state());

        // A partial commit: the first clause committed, the second still composing.
        e = editor("x", 1);
        apply(e, 1, 1, "にほんご", 1, 5, 5, 5);
        apply(e, 1, 5, "日本ご", 3, 4, 4, 4);
        check(e.state().equals("x日本ご|4-4|3-4"), "a clause committed, the rest composing: " + e.state());

        // Reconversion: committed text becomes the composition again, unchanged.
        e = editor("かんじ漢字", 5);
        apply(e, 3, 5, "漢字", 3, 5, 5, 5);
        check(e.state().equals("かんじ漢字|5-5|3-5"), "reconversion composes committed text in place: " + e.state());
        // The composition as it was, only the caret moved inside it.
        apply(e, 4, 4, "", 3, 5, 4, 4);
        check(e.state().equals("かんじ漢字|4-4|3-5"), "a composition that stays is left alone: " + e.state());

        // Hebrew after Hebrew (dictation, handwriting).
        e = editor("שלום ", 5);
        apply(e, 5, 5, "עולם", 5, 9, 9, 9);
        apply(e, 9, 9, "", -1, -1, 9, 9);
        check(e.state().equals("שלום עולם|9-9|-1--1"), "Hebrew composed and finished: " + e.state());

        // Everything composed deleted (Backspace through the composition).
        e = editor("ab", 2);
        apply(e, 2, 2, "か", 2, 3, 3, 3);
        apply(e, 2, 3, "", 2, 2, 2, 2);
        check(e.state().equals("ab|2-2|2-2"), "a composition deleted to nothing: the text gone, still composing: "
                + e.state());
    }

    private static void commits() {
        // A text service's insertion with no composition (the emoji panel, the touch keyboard).
        Editor e = editor("hi ", 3);
        apply(e, 3, 3, "😀", -1, -1, 5, 5);
        check(e.state().equals("hi 😀|5-5|-1--1"), "an emoji: two units, the caret after them: " + e.state());
        // A replacement of committed text (a correction).
        e = editor("teh cat", 7);
        apply(e, 0, 3, "the", -1, -1, 7, 7);
        check(e.state().equals("the cat|7-7|-1--1"), "a correction replaces the word: " + e.state());
        // A deletion.
        apply(e, 3, 7, "", -1, -1, 3, 3);
        check(e.state().equals("the|3-3|-1--1"), "a deletion: " + e.state());
        // A commit while the editor composes (from before): the composing region is replaced.
        e = editor("ab", 2);
        apply(e, 2, 2, "nihon", 2, 7, 7, 7);
        apply(e, 2, 7, "日本", -1, -1, 4, 4);
        check(e.state().equals("ab日本|4-4|-1--1"), "a composition committed as its conversion: " + e.state());
    }

    private static void selections() {
        Editor e = editor("hello", 5);
        apply(e, 5, 5, "", -1, -1, 4, 1);
        check(Selection.getSelectionStart(e.text) == 4 && Selection.getSelectionEnd(e.text) == 1,
                "a selection made backwards: the anchor at 4, the caret at 1");
        apply(e, 0, 0, "", -1, -1, 1, 4);
        check(e.state().equals("hello|1-4|-1--1"), "and forwards: " + e.state());
    }

    private static void looks() {
        // Two clauses: the first being converted (highlighted), the second typed (underlined).
        int[] runs = {
            2, 4, WindowsTextInput.RUN_UNDERLINE | WindowsTextInput.RUN_BACKGROUND | WindowsTextInput.RUN_TARGET,
            0, 0xff0078d7,
            4, 6, WindowsTextInput.RUN_UNDERLINE, 0, 0,
        };
        Editor e = editor("ab", 2);
        WindowsTextInput.apply(e, 2, 2, "今日は", 2, 5, 5, 5, runs);
        UnderlineSpan[] lines = e.text.getSpans(2, 5, UnderlineSpan.class);
        BackgroundColorSpan[] backs = e.text.getSpans(2, 5, BackgroundColorSpan.class);
        boolean target = backs.length == 1 && e.text.getSpanStart(backs[0]) == 2 && e.text.getSpanEnd(backs[0]) == 4
                && backs[0].getBackgroundColor() == 0xff0078d7;
        check(lines.length >= 2 && target, "the text service's look: each clause underlined, the target highlighted");
        WindowsTextInput.apply(e, 2, 5, "", -1, -1, 5, 5, null);
        check(e.text.getSpans(0, e.text.length(), BackgroundColorSpan.class).length == 0,
                "the composition finished: its look goes with it");
        check(WindowsTextInput.styled("abc", null, 0) instanceof String, "no runs: the editor's own underline");
    }

    private static void copies() {
        // The editor's copy: its text, its offset and the selection's direction.
        Editor e = new Editor("hello world", 9, 3);
        WindowsTextInput.Copy c = WindowsTextInput.read(e, e.text, InputType.TYPE_CLASS_TEXT);
        check(c.text.equals("hello world") && c.offset == 0 && c.anchor == 9 && c.caret == 3,
                "a copy keeps the selection's direction");
        // A long text: a window around the caret, never half an emoji at its edges.
        StringBuilder sb = new StringBuilder();
        while (sb.length() < WindowsTextInput.REACH + 10) sb.append("😀");
        final int caret = sb.length() - 1;  // inside the text's end, on an even unit
        e = new Editor(sb.toString(), caret - 1, caret - 1);
        c = WindowsTextInput.read(e, e.text, InputType.TYPE_CLASS_TEXT);
        check(c.offset > 0 && !Character.isLowSurrogate(c.text.charAt(0)) && c.offset + c.text.length() <= sb.length()
                && c.anchor == caret - 1, "a long text's window starts on a whole character, at " + c.offset);
        // A password's text never leaves the app.
        e = editor("secret", 6);
        c = WindowsTextInput.read(e, e.text, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        check(c.text.isEmpty() && c.caret == 0, "a password's copy is empty");
        check(WindowsTextInput.secret(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_VARIATION_PASSWORD)
                && WindowsTextInput.secret(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_WEB_PASSWORD)
                && WindowsTextInput.secret(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD)
                && !WindowsTextInput.secret(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI),
                "PINs and web and visible passwords are secret too; a URL is not");
        // An editor with no Editable to watch: its surrounding text.
        e = new Editor("abcdef", 4, 4);
        c = WindowsTextInput.read(e, null, InputType.TYPE_CLASS_TEXT);
        check(c.text.equals("abcdef") && c.offset == 0 && c.caret == 4, "an unwatched editor: getSurroundingText");
    }
}
