// maintenance.h -- when the launcher opens a maintenance window ("Update apps in the background").
//
// A maintenance window is `winapk-runtime --system --maintenance`: the system broker boots Google services,
// runs the apps' jobs that are due (the Play Store's update check and installs among them) and stops a few
// minutes after the last. The owner's rule: it runs only while Win.apk itself runs, never from a Windows
// scheduled task or a service. So the launcher, shown or hidden, asks at its start and every kCheckMinutes
// whether one is due, and opens it when it is; the launcher's exit rule (maybe_quit) is not changed for it,
// but a window it opened counts as something left, so the launcher waits for its end.
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
};

enum class Decision {
    kStart,      // open a window now, and note the time
    kOff,        // the setting is off, or the setup is not done: nothing to do
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
    if (c.window_running || c.broker_running) return Decision::kRunning;
    if (!due(now, last)) return Decision::kNotDue;
    if (!c.on_mains || !c.on_network || c.app_starting) return Decision::kWait;
    return Decision::kStart;
}

}  // namespace winapk::launcher::maintenance
