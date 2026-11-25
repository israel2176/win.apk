// uia_provider.h -- the app window's views as a UI Automation provider, for screen readers.
//
// Narrator, NVDA, JAWS or any UI Automation client asks the app's window for its UIA root
// (WM_GETOBJECT with UiaRootObjectId). Its answer is a fragment root whose children are the app's
// windows (the activity's, a dialog's, a popup's, a toast's), each a tree of the elements the
// views' AccessibilityNodeInfo describe, as TalkBack reads them on a phone: uia_tree.h decides
// what each node is; this file serves it through COM.
//
// Lazy: until a client asks, uiautomationcore.dll is not loaded and the app knows of no screen
// reader. A client's first call turns the app's accessibility on (AccessibilityManager.isEnabled(),
// as TalkBack does), and the app sends a snapshot of its windows whenever its views send the
// accessibility events Android sends a screen reader (a focus moved, a text or a window changed),
// at most every 100 ms. A minute with no call from any client, while no client listens for events,
// turns it off again.
//
// Threads: UIA calls the providers on its own threads. They answer from the newest snapshot alone
// (an immutable tree behind a shared pointer: a call holds a lock only to copy the pointer), so a
// client never waits on the app's main thread or the window's. What goes to the app (a click, a
// text, a scroll) and every event raised goes through one worker thread of this file. Events are
// computed and raised only while UiaClientsAreListening.
//
// Coordinates: a node's bounds are pixels of the app's frame; Geometry turns them into the screen's
// at the moment UIA asks, so moving the window, a change of DPI or the frame's letterboxing are
// always what the screen shows.
#pragma once

#include <windows.h>
#include <unknwn.h>  // IUnknown (provider_for), which WIN32_LEAN_AND_MEAN leaves out

#include <cstdint>
#include <string>

#include "uia_tree.h"

namespace winapk::host::uia {

// The app's side (native_accessibility.cpp: HostAccessibility in Java). Called on this file's
// worker thread only.
class AppSide {
public:
    virtual ~AppSide() = default;
    // A client started reading the app (true), or none has for a while (false).
    virtual void activate(bool on) = 0;
    // Send a snapshot soon, though no view said it changed.
    virtual void refresh() = 0;
    // AccessibilityNodeInfo.performAction on node `id`: `action` is an action id (uia_tree.h's
    // action::), with ACTION_SET_TEXT's text, ACTION_SET_SELECTION's start and end (arg1, arg2) or
    // ACTION_SET_PROGRESS's value.
    virtual void perform(int64_t id, int action, const std::wstring& text, int arg1, int arg2, double value) = 0;
};

// Where a rectangle of the app's frame is on the screen, and which frame pixel a screen point is
// on (win32_window.cpp; a test gives its own). False when the window shows no frame.
struct Geometry {
    bool (*to_screen)(const Box& frame, RECT* screen) = nullptr;
    bool (*to_frame)(POINT screen, POINT* frame) = nullptr;
};

// ---- The window's thread ----------------------------------------------------------------------
void attach(HWND hwnd, const Geometry& geometry);
// WM_GETOBJECT: true, with the result, when a client asked for the UIA root.
bool on_get_object(HWND hwnd, WPARAM wp, LPARAM lp, LRESULT* result);
// WM_DESTROY: the window's providers are disconnected, and the app told no one reads it.
void detach(HWND hwnd);

// ---- Any thread -------------------------------------------------------------------------------
void set_app(AppSide* app);
// A frame was presented. While a client reads, a frame with no accessibility event since the last
// snapshot asks for one (at most twice a second): a view that changes without saying so.
void note_frame();
// A snapshot's text (uia_tree.h) from the app's main thread; it is parsed and its events raised on
// the worker. False: no client reads the app now, so the app stops sending.
bool publish(std::wstring text);
// An announcement (View.announceForAccessibility, TYPE_ANNOUNCEMENT): a UIA notification.
void announce(const std::wstring& text, bool assertive);
// Whether a client is being served.
bool active();

// ---- Tests (test/uia_test.cpp) ----------------------------------------------------------------
// The provider of one node (0: the window's root) as UIA gets it, a new reference; null when the
// newest snapshot has no such node.
IUnknown* provider_for(HWND hwnd, int64_t id);
// Waits until the worker has done everything queued before this call (a published snapshot
// parsed, the app called).
void flush();

}  // namespace winapk::host::uia
