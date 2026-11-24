/*
 * Adapted from the other runtime (https://example.invalid/other-runtime),
 * app/java/com/other/app/accessibility/UiaBridge.java,
 * Copyright (C) 2026 its authors, SPDX-License-Identifier: Apache-2.0. Modified for Win.apk.
 */
// com.macapk.loader.HostAccessibility -- the app's views for the host's screen readers (on
// Windows, UI Automation: windows/host/uia_provider.h, native_accessibility.cpp), read the way
// TalkBack reads them on a phone: each window's root view, its children for accessibility
// (ViewGroup.addChildrenForAccessibility: the views important for accessibility, in AOSP's
// reading order, right to left in a right-to-left layout), each one's AccessibilityNodeInfo
// (View.createAccessibilityNodeInfo, or its AccessibilityNodeProvider's nodes: Compose, a
// WebView, any virtual hierarchy), and its actions through performAccessibilityAction. Works
// the same on Win.apk's own views and on AOSP's (WINAPK_AOSP_VIEWS=1): only public framework
// API and ViewHost's windows.
//
// Lazy, as Android is with no accessibility service on: nothing here runs until the host says a
// client asked (activate). Then AccessibilityManager.isEnabled() is true, as while TalkBack runs,
// and on the main thread every TICK_MS the tree is published if the host saw a frame since the
// last (nativeDue); the host turns it off again when no client has asked for a minute.
//
// the other runtime reads the nodes through ART's AccessibilityInteractionController from UIA's threads;
// here the main thread builds a snapshot and the host answers from it, so no host thread waits
// on the app. The snapshot's text is described in uia_provider.h.
package com.macapk.loader;

import android.graphics.Rect;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;

import com.macapk.loader.view.ViewHost;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.WeakHashMap;

public final class HostAccessibility {
    private HostAccessibility() {}

    private static final long TICK_MS = 250;
    private static final int MAX_NODES = 4000;
    private static final int MAX_TEXT = 2000;
    private static final int HOST_VIEW_ID = -1;           // AccessibilityNodeProvider.HOST_VIEW_ID
    private static final int UNDEFINED_ITEM_ID = Integer.MAX_VALUE;  // AccessibilityNodeInfo's

    // Node flags and what a node can do (uia_provider.h: Flag, Can).
    private static final int ENABLED = 1, FOCUSABLE = 2, FOCUSED = 4, CLICKABLE = 8, CHECKABLE = 16,
            CHECKED = 32, EDITABLE = 64, PASSWORD = 128, VISIBLE = 256, SCROLLABLE = 512, HEADING = 1024,
            SELECTED = 2048, LONG_CLICKABLE = 4096, RANGE = 8192, COLLECTION = 16384, ITEM = 32768,
            MULTILINE = 65536;
    private static final int CAN_FORWARD = 1, CAN_BACKWARD = 2, CAN_UP = 4, CAN_DOWN = 8, CAN_LEFT = 16,
            CAN_RIGHT = 32, CAN_PROGRESS = 64, CAN_SET_TEXT = 128, CAN_SHOW = 256, CAN_CLICK = 512;

    // AccessibilityNodeInfo's action ids and argument keys, as AOSP defines them.
    private static final int ACTION_FOCUS = 0x1, ACTION_CLICK = 0x10, ACTION_SCROLL_FORWARD = 0x1000,
            ACTION_SCROLL_BACKWARD = 0x2000, ACTION_SET_TEXT = 0x200000, ACTION_SHOW_ON_SCREEN = 0x01020036,
            ACTION_SCROLL_UP = 0x01020038, ACTION_SCROLL_LEFT = 0x01020039, ACTION_SCROLL_DOWN = 0x0102003a,
            ACTION_SCROLL_RIGHT = 0x0102003b, ACTION_SET_PROGRESS = 0x0102003d;
    private static final String ARG_SET_TEXT = "ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE";
    private static final String ARG_PROGRESS = "android.view.accessibility.action.ARGUMENT_PROGRESS_VALUE";

    private static volatile boolean sInstalled;
    private static volatile boolean sActive;
    private static Handler sMain;

    // Main thread only: each view's serial (a node's id), and what the last snapshot's ids
    // stand for, so an action finds its view.
    private static final WeakHashMap<View, Integer> sSerials = new WeakHashMap<>();
    private static int sNextSerial = 1;
    private static final Map<Long, Target> sTargets = new HashMap<>();

    private static final class Target {
        final View view;
        final AccessibilityNodeProvider provider;
        final int virtualId;

        Target(View view, AccessibilityNodeProvider provider, int virtualId) {
            this.view = view;
            this.provider = provider;
            this.virtualId = virtualId;
        }
    }

    private static native void nativeInstall();
    private static native boolean nativeDue();
    private static native boolean nativePublish(String snapshot);
    private static native void nativeAnnounce(String text, boolean assertive);

    /** At the window's creation (Boot): the host may now call in. A host without the natives
     *  (the Mac) has no screen-reader bridge, and every call here is a no-op. */
    public static void install() {
        if (sInstalled) return;
        try {
            nativeInstall();
            sInstalled = true;
        } catch (UnsatisfiedLinkError e) {
            // No host bridge.
        }
    }

    /** Whether a screen reader is reading the app (AccessibilityManager.isEnabled()). */
    public static boolean active() {
        return sActive;
    }

    /** An AccessibilityEvent the app sent while a screen reader reads it: an announcement is
     *  spoken; anything else may have changed the tree, which is published on the next tick. */
    public static void onEvent(int type, CharSequence text, boolean assertive) {
        if (!sActive) return;
        if (type == 0x00004000 /* TYPE_ANNOUNCEMENT */ && text != null && text.length() > 0) {
            try {
                nativeAnnounce(clean(text), assertive);
            } catch (UnsatisfiedLinkError ignored) {
                // No host bridge.
            }
        }
    }

    // ---- From the host's worker thread: everything goes to the main thread ----

    static void activate(boolean on) {
        main().post(() -> setActive(on));
    }

    static void refresh() {
        main().post(HostAccessibility::publish);
    }

    static void perform(long id, int action, String text) {
        main().post(() -> {
            boolean done = doPerform(id, action, text);
            if (!done) System.err.println("[win.apk] accessibility: action " + action + " on node " + id + " not done");
            publish();
        });
    }

    private static Handler main() {
        Handler h = sMain;
        if (h == null) {
            h = new Handler(Looper.getMainLooper());
            sMain = h;
        }
        return h;
    }

    private static final Runnable TICK = new Runnable() {
        @Override public void run() {
            if (!sActive) return;
            if (nativeDue()) publish();
            if (sActive) main().postDelayed(this, TICK_MS);
        }
    };

    private static void setActive(boolean on) {
        if (sActive == on) return;
        sActive = on;
        System.err.println("[win.apk] accessibility: " + (on
                ? "a screen reader is reading the app (UI Automation); accessibility is on"
                : "no screen reader has asked for a minute; accessibility is off"));
        if (!on) sTargets.clear();
        try {
            ViewHost.get().accessibilityEnabled(on);
        } catch (Throwable t) {
            System.err.println("[win.apk] accessibility: the framework's state: " + t);
        }
        if (on) main().post(TICK);
    }

    private static void publish() {
        String snapshot;
        try {
            snapshot = snapshot();
        } catch (Throwable t) {
            // An app's own node code threw: this snapshot is skipped, the next tick tries again.
            System.err.println("[win.apk] accessibility: the view tree could not be read: " + t);
            return;
        }
        if (!nativePublish(snapshot) && sActive) setActive(false);
    }

    // ---- The snapshot (main thread) ----

    private static final class Walk {
        final StringBuilder lines = new StringBuilder();
        final Map<Long, String> names = new HashMap<>();
        final Set<Long> actionable = new HashSet<>();
        final Set<Long> seen = new HashSet<>();
        final Rect bounds = new Rect();
        long focused;
        int count;
    }

    static String snapshot() {
        return snapshot(ViewHost.get().accessibilityRoots());
    }

    /** The snapshot of these windows' roots, bottom to top (a test gives its own). */
    static String snapshot(List<View> roots) {
        sTargets.clear();
        Walk w = new Walk();
        StringBuilder top = new StringBuilder();
        for (View root : roots) {
            if (root == null || root.getVisibility() != View.VISIBLE || !root.isAttachedToWindow()) continue;
            long id = addView(w, root, 0);
            if (id == 0) continue;
            if (top.length() > 0) top.append(',');
            top.append(id);
        }
        return "W1\t" + w.focused + "\t" + Locale.getDefault().toLanguageTag() + "\t" + top + "\n" + w.lines;
    }

    private static long serialOf(View v) {
        Integer s = sSerials.get(v);
        if (s == null) {
            s = sNextSerial++;
            if (sNextSerial <= 0) sNextSerial = 1;
            sSerials.put(v, s);
        }
        return s & 0xffffffffL;
    }

    /** A real view's node, its children's first. 0 when it gives none. */
    private static long addView(Walk w, View v, long parent) {
        if (w.count >= MAX_NODES) return 0;
        long id = serialOf(v);
        if (!w.seen.add(id)) return 0;
        AccessibilityNodeProvider provider = v.getAccessibilityNodeProvider();
        AccessibilityNodeInfo info = provider != null ? provider.createAccessibilityNodeInfo(HOST_VIEW_ID)
                : v.createAccessibilityNodeInfo();
        if (info == null) return 0;
        w.count++;
        sTargets.put(id, new Target(v, provider, HOST_VIEW_ID));
        if (info.isFocused()) w.focused = id;
        List<Long> kids = new ArrayList<>();
        if (provider != null) {
            addVirtualChildren(w, v, provider, info, id, kids);
        } else if (v instanceof ViewGroup) {
            ArrayList<View> children = new ArrayList<>();
            ((ViewGroup) v).addChildrenForAccessibility(children);
            for (View c : children) {
                long k = addView(w, c, id);
                if (k != 0) kids.add(k);
            }
        }
        emit(w, id, parent, info, kids);
        return id;
    }

    private static void addVirtualChildren(Walk w, View host, AccessibilityNodeProvider provider,
            AccessibilityNodeInfo info, long id, List<Long> kids) {
        for (int i = 0; i < info.getChildCount(); i++) {
            long child = info.getChildId(i);
            int virtual = AccessibilityNodeInfo.getVirtualDescendantId(child);
            long k;
            if (virtual == UNDEFINED_ITEM_ID || virtual == HOST_VIEW_ID) {
                // A real view the provider put among its nodes (Compose's AndroidView).
                View c = findByAccessibilityId(host, AccessibilityNodeInfo.getAccessibilityViewId(child));
                k = c != null ? addView(w, c, id) : 0;
            } else {
                k = addVirtual(w, host, provider, virtual, id);
            }
            if (k != 0) kids.add(k);
        }
    }

    private static long addVirtual(Walk w, View host, AccessibilityNodeProvider provider, int virtual, long parent) {
        if (w.count >= MAX_NODES) return 0;
        long id = ((long) virtual + 1) << 32 | serialOf(host);
        if (!w.seen.add(id)) return 0;
        AccessibilityNodeInfo info = provider.createAccessibilityNodeInfo(virtual);
        if (info == null) return 0;
        w.count++;
        sTargets.put(id, new Target(host, provider, virtual));
        if (info.isFocused()) w.focused = id;
        List<Long> kids = new ArrayList<>();
        addVirtualChildren(w, host, provider, info, id, kids);
        emit(w, id, parent, info, kids);
        return id;
    }

    // AOSP's View.getAccessibilityViewId() (hidden; absent from Win.apk's View, whose nodes use
    // the identity hash instead: AccessibilityNodeInfo.accessibilityViewIdOf).
    private static java.lang.reflect.Method sViewId;
    private static boolean sViewIdLooked;

    private static int accessibilityViewId(View v) {
        if (!sViewIdLooked) {
            sViewIdLooked = true;
            try {
                sViewId = View.class.getMethod("getAccessibilityViewId");
            } catch (NoSuchMethodException e) {
                sViewId = null;
            }
        }
        if (sViewId != null) {
            try {
                return (Integer) sViewId.invoke(v);
            } catch (ReflectiveOperationException e) {
                return System.identityHashCode(v);
            }
        }
        return System.identityHashCode(v);
    }

    private static View findByAccessibilityId(View root, int viewId) {
        ArrayList<View> pending = new ArrayList<>();
        pending.add(root);
        for (int i = 0; i < pending.size() && i < MAX_NODES; i++) {
            View v = pending.get(i);
            if (v != root && accessibilityViewId(v) == viewId) return v;
            if (v instanceof ViewGroup) {
                ViewGroup g = (ViewGroup) v;
                for (int c = 0; c < g.getChildCount(); c++) pending.add(g.getChildAt(c));
            }
        }
        return null;
    }

    private static void emit(Walk w, long id, long parent, AccessibilityNodeInfo n, List<Long> kids) {
        int flags = 0;
        if (n.isEnabled()) flags |= ENABLED;
        if (n.isFocusable()) flags |= FOCUSABLE;
        if (n.isFocused()) flags |= FOCUSED;
        if (n.isClickable()) flags |= CLICKABLE;
        if (n.isLongClickable()) flags |= LONG_CLICKABLE;
        if (n.isCheckable()) flags |= CHECKABLE;
        if (n.isChecked()) flags |= CHECKED;
        if (n.isEditable()) flags |= EDITABLE;
        if (n.isPassword()) flags |= PASSWORD;
        if (n.isVisibleToUser()) flags |= VISIBLE;
        if (n.isScrollable()) flags |= SCROLLABLE;
        if (n.isSelected()) flags |= SELECTED;
        if (n.isMultiLine()) flags |= MULTILINE;
        AccessibilityNodeInfo.CollectionItemInfo item = n.getCollectionItemInfo();
        if (n.isHeading() || (item != null && item.isHeading())) flags |= HEADING;
        if (n.getCollectionInfo() != null) flags |= COLLECTION;
        if (item != null) flags |= ITEM;
        AccessibilityNodeInfo.RangeInfo range = n.getRangeInfo();
        String rangeText = "";
        if (range != null) {
            flags |= RANGE;
            rangeText = range.getMin() + "," + range.getMax() + "," + range.getCurrent();
        }
        int can = 0;
        for (AccessibilityNodeInfo.AccessibilityAction a : n.getActionList()) {
            switch (a.getId()) {
                case ACTION_SCROLL_FORWARD: can |= CAN_FORWARD; break;
                case ACTION_SCROLL_BACKWARD: can |= CAN_BACKWARD; break;
                case ACTION_SCROLL_UP: can |= CAN_UP; break;
                case ACTION_SCROLL_DOWN: can |= CAN_DOWN; break;
                case ACTION_SCROLL_LEFT: can |= CAN_LEFT; break;
                case ACTION_SCROLL_RIGHT: can |= CAN_RIGHT; break;
                case ACTION_SET_PROGRESS: can |= CAN_PROGRESS; break;
                case ACTION_SET_TEXT: can |= CAN_SET_TEXT; break;
                case ACTION_SHOW_ON_SCREEN: can |= CAN_SHOW; break;
                case ACTION_CLICK: can |= CAN_CLICK; break;
                default: break;
            }
        }
        // What TalkBack says (the other runtime's reading of a node): a field is named by its description or
        // its hint, and its text is its value; anything else by its description, else its text,
        // else, when it can be acted on, the text of the views inside it, as TalkBack joins them.
        String name;
        String value = "";
        String help = clean(n.getHintText());
        if (n.isEditable()) {
            name = clean(n.getContentDescription());
            if (name.isEmpty()) name = help;
            value = n.isShowingHintText() ? "" : clean(n.getText());
        } else {
            name = clean(n.getContentDescription());
            if (name.isEmpty()) name = clean(n.getText());
            CharSequence state = n.getStateDescription();
            if (state != null && state.length() > 0 && help.isEmpty()) help = clean(state);
        }
        boolean actionable = (flags & (CLICKABLE | LONG_CLICKABLE | CHECKABLE | FOCUSABLE)) != 0;
        if (name.isEmpty() && actionable && (flags & EDITABLE) == 0) {
            StringBuilder joined = new StringBuilder();
            for (Long k : kids) {
                String s = w.names.get(k);
                if (s == null || s.isEmpty() || w.actionable.contains(k)) continue;
                if (joined.length() > 0) joined.append(", ");
                joined.append(s);
            }
            name = joined.toString();
        }
        CharSequence error = n.getError();
        if (error != null && error.length() > 0) help = clean(error);
        w.names.put(id, name);
        if (actionable) w.actionable.add(id);
        n.getBoundsInScreen(w.bounds);
        Rect b = w.bounds;
        StringBuilder line = w.lines;
        line.append(id).append('\t').append(parent).append('\t').append(clean(n.getClassName())).append('\t')
                .append(name).append('\t').append(value).append('\t').append(help).append('\t').append(flags)
                .append('\t').append(b.left).append(',').append(b.top).append(',').append(b.right).append(',')
                .append(b.bottom).append('\t');
        for (int i = 0; i < kids.size(); i++) {
            if (i > 0) line.append(',');
            line.append(kids.get(i));
        }
        line.append('\t').append(clean(n.getViewIdResourceName())).append('\t').append(can).append('\t')
                .append(rangeText).append('\t').append(n.getLiveRegion()).append('\n');
    }

    private static String clean(CharSequence s) {
        if (s == null) return "";
        String t = s.toString();
        if (t.length() > MAX_TEXT) t = t.substring(0, MAX_TEXT);
        return t.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
    }

    // ---- Actions (main thread) ----

    /** perform's work, on the calling thread (the main thread; a test's). */
    static boolean doPerform(long id, int action, String text) {
        Target t = sTargets.get(id);
        if (t == null) return false;
        int a;
        Bundle args = null;
        switch (action) {
            case 1: a = ACTION_CLICK; break;
            case 2: a = ACTION_FOCUS; break;
            case 3:
                a = ACTION_SET_TEXT;
                args = new Bundle();
                args.putCharSequence(ARG_SET_TEXT, text);
                break;
            case 5: a = ACTION_SHOW_ON_SCREEN; break;
            case 6: a = ACTION_SCROLL_FORWARD; break;
            case 7: a = ACTION_SCROLL_BACKWARD; break;
            case 8: a = ACTION_SCROLL_UP; break;
            case 9: a = ACTION_SCROLL_DOWN; break;
            case 10: a = ACTION_SCROLL_LEFT; break;
            case 11: a = ACTION_SCROLL_RIGHT; break;
            case 12:
                a = ACTION_SET_PROGRESS;
                args = new Bundle();
                try {
                    args.putFloat(ARG_PROGRESS, Float.parseFloat(text));
                } catch (NumberFormatException e) {
                    return false;
                }
                break;
            default: return false;
        }
        if (t.provider != null) return t.provider.performAction(t.virtualId, a, args);
        return t.view.performAccessibilityAction(a, args);
    }
}
