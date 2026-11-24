// AccessibilityTreeTest -- what a screen reader is given of an app's views
// (com.macapk.loader.HostAccessibility's snapshot, windows/host/uia_provider.h), from the real
// View classes of Win.apk's framework on a plain JVM, no window: Hebrew text as names, the
// reading order (right to left in a right-to-left layout, as AOSP's ViewLocationHolder sorts),
// a layout that is not important for accessibility left out with its children lifted, a gone
// view left out, a check box's state, a field's value and hint, a seek bar's range, and actions
// reaching the views (a click, a new text, a progress).
//
//   javac -cp "<runtime>/lib/android-framework.jar;<runtime>/lib/macapk-loader.jar" -d out \
//         windows/test/java/com/macapk/loader/AccessibilityTreeTest.java
//   MACAPK_LOG_LEVEL=7 java -Djava.awt.headless=true \
//         -cp "<runtime>/lib/android-framework.jar;<runtime>/lib/macapk-loader.jar;out" \
//         com.macapk.loader.AccessibilityTreeTest
//
// Exits 0 when every check passes.
package com.macapk.loader;

import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class AccessibilityTreeTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "[PASS] " : "[FAIL] ") + what);
        if (!ok) failures++;
    }

    /** A snapshot's node lines by id, each its fields. */
    static Map<String, String[]> nodes(String snapshot) {
        Map<String, String[]> out = new HashMap<>();
        String[] lines = snapshot.split("\n");
        for (int i = 1; i < lines.length; i++) {
            String[] f = lines[i].split("\t", -1);
            out.put(f[0], f);
        }
        return out;
    }

    static String[] byName(Map<String, String[]> nodes, String name) {
        for (String[] f : nodes.values()) if (f[3].equals(name)) return f;
        return null;
    }

    /** A row whose layout direction the test sets: a view resolves right to left only in an app
     *  whose manifest says supportsRtl, and these views have no context. */
    static final class RtlRow extends LinearLayout {
        boolean rtl;
        RtlRow() {
            super(null);
            setOrientation(LinearLayout.HORIZONTAL);
        }
        @Override public int getLayoutDirection() {
            return rtl ? View.LAYOUT_DIRECTION_RTL : View.LAYOUT_DIRECTION_LTR;
        }
    }

    static void place(View v, int l, int t, int r, int b) {
        v.measure(View.MeasureSpec.makeMeasureSpec(r - l, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(b - t, View.MeasureSpec.EXACTLY));
        v.layout(l, t, r, b);
    }

    public static void main(String[] args) {
        FrameLayout root = new FrameLayout(null);
        // A plain layout: not important for accessibility, so its children are the root's.
        RtlRow row = new RtlRow();
        TextView first = new TextView(null);
        first.setText("ראשון");   // "first"
        first.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_YES);
        TextView second = new TextView(null);
        second.setText("שני");               // "second"
        second.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_YES);
        row.addView(first);
        row.addView(second);
        Button ok = new Button(null);
        ok.setText("אישור");       // "OK"
        final boolean[] clicked = {false};
        ok.setOnClickListener(v -> clicked[0] = true);
        CheckBox remember = new CheckBox(null);
        remember.setText("זכור");
        remember.setChecked(true);
        EditText name = new EditText(null);
        name.setHint("שם");
        SeekBar volume = new SeekBar(null);
        volume.setFocusable(true);  // the theme's Widget.SeekBar style makes it so in an app
        volume.setMax(100);
        volume.setProgress(30);
        TextView gone = new TextView(null);
        gone.setText("gone");
        gone.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_YES);
        gone.setVisibility(View.GONE);
        root.addView(row);
        root.addView(ok);
        root.addView(remember);
        root.addView(name);
        root.addView(volume);
        root.addView(gone);
        place(root, 0, 0, 400, 600);
        place(row, 0, 0, 400, 50);
        place(first, 0, 0, 200, 50);
        place(second, 200, 0, 400, 50);
        place(ok, 0, 60, 200, 110);
        place(remember, 0, 120, 200, 170);
        place(name, 0, 180, 400, 230);
        place(volume, 0, 240, 400, 280);
        root.macapkDispatchAttachedToWindow();

        List<View> roots = new ArrayList<>();
        roots.add(root);
        String snap = HostAccessibility.snapshot(roots);
        Map<String, String[]> n = nodes(snap);
        check(snap.startsWith("W1\t"), "a snapshot starts with its header");
        String[] f1 = byName(n, "ראשון"), f2 = byName(n, "שני");
        check(f1 != null && f2 != null, "the Hebrew texts are names");
        String[] rootLine = null;
        for (String[] f : n.values()) if (f[1].equals("0")) rootLine = f;
        check(rootLine != null, "the window's root is a node");
        List<String> kids = rootLine != null && !rootLine[8].isEmpty()
                ? java.util.Arrays.asList(rootLine[8].split(",")) : new ArrayList<>();
        check(f1 != null && f2 != null && kids.contains(f1[0]) && kids.contains(f2[0]),
                "a layout not important for accessibility is left out: its children are the root's");
        check(f1 != null && f2 != null && kids.indexOf(f1[0]) < kids.indexOf(f2[0]),
                "left to right: the left text is read first");
        check(byName(n, "gone") == null, "a gone view is left out");
        String[] okLine = byName(n, "אישור");
        check(okLine != null && (Integer.parseInt(okLine[6]) & 8) != 0 && (Integer.parseInt(okLine[10]) & 512) != 0,
                "a button is clickable and can be clicked");
        String[] cb = byName(n, "זכור");
        check(cb != null && (Integer.parseInt(cb[6]) & (16 | 32)) == (16 | 32), "a checked check box is checkable and checked");
        String[] field = byName(n, "שם");
        check(field != null && (Integer.parseInt(field[6]) & 64) != 0 && field[4].isEmpty()
                && (Integer.parseInt(field[10]) & 128) != 0,
                "an empty field is named by its hint, has no value, and takes a text");
        String[] seek = null;
        for (String[] f : n.values()) if (f[2].endsWith("SeekBar")) seek = f;
        check(seek != null && seek[11].equals("0.0,100.0,30.0") && (Integer.parseInt(seek[10]) & 64) != 0,
                "a seek bar has its range and takes a progress");

        // Actions, as the host's worker hands them to the main thread.
        check(okLine != null && HostAccessibility.doPerform(Long.parseLong(okLine[0]), 1, "") && clicked[0],
                "a click reaches the button's listener");
        check(field != null && HostAccessibility.doPerform(Long.parseLong(field[0]), 3, "דנה")
                && "דנה".contentEquals(name.getText()), "a new Hebrew text reaches the field");
        check(seek != null && HostAccessibility.doPerform(Long.parseLong(seek[0]), 12, "55")
                && volume.getProgress() == 55, "a progress reaches the seek bar");
        n = nodes(HostAccessibility.snapshot(roots));
        field = null;
        for (String[] f : n.values()) if (f[2].endsWith("EditText")) field = f;
        check(field != null && field[4].equals("דנה"), "the field's new text is its value");

        // Right to left: the right text is read first.
        row.rtl = true;
        n = nodes(HostAccessibility.snapshot(roots));
        f1 = byName(n, "ראשון");
        f2 = byName(n, "שני");
        rootLine = null;
        for (String[] f : n.values()) if (f[1].equals("0")) rootLine = f;
        kids = rootLine != null && !rootLine[8].isEmpty()
                ? java.util.Arrays.asList(rootLine[8].split(",")) : new ArrayList<>();
        check(f1 != null && f2 != null && kids.indexOf(f2[0]) >= 0 && kids.indexOf(f2[0]) < kids.indexOf(f1[0]),
                "right to left: the text on the right is read first");

        System.out.println(failures == 0 ? "ok" : "FAILED (" + failures + ")");
        System.exit(failures == 0 ? 0 : 1);
    }
}
