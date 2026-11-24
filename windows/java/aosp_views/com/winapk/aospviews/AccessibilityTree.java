// What a screen reader is told of the app's windows: every shown window's AccessibilityNodeInfo
// tree, read in process the way AOSP's AccessibilityInteractionController answers TalkBack, as
// one snapshot in the text windows/host/uia_tree.h reads (com.macapk.loader.HostAccessibility
// sends it to the host's UI Automation provider).
//
// A view's node is View.createAccessibilityNodeInfo(), which asks the view's
// AccessibilityNodeProvider for its host node when it has one; the node's child ids say where
// each child is: another view (found by its accessibility view id under this one) or a virtual
// node of the same view's provider (Compose's semantics, a custom view's virtual hierarchy, a
// keyboard's keys), which the provider creates. Only what AOSP puts in the tree is walked: the
// views important for accessibility, in AOSP's reading order. A WebView's page is not walked
// here: WebView2's own child window answers UI Automation for it.
//
// For an editor (and selectable text) the snapshot carries what the Text pattern reads by: the
// TextView's Layout's lines and the x of each offset, and the word breaks of Java's BreakIterator
// in the app's locale (Hebrew, Thai, Japanese words), so that a screen reader reads by character,
// word and line where the app shows them.
//
// Main thread only. perform() runs an action on a node the last snapshot named, as
// AccessibilityInteractionController.performAccessibilityActionUiThread does.
package com.winapk.aospviews;

import android.graphics.Rect;
import android.os.Bundle;
import android.text.Layout;
import android.view.View;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.widget.TextView;

import java.lang.ref.WeakReference;
import java.text.BreakIterator;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public final class AccessibilityTree {
    private AccessibilityTree() {}

    private static final char RECORD = '\u001e', FIELD = '\u001f';
    /** Bounds on one snapshot: a runaway virtual tree must not stall the main thread. */
    static final int MAX_NODES = 5000, MAX_DEPTH = 100, MAX_TEXT_LAYOUT = 4000, MAX_WORDS_TEXT = 20000;
    private static final String ROLE_DESCRIPTION = "AccessibilityNodeInfo.roleDescription";

    // uia_tree.h's flags.
    static final int ENABLED = 1, FOCUSABLE = 1 << 1, FOCUSED = 1 << 2, CLICKABLE = 1 << 3, LONG_CLICKABLE = 1 << 4,
            CHECKABLE = 1 << 5, CHECKED = 1 << 6, EDITABLE = 1 << 7, PASSWORD = 1 << 8, VISIBLE = 1 << 9,
            SCROLLABLE = 1 << 10, HEADING = 1 << 11, SELECTED = 1 << 12, MULTI_LINE = 1 << 13,
            CONTENT_INVALID = 1 << 14, SHOWING_HINT = 1 << 15, RTL = 1 << 16, SCREEN_READER_FOCUSABLE = 1 << 17,
            TEXT_SELECTABLE = 1 << 18;

    /** Where a node of the last snapshot lives: its view, and the provider's virtual id. */
    private static final class Target {
        final WeakReference<View> view;
        final int virtualId;

        Target(View view, int virtualId) {
            this.view = new WeakReference<>(view);
            this.virtualId = virtualId;
        }
    }

    private static Map<Long, Target> sTargets = new HashMap<>();
    private static java.lang.reflect.Field sLabeledBy;
    private static boolean sLabeledByLooked;

    /** The snapshot of these windows' roots, bottom to top (AospWindows.accessibilityRoots). */
    public static String snapshot(List<View> roots) {
        Walk w = new Walk();
        StringBuilder windows = new StringBuilder();
        long focused = 0, focusedAny = 0;
        for (View root : roots) {
            if (root == null || !root.isAttachedToWindow()) continue;
            long rootId = w.view(root, 0, 0);
            if (rootId == 0) continue;
            WindowManager.LayoutParams lp = root.getLayoutParams() instanceof WindowManager.LayoutParams
                    ? (WindowManager.LayoutParams) root.getLayoutParams() : null;
            boolean hasFocus = root.hasWindowFocus();
            windows.append(RECORD).append('W');
            field(windows, Long.toString(rootId));
            field(windows, Integer.toString(lp != null ? lp.type : 0));
            field(windows, clean(lp != null ? lp.accessibilityTitle : null));
            field(windows, hasFocus ? "1" : "0");
            // Input focus: the focused node of the window with window focus, else the topmost one's.
            if (w.focusedInWindow != 0) {
                focusedAny = w.focusedInWindow;
                if (hasFocus) focused = w.focusedInWindow;
            }
            w.focusedInWindow = 0;
        }
        if (focused == 0) focused = focusedAny;
        sTargets = w.targets;
        StringBuilder out = new StringBuilder(64 + windows.length() + w.nodes.length());
        out.append('S');
        field(out, "1");
        field(out, Long.toString(focused));
        field(out, Locale.getDefault().toLanguageTag());
        out.append(windows).append(w.nodes);
        return out.toString();
    }

    /** Performs an action on a node the last snapshot named; false when it is gone or refused it. */
    public static boolean perform(long nodeId, int action, CharSequence text, int arg1, int arg2, float value) {
        Target t = sTargets.get(nodeId);
        View v = t != null ? t.view.get() : null;
        if (v == null || !v.isAttachedToWindow()) return false;
        Bundle args = null;
        if (action == AccessibilityNodeInfo.ACTION_SET_TEXT) {
            args = new Bundle();
            args.putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, text);
        } else if (action == AccessibilityNodeInfo.ACTION_SET_SELECTION) {
            args = new Bundle();
            args.putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, arg1);
            args.putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, arg2);
        } else if (action == android.R.id.accessibilityActionSetProgress) {
            args = new Bundle();
            args.putFloat(AccessibilityNodeInfo.ACTION_ARGUMENT_PROGRESS_VALUE, value);
        }
        AccessibilityNodeProvider provider = v.getAccessibilityNodeProvider();
        if (t.virtualId != AccessibilityNodeInfo.UNDEFINED_ITEM_ID && t.virtualId != AccessibilityNodeProvider.HOST_VIEW_ID) {
            return provider != null && provider.performAction(t.virtualId, action, args);
        }
        if (provider != null) return provider.performAction(AccessibilityNodeProvider.HOST_VIEW_ID, action, args);
        return v.performAccessibilityAction(action, args);
    }

    // ---- The walk ----------------------------------------------------------------------------------

    private static final class Walk {
        final StringBuilder nodes = new StringBuilder();
        final Map<Long, Target> targets = new HashMap<>();
        int count;
        long focusedInWindow;
        final Rect bounds = new Rect();

        /** A view's node and what is under it; its id, or 0 when it gives none. */
        long view(View v, long parent, int depth) {
            if (count >= MAX_NODES || depth > MAX_DEPTH) return 0;
            AccessibilityNodeInfo info;
            try {
                info = v.createAccessibilityNodeInfo();
            } catch (RuntimeException e) {
                return 0;  // a view whose accessibility code throws: its subtree is left out
            }
            if (info == null) return 0;
            long id = AccessibilityNodeInfo.makeNodeId(v.getAccessibilityViewId(), AccessibilityNodeInfo.UNDEFINED_ITEM_ID);
            node(info, id, parent, v, AccessibilityNodeInfo.UNDEFINED_ITEM_ID, depth);
            return id;
        }

        /** A virtual node of v's provider. */
        long virtual(View v, AccessibilityNodeProvider provider, int virtualId, long parent, int depth) {
            if (count >= MAX_NODES || depth > MAX_DEPTH) return 0;
            AccessibilityNodeInfo info;
            try {
                info = provider.createAccessibilityNodeInfo(virtualId);
            } catch (RuntimeException e) {
                return 0;
            }
            if (info == null) return 0;
            long id = AccessibilityNodeInfo.makeNodeId(v.getAccessibilityViewId(), virtualId);
            node(info, id, parent, v, virtualId, depth);
            return id;
        }

        void node(AccessibilityNodeInfo info, long id, long parent, View v, int virtualId, int depth) {
            if (targets.containsKey(id)) return;  // a tree that loops
            ++count;
            targets.put(id, new Target(v, virtualId));
            // Children first, so that only the ones that gave a node are listed.
            StringBuilder kids = new StringBuilder();
            for (int i = 0; i < info.getChildCount(); i++) {
                long childId = info.getChildId(i);
                int viewId = AccessibilityNodeInfo.getAccessibilityViewId(childId);
                int childVirtual = AccessibilityNodeInfo.getVirtualDescendantId(childId);
                long got = 0;
                if (childVirtual == AccessibilityNodeInfo.UNDEFINED_ITEM_ID
                        || childVirtual == AccessibilityNodeProvider.HOST_VIEW_ID) {
                    View child = viewId == v.getAccessibilityViewId() ? null : v.findViewByAccessibilityIdTraversal(viewId);
                    if (child == null && v.getRootView() != null) child = v.getRootView().findViewByAccessibilityIdTraversal(viewId);
                    if (child != null && child != v) got = view(child, id, depth + 1);
                } else {
                    View host = viewId == v.getAccessibilityViewId() ? v
                            : v.getRootView() != null ? v.getRootView().findViewByAccessibilityIdTraversal(viewId) : null;
                    AccessibilityNodeProvider provider = host != null ? host.getAccessibilityNodeProvider() : null;
                    if (provider != null) got = virtual(host, provider, childVirtual, id, depth + 1);
                }
                if (got != 0) {
                    if (kids.length() > 0) kids.append(',');
                    kids.append(got);
                }
            }
            write(info, id, parent, v, virtualId, kids);
        }

        void write(AccessibilityNodeInfo info, long id, long parent, View v, int virtualId, CharSequence kids) {
            final boolean isView = virtualId == AccessibilityNodeInfo.UNDEFINED_ITEM_ID;
            int flags = 0;
            if (info.isEnabled()) flags |= ENABLED;
            if (info.isFocusable()) flags |= FOCUSABLE;
            if (info.isFocused()) flags |= FOCUSED;
            if (info.isClickable()) flags |= CLICKABLE;
            if (info.isLongClickable()) flags |= LONG_CLICKABLE;
            if (info.isCheckable()) flags |= CHECKABLE;
            if (info.isChecked()) flags |= CHECKED;
            if (info.isEditable()) flags |= EDITABLE;
            if (info.isPassword()) flags |= PASSWORD;
            if (info.isVisibleToUser()) flags |= VISIBLE;
            if (info.isScrollable()) flags |= SCROLLABLE;
            if (info.isHeading()) flags |= HEADING;
            if (info.isSelected()) flags |= SELECTED;
            if (info.isMultiLine()) flags |= MULTI_LINE;
            if (info.isContentInvalid()) flags |= CONTENT_INVALID;
            if (info.isShowingHintText()) flags |= SHOWING_HINT;
            if (info.isScreenReaderFocusable()) flags |= SCREEN_READER_FOCUSABLE;
            if (info.isTextSelectable()) flags |= TEXT_SELECTABLE;
            if (info.isFocused()) focusedInWindow = id;

            CharSequence text = info.getText();
            TextView tv = isView && v instanceof TextView ? (TextView) v : null;
            // A password's characters are not given to a screen reader (AOSP leaves its text out):
            // one dot each, so that it can say how many there are.
            if (text == null && info.isPassword() && tv != null && tv.getText() != null) {
                text = "•".repeat(tv.getText().length());
            }
            String textValue = text != null ? text.toString() : "";
            Layout layout = null;
            if (tv != null && (info.isEditable() || info.isTextSelectable()) && !info.isShowingHintText()) {
                layout = tv.getLayout();
                if (layout != null && layout.getText().length() != textValue.length()) layout = null;
            }
            boolean rtl = layout != null && layout.getLineCount() > 0
                    ? layout.getParagraphDirection(0) == Layout.DIR_RIGHT_TO_LEFT
                    : v.isLayoutRtl();
            if (rtl) flags |= RTL;

            nodes.append(RECORD).append('N');
            field(nodes, Long.toString(id));
            field(nodes, Long.toString(parent));
            field(nodes, Integer.toString(flags));
            info.getBoundsInScreen(bounds);
            field(nodes, bounds.left + "," + bounds.top + "," + bounds.right + "," + bounds.bottom);
            field(nodes, clean(info.getClassName()));
            Bundle extras = info.getExtras();
            field(nodes, clean(extras != null ? extras.getCharSequence(ROLE_DESCRIPTION) : null));
            field(nodes, clean(info.getContentDescription()));
            field(nodes, clean(textValue));
            field(nodes, clean(info.getHintText()));
            field(nodes, clean(info.getTooltipText()));
            field(nodes, clean(info.getStateDescription()));
            field(nodes, clean(info.getPaneTitle()));
            field(nodes, clean(info.getError()));
            field(nodes, clean(info.getViewIdResourceName()));
            field(nodes, Integer.toString(info.getLiveRegion()));
            AccessibilityNodeInfo.RangeInfo range = info.getRangeInfo();
            field(nodes, range == null ? "" : range.getType() + "," + range.getMin() + "," + range.getMax() + "," + range.getCurrent());
            AccessibilityNodeInfo.CollectionInfo coll = info.getCollectionInfo();
            field(nodes, coll == null ? "" : coll.getRowCount() + "," + coll.getColumnCount() + ","
                    + (coll.isHierarchical() ? 1 : 0) + "," + coll.getSelectionMode());
            AccessibilityNodeInfo.CollectionItemInfo item = info.getCollectionItemInfo();
            field(nodes, item == null ? "" : item.getRowIndex() + "," + item.getRowSpan() + "," + item.getColumnIndex() + ","
                    + item.getColumnSpan() + "," + (item.isHeading() ? 1 : 0) + "," + (item.isSelected() ? 1 : 0));
            int selStart = info.getTextSelectionStart(), selEnd = info.getTextSelectionEnd();
            field(nodes, text != null && (selStart >= 0 || selEnd >= 0) ? selStart + "," + selEnd : "");
            textLayout(tv, layout, textValue.length());
            field(nodes, words(textValue, info));
            field(nodes, kids.toString());
            StringBuilder actions = new StringBuilder();
            for (AccessibilityNodeInfo.AccessibilityAction a : info.getActionList()) {
                if (actions.length() > 0) actions.append(',');
                actions.append(a.getId());
            }
            field(nodes, actions.toString());
            field(nodes, Long.toString(labeledBy(info)));
        }

        /** Lines, their boxes and the x of each offset, in screen pixels (getBoundsInScreen's). */
        void textLayout(TextView tv, Layout layout, int length) {
            if (layout == null) {
                field(nodes, "");
                field(nodes, "");
                field(nodes, "");
                return;
            }
            int[] at = new int[2];
            tv.getLocationOnScreen(at);
            int ox = at[0] + tv.getTotalPaddingLeft() - tv.getScrollX();
            int oy = at[1] + tv.getTotalPaddingTop() - tv.getScrollY();
            StringBuilder starts = new StringBuilder(), boxes = new StringBuilder();
            for (int i = 0; i < layout.getLineCount(); i++) {
                if (i > 0) {
                    starts.append(',');
                    boxes.append(',');
                }
                starts.append(layout.getLineStart(i));
                boxes.append(ox + (int) Math.floor(layout.getLineLeft(i))).append(',')
                        .append(oy + layout.getLineTop(i)).append(',')
                        .append(ox + (int) Math.ceil(layout.getLineRight(i))).append(',')
                        .append(oy + layout.getLineBottom(i));
            }
            field(nodes, starts.toString());
            field(nodes, boxes.toString());
            StringBuilder xs = new StringBuilder();
            if (length <= MAX_TEXT_LAYOUT) {
                for (int off = 0; off <= length; off++) {
                    if (off > 0) xs.append(',');
                    xs.append(ox + Math.round(layout.getPrimaryHorizontal(off)));
                }
            }
            field(nodes, xs.toString());
        }
    }

    /** Java's word boundaries for an editor's or selectable text, in the app's locale. */
    static String words(String text, AccessibilityNodeInfo info) {
        if (text.isEmpty() || text.length() > MAX_WORDS_TEXT || !(info.isEditable() || info.isTextSelectable())) return "";
        BreakIterator it = BreakIterator.getWordInstance(Locale.getDefault());
        it.setText(text);
        StringBuilder out = new StringBuilder();
        for (int b = it.first(); b != BreakIterator.DONE; b = it.next()) {
            if (out.length() > 0) out.append(',');
            out.append(b);
        }
        return out.toString();
    }

    /** The node labelling this one (View.setLabelFor's): the id AOSP keeps for a screen reader to
     *  fetch through its connection, which an in-process reader has no use for. */
    private static long labeledBy(AccessibilityNodeInfo info) {
        if (!sLabeledByLooked) {
            sLabeledByLooked = true;
            try {
                sLabeledBy = AccessibilityNodeInfo.class.getDeclaredField("mLabeledById");
                sLabeledBy.setAccessible(true);
            } catch (ReflectiveOperationException | RuntimeException e) {
                sLabeledBy = null;
            }
        }
        if (sLabeledBy == null) return 0;
        try {
            long id = sLabeledBy.getLong(info);
            if (id == AccessibilityNodeInfo.UNDEFINED_NODE_ID) return 0;
            // setLabeledBy(View) names the view's host node (HOST_VIEW_ID); its node is the view's own.
            int virtual = AccessibilityNodeInfo.getVirtualDescendantId(id);
            return virtual == AccessibilityNodeProvider.HOST_VIEW_ID
                    ? AccessibilityNodeInfo.makeNodeId(AccessibilityNodeInfo.getAccessibilityViewId(id),
                            AccessibilityNodeInfo.UNDEFINED_ITEM_ID)
                    : id;
        } catch (ReflectiveOperationException | RuntimeException e) {
            return 0;
        }
    }

    private static void field(StringBuilder out, String s) {
        out.append(FIELD).append(s);
    }

    /** A string for a field: the separators never appear in it. */
    static String clean(CharSequence s) {
        if (s == null || s.length() == 0) return "";
        String t = s.toString();
        return t.indexOf(RECORD) < 0 && t.indexOf(FIELD) < 0 ? t : t.replace(RECORD, ' ').replace(FIELD, ' ');
    }
}
