package com.winapk.aospviews;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.os.SystemClock;
import android.view.Display;
import android.view.View;
import android.view.ViewGroup;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.autofill.AutofillId;
import android.view.contentcapture.ContentCaptureSession;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodSubtype;
import android.window.BackEvent;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.WeakHashMap;

/**
 * Android 16's (API 36) members of the classes lib/aosp-views.jar takes from AOSP 14, which an app
 * targeting 36 is told it runs on (PresentedSdk) and so calls: androidx.core reads
 * AccessibilityNodeInfo.getChecked behind SDK_INT >= 36, androidx.activity reads
 * BackEvent.getFrameTimeMillis from every back event (docs/research/api-gaps.md). build_aosp_views.sh
 * adds each public static method here as an instance method of its first parameter's class
 * (windows/scripts/AospAddMethods.java), as it does Api35's.
 *
 * As in Api35: a value the object keeps is AOSP 16's (its setter, its getter, 16's default), kept
 * beside the object, weakly; a copy of the object (AccessibilityNodeInfo's obtain(info), a parcel)
 * does not carry it. Where 16 acts on a value, AOSP 14's behaviour stands and the value is a hint this
 * runtime has no part for (a display's HDR ratio and adaptive refresh, the writing tools).
 * Left out, as LinkScan lists them: members of interfaces (WindowManager, AttachedSurfaceControl),
 * members that name a type AOSP 14 has no class of (ComponentCaller, InputTransferToken, credentials,
 * AppJankStats, HighContrastTextStateChangeListener), static fields (RangeInfo.INDETERMINATE) and
 * constructors (BackEvent's five-argument one).
 */
public final class Api36 {
    private Api36() {}

    private static final Map<Object, Object[]> sState = Collections.synchronizedMap(new WeakHashMap<>());

    private static Object get(Object o, int slot, Object dflt) {
        Object[] s = sState.get(o);
        return s == null || s[slot] == null ? dflt : s[slot];
    }

    private static void put(Object o, int slot, Object v) {
        synchronized (sState) {
            Object[] s = sState.get(o);
            if (s == null) sState.put(o, s = new Object[8]);
            s[slot] = v;
        }
    }

    // ---- AccessibilityNodeInfo ----

    /** AccessibilityNodeInfo.CHECKED_STATE_* (36). */
    private static final int CHECKED_FALSE = 0, CHECKED_TRUE = 1, CHECKED_PARTIAL = 2;

    /** 16's tri-state checked: setChecked(boolean) of 14 is TRUE or FALSE, PARTIAL is kept beside. */
    public static void setChecked(AccessibilityNodeInfo n, int state) {
        n.setChecked(state == CHECKED_TRUE);
        put(n, 0, state == CHECKED_PARTIAL);
    }

    public static int getChecked(AccessibilityNodeInfo n) {
        if (n.isChecked()) return CHECKED_TRUE;
        return Boolean.TRUE.equals(get(n, 0, false)) ? CHECKED_PARTIAL : CHECKED_FALSE;
    }

    /** AccessibilityNodeInfo.EXPANDED_STATE_UNDEFINED (0) until set. */
    public static void setExpandedState(AccessibilityNodeInfo n, int state) { put(n, 1, state); }
    public static int getExpandedState(AccessibilityNodeInfo n) { return (Integer) get(n, 1, 0); }

    public static void setFieldRequired(AccessibilityNodeInfo n, boolean required) { put(n, 2, required); }
    public static boolean isFieldRequired(AccessibilityNodeInfo n) { return (Boolean) get(n, 2, false); }

    public static void setSupplementalDescription(AccessibilityNodeInfo n, CharSequence d) { put(n, 3, d); }
    public static CharSequence getSupplementalDescription(AccessibilityNodeInfo n) {
        return (CharSequence) get(n, 3, null);
    }

    /** 16's list of labels: 14's single setLabeledBy is the first, the rest are kept beside. As AOSP
     *  does, a label is held as its node's id, so the list is of the nodes AccessibilityNodeInfo
     *  finds for them. */
    public static void addLabeledBy(AccessibilityNodeInfo n, View label) { addLabeled(n, label, View.NO_ID); }
    public static void addLabeledBy(AccessibilityNodeInfo n, View root, int virtualDescendantId) {
        addLabeled(n, root, virtualDescendantId);
    }

    public static boolean removeLabeledBy(AccessibilityNodeInfo n, View label) {
        return removeLabeled(n, label, View.NO_ID);
    }
    public static boolean removeLabeledBy(AccessibilityNodeInfo n, View root, int virtualDescendantId) {
        return removeLabeled(n, root, virtualDescendantId);
    }

    public static List<AccessibilityNodeInfo> getLabeledByList(AccessibilityNodeInfo n) {
        List<AccessibilityNodeInfo> out = new ArrayList<>();
        AccessibilityNodeInfo first = n.getLabeledBy();
        if (first != null) out.add(first);
        @SuppressWarnings("unchecked")
        List<Object[]> more = (List<Object[]>) get(n, 4, null);
        if (more != null) {
            for (Object[] l : more) {
                View v = (View) l[0];
                int id = (Integer) l[1];
                AccessibilityNodeInfo info = id == View.NO_ID ? v.createAccessibilityNodeInfo()
                        : v.getAccessibilityNodeProvider() != null
                                ? v.getAccessibilityNodeProvider().createAccessibilityNodeInfo(id) : null;
                if (info != null) out.add(info);
            }
        }
        return out;
    }

    private static void addLabeled(AccessibilityNodeInfo n, View v, int id) {
        if (n.getLabeledBy() == null) {
            if (id == View.NO_ID) n.setLabeledBy(v);
            else n.setLabeledBy(v, id);
            return;
        }
        synchronized (sState) {
            @SuppressWarnings("unchecked")
            List<Object[]> more = (List<Object[]>) get(n, 4, null);
            if (more == null) put(n, 4, more = new ArrayList<>());
            for (Object[] l : more) if (l[0] == v && (Integer) l[1] == id) return;
            more.add(new Object[] {v, id});
        }
    }

    private static boolean removeLabeled(AccessibilityNodeInfo n, View v, int id) {
        synchronized (sState) {
            @SuppressWarnings("unchecked")
            List<Object[]> more = (List<Object[]>) get(n, 4, null);
            if (more != null && more.removeIf(l -> l[0] == v && (Integer) l[1] == id)) return true;
        }
        // The first label is 14's own field: AccessibilityNodeInfo cannot clear it but by a null view,
        // which setLabeledBy(View) does.
        if (n.getLabeledBy() != null && id == View.NO_ID) {
            n.setLabeledBy((View) null);
            return true;
        }
        return false;
    }

    // ---- View, ViewGroup ----

    public static void setSupplementalDescription(View v, CharSequence d) { put(v, 0, d); }
    public static CharSequence getSupplementalDescription(View v) { return (CharSequence) get(v, 0, null); }

    /** 16's vote propagation for the frame rate: one fixed-rate display here (Api35's frame rates). */
    public static void propagateRequestedFrameRate(ViewGroup g, float frameRate, boolean forceOverride) {}

    // ---- Display ----

    /** No HDR output is reported (Display.getHdrCapabilities is empty): an SDR display's 1. */
    public static float getHighestHdrSdrRatio(Display d) { return 1f; }

    /** 16's suggested rate for a frame-rate category: the display has one rate. */
    public static float getSuggestedFrameRate(Display d, int category) { return d.getRefreshRate(); }

    /** No adaptive refresh rate (ARR). */
    public static boolean hasArrSupport(Display d) { return false; }

    // ---- AccessibilityManager ----

    /** The high-contrast text setting, off (Windows' high contrast is a theme, not this text mode). */
    public static boolean isHighContrastTextEnabled(AccessibilityManager m) { return false; }

    // ---- AutofillId ----

    public static boolean isVirtual(AutofillId id) { return id.isVirtualInt() || id.isVirtualLong(); }

    /** 16: the virtual child's int id, View.NO_ID for one that is not virtual by int. */
    public static int getAutofillVirtualId(AutofillId id) {
        return id.isVirtualInt() ? id.getVirtualChildIntId() : View.NO_ID;
    }

    /** No autofill service, so no autofill session. */
    public static boolean isInAutofillSession(AutofillId id) { return false; }

    // ---- ContentCaptureSession ----

    /** No content capture service: nothing buffered to flush. */
    public static void flush(ContentCaptureSession s) {}

    // ---- EditorInfo ----

    public static void setAutofillId(EditorInfo e, AutofillId id) { put(e, 0, id); }
    public static AutofillId getAutofillId(EditorInfo e) { return (AutofillId) get(e, 0, null); }

    /** 16's default is enabled; no writing tools IME here acts on it. */
    public static void setWritingToolsEnabled(EditorInfo e, boolean enabled) { put(e, 1, enabled); }
    public static boolean isWritingToolsEnabled(EditorInfo e) { return (Boolean) get(e, 1, true); }

    // ---- InputMethodInfo, InputMethodSubtype ----

    /** The IME's language settings: Windows' IME declares none. */
    public static android.content.Intent createImeLanguageSettingsActivityIntent(InputMethodInfo i) { return null; }

    public static int getLayoutLabelResource(InputMethodSubtype s) { return 0; }
    public static CharSequence getLayoutLabelNonLocalized(InputMethodSubtype s) { return ""; }
    public static CharSequence getLayoutDisplayName(InputMethodSubtype s, Context c, ApplicationInfo info) {
        return "";
    }

    public static InputMethodSubtype.InputMethodSubtypeBuilder setLayoutLabelResource(
            InputMethodSubtype.InputMethodSubtypeBuilder b, int res) {
        return b;
    }

    public static InputMethodSubtype.InputMethodSubtypeBuilder setLayoutLabelNonLocalized(
            InputMethodSubtype.InputMethodSubtypeBuilder b, CharSequence label) {
        return b;
    }

    // ---- BackEvent ----

    /** 16: the frame time of the event, which 16's four-argument constructor sets to
     *  SystemClock.uptimeMillis() when it is made; AOSP 14's has no field for it, so it is the time
     *  this event is first asked, kept for it (an event is read as soon as it is dispatched). */
    public static long getFrameTimeMillis(BackEvent e) {
        synchronized (sState) {
            Object t = get(e, 0, null);
            if (t == null) put(e, 0, t = SystemClock.uptimeMillis());
            return (Long) t;
        }
    }
}
