// android.view.accessibility.AccessibilityManager -- accessibility
// service stub. We don't surface VoiceOver/screen-reader features to
// the macOS host; the service exists so getSystemService doesn't NPE
// and isEnabled-style queries return sane defaults. v1.0.98.
package android.view.accessibility;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public class AccessibilityManager {
    public AccessibilityManager() {}

    // Win.apk: a host screen reader (Windows' UI Automation, com.macapk.loader.HostAccessibility)
    // turns accessibility on while it reads the app, as TalkBack does on a device: isEnabled(),
    // the enabled service list, the state listeners. One state for the process, as the
    // service's; touch exploration stays off (a screen reader on a PC does not explore by touch).
    private static volatile boolean sHostEnabled;
    private static final java.util.concurrent.CopyOnWriteArrayList<Object[]> sStateListeners =
            new java.util.concurrent.CopyOnWriteArrayList<>();
    private static final java.util.concurrent.CopyOnWriteArrayList<java.lang.ref.WeakReference<AccessibilityManager>>
            sManagers = new java.util.concurrent.CopyOnWriteArrayList<>();

    /** HostAccessibility (main thread): a screen reader started or stopped reading the app.
     *  AOSP's setStateLocked: the listeners hear the change, each on its handler. */
    public static void macapkSetHostEnabled(boolean on) {
        if (sHostEnabled == on) return;
        sHostEnabled = on;
        for (Object[] entry : sStateListeners) {
            final AccessibilityStateChangeListener l = (AccessibilityStateChangeListener) entry[0];
            final android.os.Handler h = (android.os.Handler) entry[1];
            if (h != null) h.post(() -> l.onAccessibilityStateChanged(on));
            else l.onAccessibilityStateChanged(on);
        }
        for (java.lang.ref.WeakReference<AccessibilityManager> ref : sManagers) {
            AccessibilityManager m = ref.get();
            if (m == null) {
                sManagers.remove(ref);
                continue;
            }
            for (AccessibilityServicesStateChangeListener l : new ArrayList<>(m.mServicesStateListeners)) {
                l.onAccessibilityServicesStateChanged(m);
            }
        }
    }

    public boolean isEnabled() { return sHostEnabled; }
    public boolean isTouchExplorationEnabled() { return false; }
    public boolean isHighTextContrastEnabled() { return false; }
    public int getRecommendedTimeoutMillis(int original, int flags) { return original; }

    // AOSP: true only when the CURRENT AccessibilityNodeInfo request came from
    // a caller flagged isAccessibilityTool() (AccessibilityManager.java:1115,
    // backed by mRequestFromAccessibilityTool). No request is attributed to one
    // here; false is the honest answer.
    public boolean isRequestFromAccessibilityTool() { return false; }

    /** The host screen reader, while it reads, as the one enabled service: spoken feedback that
     *  retrieves window content, as TalkBack's info says (Compose, among others, reads this list
     *  to decide whether to build its semantics tree). */
    public List<Object> getEnabledAccessibilityServiceList(int feedbackTypeFlags) {
        if (!sHostEnabled) return Collections.emptyList();
        android.accessibilityservice.AccessibilityServiceInfo info =
                new android.accessibilityservice.AccessibilityServiceInfo();
        info.feedbackType = android.accessibilityservice.AccessibilityServiceInfo.FEEDBACK_SPOKEN;
        if ((feedbackTypeFlags & info.feedbackType) == 0) return Collections.emptyList();
        info.eventTypes = AccessibilityEvent.TYPES_ALL_MASK;
        List<Object> out = new ArrayList<>();
        out.add(info);
        return out;
    }
    public List<Object> getInstalledAccessibilityServiceList() {
        return getEnabledAccessibilityServiceList(
                android.accessibilityservice.AccessibilityServiceInfo.FEEDBACK_ALL_MASK);
    }

    public void interrupt() {}
    public void sendAccessibilityEvent(Object event) {
        if (event instanceof AccessibilityEvent) sendAccessibilityEvent((AccessibilityEvent) event);
    }

    public boolean addAccessibilityStateChangeListener(AccessibilityStateChangeListener listener) {
        return addStateListener(listener, null);
    }
    public boolean removeAccessibilityStateChangeListener(AccessibilityStateChangeListener listener) {
        for (Object[] entry : sStateListeners) {
            if (entry[0] == listener) return sStateListeners.remove(entry);
        }
        return false;
    }
    private static boolean addStateListener(AccessibilityStateChangeListener listener, android.os.Handler handler) {
        if (listener == null) return false;
        for (Object[] entry : sStateListeners) if (entry[0] == listener) return true;
        sStateListeners.add(new Object[] {listener, handler});
        return true;
    }
    public boolean addTouchExplorationStateChangeListener(TouchExplorationStateChangeListener listener) { return false; }
    public boolean removeTouchExplorationStateChangeListener(TouchExplorationStateChangeListener listener) { return false; }

    // API 34 plural-services variant (AccessibilityManager.java:348,1018-1045),
    // distinct from AccessibilityStateChangeListener above. A real registry
    // (not a bare no-op) so add/remove bookkeeping is honest; onChanged never
    // fires because no AccessibilityService is ever bound (see
    // getInstalledAccessibilityServiceList above) -- no service state ever
    // changes because none is ever enabled.
    private final ArrayList<AccessibilityServicesStateChangeListener> mServicesStateListeners =
            new ArrayList<AccessibilityServicesStateChangeListener>();
    { sManagers.add(new java.lang.ref.WeakReference<>(this)); }

    public void addAccessibilityServicesStateChangeListener(AccessibilityServicesStateChangeListener listener) {
        if (listener != null && !mServicesStateListeners.contains(listener)) mServicesStateListeners.add(listener);
    }
    public void addAccessibilityServicesStateChangeListener(java.util.concurrent.Executor executor,
            AccessibilityServicesStateChangeListener listener) {
        // Mac.apk has no executor-scoped dispatch loop to post onto (the
        // listener never fires either way); keep the registration so remove()
        // and any future real dispatch see a consistent list.
        if (listener != null && !mServicesStateListeners.contains(listener)) mServicesStateListeners.add(listener);
    }
    public boolean removeAccessibilityServicesStateChangeListener(AccessibilityServicesStateChangeListener listener) {
        return mServicesStateListeners.remove(listener);
    }

    public interface AccessibilityStateChangeListener {
        void onAccessibilityStateChanged(boolean enabled);
    }
    public interface TouchExplorationStateChangeListener {
        void onTouchExplorationStateChanged(boolean enabled);
    }
    public interface AccessibilityServicesStateChangeListener {
        void onAccessibilityServicesStateChanged(AccessibilityManager manager);
    }

    /** AOSP AccessibilityManager.sendAccessibilityEvent (API 4), correctly typed
     *  (an older loose Object overload also exists). No a11y client consumes
     *  events in Mac.apk, so the event is accepted and dropped. */
    /** AOSP: the event goes to the enabled services; here to the host screen reader. */
    public void sendAccessibilityEvent(android.view.accessibility.AccessibilityEvent event) {
        if (event == null || !sHostEnabled) return;
        StringBuilder text = new StringBuilder();
        for (CharSequence t : event.getText()) {
            if (t == null) continue;
            if (text.length() > 0) text.append(' ');
            text.append(t);
        }
        com.macapk.loader.HostAccessibility.onEvent(event.getEventType(), text, false);
    }


    private static AccessibilityManager sInstance;
    public static AccessibilityManager getInstance(android.content.Context context) {
        if (sInstance == null) sInstance = new AccessibilityManager();
        return sInstance;
    }

    // ======================================================================
    //  AOSP API-36 surface completion (Mac.apk API-gap lane, android.view).
    //
    //  Mac.apk bridges NO accessibility service to NSAccessibility, and this
    //  class already answers that honestly: isEnabled() and
    //  isTouchExplorationEnabled() are false and the service lists are empty.
    //  Every answer below is the one a device with no service connected and no
    //  accessibility setting enabled gives -- a real, common device state.
    // ======================================================================

    /** AOSP AccessibilityManager.addAccessibilityStateChangeListener(listener,
     *  Handler): the Handler overload of the method already above. AOSP posts
     *  the callback to that Handler; nothing ever changes the state here, so
     *  nothing is ever posted -- same as the no-Handler form, which this tree
     *  already answers `false` from (nothing to register with). */
    public void addAccessibilityStateChangeListener(AccessibilityStateChangeListener listener,
            android.os.Handler handler) {
        addStateListener(listener, handler);
    }

    /** AOSP AccessibilityManager.addTouchExplorationStateChangeListener(listener,
     *  Handler): same shape. */
    public void addTouchExplorationStateChangeListener(
            TouchExplorationStateChangeListener listener, android.os.Handler handler) {
    }

    /** AOSP AccessibilityManager.AudioDescriptionRequestedChangeListener
     *  (API 31): told when the user's "prefer audio descriptions" setting
     *  changes. */
    public interface AudioDescriptionRequestedChangeListener {
        void onAudioDescriptionRequestedChanged(boolean enabled);
    }
    /** AOSP AccessibilityManager.isAudioDescriptionRequested(): the user's
     *  audio-description preference. macOS exposes no such setting to us, and
     *  the platform default is off. */
    public boolean isAudioDescriptionRequested() { return false; }
    /** AOSP add/removeAudioDescriptionRequestedChangeListener. The setting never
     *  changes here, so no callback is ever delivered; remove() reports that the
     *  listener was not registered, which is AOSP's answer for an unknown
     *  listener and keeps the two consistent. */
    public void addAudioDescriptionRequestedChangeListener(
            java.util.concurrent.Executor executor,
            AudioDescriptionRequestedChangeListener listener) {
    }
    public boolean removeAudioDescriptionRequestedChangeListener(
            AudioDescriptionRequestedChangeListener listener) {
        return false;
    }

    /** AOSP AccessibilityManager.HighContrastTextStateChangeListener (API 36).*/
    public interface HighContrastTextStateChangeListener {
        void onHighContrastTextStateChanged(boolean enabled);
    }
    /** AOSP AccessibilityManager.isHighContrastTextEnabled() (API 36): the
     *  modern spelling of isHighTextContrastEnabled(), which this class already
     *  answers false. Delegating keeps the two from ever disagreeing. */
    public boolean isHighContrastTextEnabled() { return isHighTextContrastEnabled(); }
    public void addHighContrastTextStateChangeListener(java.util.concurrent.Executor executor,
            HighContrastTextStateChangeListener listener) {
    }
    public void removeHighContrastTextStateChangeListener(
            HighContrastTextStateChangeListener listener) {
    }

    /** AOSP AccessibilityManager.isAccessibilityButtonSupported(): whether the
     *  navigation bar can host the accessibility shortcut button. Mac.apk draws
     *  no navigation bar, so it cannot. */
    public static boolean isAccessibilityButtonSupported() { return false; }

    /** AOSP AccessibilityManager.getAccessibilityFocusStrokeWidth() /
     *  getAccessibilityFocusColor() (API 31): the width and colour of the
     *  focus rectangle an accessibility service draws. AOSP reads them from
     *  Settings.Secure with platform defaults of 4dp and 0xbf39b500 (the
     *  standard green); those defaults are what a device reports when the user
     *  has not customised them, which is the state here. */
    public int getAccessibilityFocusStrokeWidth() {
        // 4dp at the current display density, matching AOSP's
        // ACCESSIBILITY_FOCUS_STROKE_WIDTH_DEFAULT_VALUE (which is stored in px
        // computed from 4dp).
        try {
            float density = android.content.res.Resources.getSystem()
                    .getDisplayMetrics().density;
            return Math.round(4f * density);
        } catch (Throwable ignored) {
            return 4;
        }
    }
    public int getAccessibilityFocusColor() { return 0xbf39b500; }

    /** AOSP AccessibilityManager.getAccessibilityServiceList(), deprecated since
     *  API 16 in favour of getInstalledAccessibilityServiceList(). No
     *  accessibility service is installed, so the list is empty -- consistent
     *  with the two lists already above. */
    @Deprecated
    public List<android.content.pm.ServiceInfo> getAccessibilityServiceList() {
        return java.util.Collections.emptyList();
    }

    /** AOSP AccessibilityManager.add/removeAccessibilityRequestPreparer: register
     *  a view that needs to do work before an extra-data request is answered. No
     *  service ever makes such a request here (see the class note above), so the
     *  preparer is held and never invoked -- and add/remove still pair correctly
     *  so nothing leaks. */
    private final java.util.ArrayList<AccessibilityRequestPreparer> mRequestPreparers =
            new java.util.ArrayList<AccessibilityRequestPreparer>();
    public void addAccessibilityRequestPreparer(AccessibilityRequestPreparer preparer) {
        if (preparer != null) mRequestPreparers.add(preparer);
    }
    public void removeAccessibilityRequestPreparer(AccessibilityRequestPreparer preparer) {
        mRequestPreparers.remove(preparer);
    }
}
