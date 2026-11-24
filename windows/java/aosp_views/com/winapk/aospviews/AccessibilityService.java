// IAccessibilityManager as AccessibilityManagerService answers an app (docs/research/aosp-views.md,
// step V2): with no accessibility service on, the client's state 0 (not enabled, no touch
// exploration, no high-contrast text), no relevant event types, no services installed or enabled,
// no recommended timeout, and the focus highlight from the framework's own resources
// (accessibility_focus_highlight_stroke_width and _color, which the service reads).
//
// While a host screen reader reads the app (Windows' UI Automation: com.macapk.loader.
// HostAccessibility, which reads the views' AccessibilityNodeInfo in process: AccessibilityTree), it is the one enabled
// service, as TalkBack is on a phone: every client hears the state ACCESSIBILITY_ENABLED and every
// event type as relevant (AccessibilityManagerService.onUserStateChangedLocked ->
// scheduleUpdateClientsIfNeeded), getEnabledAccessibilityServiceList lists it (spoken feedback),
// and the events the app sends go to it (an announcement is spoken). Touch exploration stays off:
// a screen reader on a PC does not explore by touch, and the host's mouse hover stays a hover.
package com.winapk.aospviews;

import android.accessibilityservice.AccessibilityServiceInfo;
import android.os.IBinder;
import android.os.RemoteException;
import android.view.IWindow;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.IAccessibilityInteractionConnection;
import android.view.accessibility.IAccessibilityManagerClient;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

public final class AccessibilityService {
    private static final int STATE_FLAG_ACCESSIBILITY_ENABLED = 0x00000001;  // AccessibilityManager's

    private static final List<IAccessibilityManagerClient> sClients = new CopyOnWriteArrayList<>();
    private static volatile boolean sHostEnabled;

    /** HostAccessibility (through AospViewHost.accessibilityEnabled), on the main thread: a host
     *  screen reader started or stopped reading the app. Every client hears it. */
    static void setHostEnabled(boolean on) {
        if (sHostEnabled == on) return;
        sHostEnabled = on;
        for (IAccessibilityManagerClient c : sClients) {
            try {
                c.setRelevantEventTypes(relevantEventTypes());
                c.setState(state());
                c.notifyServicesStateChanged(0L);
            } catch (RemoteException | RuntimeException e) {
                sClients.remove(c);
            }
        }
    }

    private static int state() { return sHostEnabled ? STATE_FLAG_ACCESSIBILITY_ENABLED : 0; }

    private static int relevantEventTypes() { return sHostEnabled ? AccessibilityEvent.TYPES_ALL_MASK : 0; }

    /** AccessibilityManagerService.addClient: IntPair.of(state, relevant event types). */
    public long addClient(IAccessibilityManagerClient client, int userId) {
        if (client != null) sClients.add(client);
        return ((long) state() << 32) | (relevantEventTypes() & 0xffffffffL);
    }

    public boolean removeClient(IAccessibilityManagerClient client, int userId) { return sClients.remove(client); }

    /** The views' events go to the host screen reader: each says the tree changed (a snapshot
     *  follows), an announcement is spoken. */
    public void sendAccessibilityEvent(AccessibilityEvent event, int userId) {
        if (event == null || !sHostEnabled) return;
        com.macapk.loader.HostAccessibility.onEvent(event);
    }

    /** The host screen reader, while it reads, as the one service: what TalkBack's info says. */
    private static List<AccessibilityServiceInfo> hostService(int feedbackType) {
        List<AccessibilityServiceInfo> out = new ArrayList<>();
        if (!sHostEnabled || (feedbackType & AccessibilityServiceInfo.FEEDBACK_SPOKEN) == 0) return out;
        AccessibilityServiceInfo info = new AccessibilityServiceInfo();
        info.feedbackType = AccessibilityServiceInfo.FEEDBACK_SPOKEN;
        info.eventTypes = AccessibilityEvent.TYPES_ALL_MASK;
        out.add(info);
        return out;
    }

    public List<AccessibilityServiceInfo> getInstalledAccessibilityServiceList(int userId) {
        return hostService(AccessibilityServiceInfo.FEEDBACK_ALL_MASK);
    }

    public List<AccessibilityServiceInfo> getEnabledAccessibilityServiceList(int feedbackType, int userId) {
        return hostService(feedbackType);
    }

    /** ViewRootImpl registers its window's interaction connection once accessibility is on; the
     *  host reads the views in process instead, so the connection is not kept. */
    public int addAccessibilityInteractionConnection(IWindow window, IBinder leashToken,
            IAccessibilityInteractionConnection connection, String packageName, int userId) {
        return -1;  // AccessibilityWindowInfo.UNDEFINED_WINDOW_ID
    }

    public void removeAccessibilityInteractionConnection(IWindow window) {}

    public long getRecommendedTimeoutMillis() { return 0L; }

    public int getFocusStrokeWidth() {
        android.content.res.Resources r = AospWindows.resources();
        return r != null ? r.getDimensionPixelSize(com.android.internal.R.dimen.accessibility_focus_highlight_stroke_width) : 0;
    }

    public int getFocusColor() {
        android.content.res.Resources r = AospWindows.resources();
        return r != null ? r.getColor(com.android.internal.R.color.accessibility_focus_highlight_color, null) : 0;
    }
}
