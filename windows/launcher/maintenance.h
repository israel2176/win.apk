// maintenance.h -- when the launcher opens a maintenance window ("Update apps in the background").
//
// A maintenance window is `winapk-runtime --system --maintenance`: the system broker boots Google services,
// runs the apps' jobs that are due (the Play Store's update check and installs among them) and stops a few
// minutes after the last. The rule: it runs only while Win.apk itself runs, never from a Windows
// scheduled task or a service. So the launcher, shown or hidden, asks at its start and every kCheckMinutes
// whether one is due, and opens it when it is. The launcher is not kept alive for it: a window it opened keeps
// a launcher that was never shown (started for an app's shortcut or an install) until the window ends, but
// once the user closes the library, closing Win.apk is how they stop it all (nothing of Win.apk goes on with
// nothing on screen): no window is opened from then on, and when no app window is left on screen the launcher
// quits without waiting, ending the window's broker and the apps it started (end_background_work). Showing
// the library again lifts that.
//
// The decision is here, with no Windows call, so that test/launcher_state_test.cpp drives it; the launcher
// gathers the conditions (the settings, GetSystemPowerStatus, Network List Manager, the broker's mutex) and
// keeps the last window's start in the registry (`LastMaintenance`, Unix seconds).

#pragma once

#include <cstdint>

namespace winapk::launcher::maintenance {

constexpr int64_t kIntervalSeconds = 4 * 3600;  // as the scheduled task's trigger was
constexpr unsigned kCheckMinutes = 15;          // how often the launcher asks

struct Conditions {
    bool setting_on = false;      // "Use Google services" and "Update apps in the background" both on
    bool apps_ready = false;      // Play services, Services Framework and Play Store installed
    bool on_mains = false;        // not on battery (a PC with no battery is on mains)
    bool on_network = false;      // connected to some network, as the task's RunOnlyIfNetworkAvailable
    bool window_running = false;  // a maintenance window this launcher opened has not ended
    bool broker_running = false;  // a system broker runs (an app's, or another launcher's window)
    bool app_starting = false;    // an app's start is under way: wait, rather than start two JVMs at once
    bool closed_by_user = false;  // the user closed the library and has not shown it again: Win.apk is ending
};

enum class Decision {
    kStart,      // open a window now, and note the time
    kOff,        // the setting is off, or the setup is not done: nothing to do
    kClosed,     // the user closed the library: Win.apk is on its way out (an open app's broker runs its jobs)
    kRunning,    // a window or a broker runs already (the broker runs the jobs that are due itself)
    kNotDue,     // the last window started less than kIntervalSeconds ago
    kWait,       // due, but on battery, with no network, or while an app starts: ask again later
};

// Whether a window is due: none was noted, the last one is kIntervalSeconds old, or the clock went back
// past it (a noted time in the future would otherwise hold windows off until then).
inline bool due(int64_t now, int64_t last) {
    return last <= 0 || now < last || now - last >= kIntervalSeconds;
}

inline Decision decide(int64_t now, int64_t last, const Conditions& c) {
    if (!c.setting_on || !c.apps_ready) return Decision::kOff;
    if (c.closed_by_user) return Decision::kClosed;
    if (c.window_running || c.broker_running) return Decision::kRunning;
    if (!due(now, last)) return Decision::kNotDue;
    if (!c.on_mains || !c.on_network || c.app_starting) return Decision::kWait;
    return Decision::kStart;
}

// When the hidden launcher has nothing left but a maintenance window it opened: whether it waits for the
// window's end (kMaintenanceEnded) or quits now, which ends the window's broker and what it started. It waits
// only when the user did not close the library: a launcher never shown (an app's shortcut, an install from
// Explorer) lets a window it already opened finish, a few minutes after its last job; a library the user closed
// is Win.apk closed, and a Play Store install still under way is cut off as a phone's power-off cuts it.
inline bool wait_for_window(bool window_running, bool closed_by_user) { return window_running && !closed_by_user; }

}  // namespace winapk::launcher::maintenance
