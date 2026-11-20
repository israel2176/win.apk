// IAccessibilityManager as AccessibilityManagerService answers an app when no accessibility
// service is on (docs/research/aosp-views.md, step V2): the client's state 0 (not enabled, no
// touch exploration, no high-contrast text), no relevant event types, no services installed or
// enabled, no recommended timeout, and the focus highlight from the framework's own resources
// (accessibility_focus_highlight_stroke_width and _color, which the service reads). The host has
// no accessibility bridge yet (UI Automation over AccessibilityNodeInfo is later work), so
// AccessibilityManager.isEnabled() is false, as on a phone with TalkBack off.
package com.winapk.aospviews;

import android.accessibilityservice.AccessibilityServiceInfo;
import android.view.accessibility.IAccessibilityManagerClient;

import java.util.ArrayList;
import java.util.List;

public final class AccessibilityService {
    public long addClient(IAccessibilityManagerClient client, int userId) { return 0L; }

    public List<AccessibilityServiceInfo> getInstalledAccessibilityServiceList(int userId) { return new ArrayList<>(); }

    public List<AccessibilityServiceInfo> getEnabledAccessibilityServiceList(int feedbackType, int userId) {
        return new ArrayList<>();
    }

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
