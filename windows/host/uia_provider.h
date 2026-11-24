// uia_provider.h -- the app window's views for Windows' UI Automation (screen readers).
//
// Adapted from the other runtime (https://example.invalid/other-runtime), compat/accessibility/uia_bridge.cc,
// Copyright (C) 2026 its authors, SPDX-License-Identifier: Apache-2.0. Modified for Win.apk.
//
// A screen reader (Narrator, NVDA, JAWS) or any UI Automation client reads an Android app's
// views as UIA elements, the way TalkBack reads them on a phone: from AccessibilityNodeInfo,
// which every view (Win.apk's own, AOSP's under WINAPK_AOSP_VIEWS=1, an app's own and a virtual
// hierarchy behind an AccessibilityNodeProvider such as Compose's) describes itself with. The
// Java side (runtime/java/com/macapk/loader/HostAccessibility.java) walks the window's view tree
// on the app's main thread and publishes it here as a snapshot; the providers answer UIA from
// that snapshot alone, so UIA's threads never wait on Java, and a call into the app (a click, a
// text, a scroll) is handed to one worker thread that posts it to the main thread.
//
// Lazy: nothing of this runs, and uiautomationcore.dll is not even loaded, until a client asks
// the window for its UIA root (WM_GETOBJECT, UiaRootObjectId). Then the app is told an
// accessibility service is on (AccessibilityManager.isEnabled(), as on a phone while TalkBack
// runs) and publishes while frames change. A minute with no call from any client, while no
// client listens for events, turns it off again.
//
// Snapshot text, one line per record, fields split by tabs (strings never hold a tab or a line
// break: the Java side turns them into spaces):
//   W1 <focused id> <locale BCP 47> <window roots, bottom to top, comma-separated>
//   <id> <parent, 0 for a window's root> <class> <name> <value> <help> <flags> <l,t,r,b>
//        <children, comma-separated> <view id resource name> <can> <min,max,now> <live region>
// Bounds are pixels of the app's frame (its surface), origin at its top left.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace winapk::host::uia {

// A node's state (HostAccessibility.java writes the same bits).
enum Flag : uint32_t {
    kEnabled = 1, kFocusable = 2, kFocused = 4, kClickable = 8, kCheckable = 16, kChecked = 32,
    kEditable = 64, kPassword = 128, kVisible = 256, kScrollable = 512, kHeading = 1024, kSelected = 2048,
    kLongClickable = 4096, kRange = 8192, kCollection = 16384, kCollectionItem = 32768, kMultiLine = 65536,
};

// What a node's AccessibilityActions let UIA's patterns do.
enum Can : uint32_t {
    kCanScrollForward = 1, kCanScrollBackward = 2, kCanScrollUp = 4, kCanScrollDown = 8, kCanScrollLeft = 16,
    kCanScrollRight = 32, kCanSetProgress = 64, kCanSetText = 128, kCanShowOnScreen = 256, kCanClick = 512,
};

// What the provider asks the app to do (HostAccessibility.perform's action).
enum Action : int {
    kActClick = 1, kActFocus = 2, kActSetText = 3, kActShowOnScreen = 5, kActScrollForward = 6,
    kActScrollBackward = 7, kActScrollUp = 8, kActScrollDown = 9, kActScrollLeft = 10, kActScrollRight = 11,
    kActSetProgress = 12,
};

struct Node {
    int64_t id = 0, parent = 0;
    std::wstring class_name, name, value, help, automation_id;
    uint32_t flags = 0, can = 0;
    RECT bounds{};  // pixels of the app's frame
    std::vector<int64_t> children;
    double range_min = 0, range_max = 0, range_now = 0;
    int live = 0;  // View.ACCESSIBILITY_LIVE_REGION_*: 0 none, 1 polite, 2 assertive
    int control_type = 0;
};

struct Snapshot {
    uint64_t generation = 0;
    int64_t focused = 0;
    std::wstring locale;
    LCID lcid = 0;
    std::vector<int64_t> top;  // the windows' roots, bottom to top
    std::vector<Node> nodes;
    std::unordered_map<int64_t, size_t> index;

    const Node* find(int64_t id) const {
        auto it = index.find(id);
        return it == index.end() ? nullptr : &nodes[it->second];
    }
};

// Parses a snapshot's text; false when it is not one. Pure.
bool parse_snapshot(const std::wstring& text, Snapshot* out);
// The UIA control type for a node: its class first, then what it can do. Pure.
int control_type(const Node& node);

// The app's side (native_accessibility.cpp: HostAccessibility in Java). Called on the provider's
// worker thread, never on a UIA thread or the window's.
class AppSide {
public:
    virtual ~AppSide() = default;
    // A client started asking (true), or none has asked for a while (false).
    virtual void activate(bool on) = 0;
    // Publish a snapshot soon, whatever the frames did.
    virtual void refresh() = 0;
    virtual void perform(int64_t id, int action, const std::wstring& text) = 0;
};

// Where a rect of the app's frame is on the screen, and which frame pixel a screen point is on
// (win32_window.cpp; a test gives its own). False when the window shows no frame.
struct Geometry {
    bool (*to_screen)(const RECT& frame, RECT* screen) = nullptr;
    bool (*to_frame)(POINT screen, POINT* frame) = nullptr;
};

// ---- The window's thread --------------------------------------------------------------------
void attach(HWND hwnd, const Geometry& geometry);
// WM_GETOBJECT: true, with the result, when it asked for the UIA root.
bool on_get_object(HWND hwnd, WPARAM wp, LPARAM lp, LRESULT* result);
// WM_DESTROY: every element of the window is gone.
void detach(HWND hwnd);

// ---- Any thread -----------------------------------------------------------------------------
// A frame was presented: what is on the screen may have changed.
void note_frame();
void set_app(AppSide* app);
// Whether a client is being served.
bool active();
// Whether the app should publish now: serving, and a frame came or a refresh was asked for.
bool due();
// A new snapshot from the app's main thread; the events it makes (focus, structure, values,
// live regions) go out on the worker. False: no client has asked for a while, stop publishing.
bool publish(const std::wstring& text);
// An announcement (View.announceForAccessibility): a UIA notification.
void announce(const std::wstring& text, bool assertive);

// The provider of one node (0: the window's root), as UIA gets it: for test/uia_test.cpp, which
// checks the patterns themselves where a client (Wine's) lacks them. A new reference.
IUnknown* provider_for(HWND hwnd, int64_t id);

}  // namespace winapk::host::uia
