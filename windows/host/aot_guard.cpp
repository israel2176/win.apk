// aot_guard.cpp -- see aot_guard.h.

#include "aot_guard.h"

#include "crash_report.h"
#include "winapk/aot_files.h"
#include "winapk/arm64_aot.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>

namespace winapk::host {

namespace fs = std::filesystem;

namespace {

constexpr int kCrashesInARow = 2;

struct State {
    uint64_t id = 0;
    bool off = false;      // the app runs on the JIT
    bool settled = false;  // it crashed on the JIT as well: the guard stands down for this build
    int crashes = 0;       // in a row, in the current mode
    bool open = false;     // a run that counts is going on, or ended without a crash
    std::string why_off;   // the crashes that turned translation off
    std::vector<std::string> recent;  // "<time>: <what>" of the crashes in a row

    bool is_default() const { return !off && !settled && crashes == 0 && !open && recent.empty(); }

    std::string text() const {
        std::ostringstream o;
        char id_hex[24];
        std::snprintf(id_hex, sizeof id_hex, "%llx", static_cast<unsigned long long>(id));
        o << "winapk-aot-guard 1\nid " << id_hex << "\nmode " << (off ? "off" : "on") << "\nsettled "
          << (settled ? 1 : 0) << "\ncrashes " << crashes << "\nopen " << (open ? 1 : 0) << "\n";
        if (!why_off.empty()) o << "why " << why_off << "\n";
        for (const std::string& r : recent) o << "crash " << r << "\n";
        return o.str();
    }

    // False when the text is not a state file of this format.
    bool parse(const std::string& s) {
        std::istringstream in(s);
        std::string line;
        if (!std::getline(in, line) || line != "winapk-aot-guard 1") return false;
        while (std::getline(in, line)) {
            const size_t sp = line.find(' ');
            const std::string key = line.substr(0, sp), value = sp == std::string::npos ? "" : line.substr(sp + 1);
            if (key == "id") id = std::strtoull(value.c_str(), nullptr, 16);
            else if (key == "mode") off = value == "off";
            else if (key == "settled") settled = value == "1";
            else if (key == "crashes") crashes = std::atoi(value.c_str());
            else if (key == "open") open = value == "1";
            else if (key == "why") why_off = value;
            else if (key == "crash") recent.push_back(value);
        }
        return true;
    }
};

// One process per app (the runtime's single-instance claim), so the state
// read at the start is the file's until this process writes it again.
SRWLOCK g_lock = SRWLOCK_INIT;
State g_state;
fs::path g_file;
std::string g_file_u8;
bool g_engaged = false;
std::atomic<bool> g_loaded{false};

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : "; ") + x;
    return s;
}

void save() {
    if (!winapk::arm64::aot_write_file(g_file, g_state.text())) {
        std::fprintf(stderr, "[win.apk] arm64 AOT: cannot write %s (error %lu)\n", g_file_u8.c_str(), GetLastError());
    }
}

void log_line(const std::string& s) {
    std::fprintf(stderr, "%s\n", s.c_str());
    std::fflush(stderr);
}

// The first translation of this process is loaded: from now on a crash counts.
void on_loaded() {
    g_loaded.store(true);
    AcquireSRWLockExclusive(&g_lock);
    if (!g_state.off && !g_state.settled && !g_state.open) {
        g_state.open = true;
        save();
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// The process is ending through the crash path (crash_report.h). The
// faulting thread may hold the lock (a crash inside save()): then nothing is
// counted rather than the handler waiting for ever.
void on_crash(const char* what) {
    if (!g_engaged) return;
    bool locked = false;
    for (int i = 0; i < 50 && !(locked = TryAcquireSRWLockExclusive(&g_lock)); ++i) Sleep(10);
    if (!locked) return;
    char when[32] = "?";
    const std::time_t now = std::time(nullptr);
    if (const std::tm* t = std::localtime(&now)) std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", t);
    std::string entry = std::string(when) + ": " + what;
    std::replace(entry.begin(), entry.end(), '\n', ' ');
    std::replace(entry.begin(), entry.end(), '\r', ' ');
    if (entry.size() > 300) entry.resize(300);
    State& s = g_state;
    if (s.off) {
        s.open = false;
        s.recent.push_back(entry);
        ++s.crashes;
        save();
        if (s.crashes >= kCrashesInARow) {
            log_line("[win.apk] arm64 AOT: the app crashes on the JIT too: not the translation (" +
                     std::to_string(s.crashes) +
                     " starts in a row with translation off); the next start uses the translation again");
        } else {
            log_line("[win.apk] arm64 AOT: crash counted: " + std::to_string(s.crashes) + " of " +
                     std::to_string(kCrashesInARow) +
                     " starts in a row on the JIT (translation is off for this app); if the next one crashes too, "
                     "it was not the translation (" + g_file_u8 + ")");
        }
    } else if (!g_loaded.load()) {
        log_line("[win.apk] arm64 AOT: crash not counted against the translation: no translated code was loaded in "
                 "this run");
    } else if (s.settled) {
        log_line("[win.apk] arm64 AOT: crash not counted against the translation: this build of the app crashed on "
                 "the JIT as well (" + g_file_u8 + ")");
    } else {
        s.open = false;
        s.recent.push_back(entry);
        ++s.crashes;
        save();
        if (s.crashes >= kCrashesInARow) {
            log_line("[win.apk] arm64 AOT: crash counted: " + std::to_string(s.crashes) +
                     " starts in a row crashed with translated code loaded; the next start runs the app on the JIT (" +
                     g_file_u8 + ")");
        } else {
            log_line("[win.apk] arm64 AOT: crash counted: " + std::to_string(s.crashes) + " of " +
                     std::to_string(kCrashesInARow) +
                     " starts in a row with translated code loaded; one more turns translation off for this app (" +
                     g_file_u8 + ")");
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace

fs::path aot_guard_file(const fs::path& app_dir) { return app_dir / "aot-guard.txt"; }

uint64_t aot_guard_id(const fs::path& apk, const fs::path& libs) {
    uint64_t h = winapk::arm64::kAotVersion;
    auto add = [&h](uint64_t v) { h = winapk::arm64::aot_hash(reinterpret_cast<const uint8_t*>(&v), sizeof v, h); };
    add(winapk::arm64::aot_lib_id(apk));
    std::vector<fs::path> sos;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(libs, ec)) {
        if (e.path().extension() == L".so") sos.push_back(e.path());
    }
    std::sort(sos.begin(), sos.end());
    for (const fs::path& so : sos) add(winapk::arm64::aot_lib_id(so));
    return h ? h : 1;
}

bool aot_guard_start(const fs::path& app_dir, uint64_t id) {
    g_file = aot_guard_file(app_dir);
    g_file_u8 = g_file.u8string();
    const std::string old = winapk::arm64::aot_read_file(g_file);
    State s;
    if (!old.empty() && (!s.parse(old) || s.id != id)) {
        if (s.crashes > 0 || s.off || s.settled) {
            log_line("[win.apk] arm64 AOT: the app or the translator changed since the crashes counted in " +
                     g_file_u8 + ": the count starts over");
        }
        s = State{};
    }
    s.id = id;
    if (s.open) {
        // The last run that counted ended without a crash (closed, or ended
        // by a kill): the crashes before it are no longer in a row.
        s.open = false;
        s.crashes = 0;
        s.recent.clear();
    }
    if (s.off && s.crashes >= kCrashesInARow) {
        log_line("[win.apk] arm64 AOT: on again for this app: it crashed on the JIT too, " + std::to_string(s.crashes) +
                 " starts in a row (" + joined(s.recent) +
                 "): not the translation. The translation is used from now on, until the app or the translator "
                 "changes");
        s.off = false;
        s.settled = true;
        s.crashes = 0;
        s.recent.clear();
        s.why_off.clear();
    } else if (!s.off && !s.settled && s.crashes >= kCrashesInARow) {
        s.off = true;
        s.why_off = joined(s.recent);
        s.crashes = 0;
        s.recent.clear();
    }
    if (s.off) {
        winapk::arm64::aot_disable();
        log_line("[win.apk] arm64 AOT: off for this app: it crashed " + std::to_string(kCrashesInARow) +
                 " starts in a row with translated code loaded (" + s.why_off + "); the JIT runs it. Delete " +
                 g_file_u8 + " to try the translation again.");
        s.open = true;  // this run counts, on the JIT
    }
    AcquireSRWLockExclusive(&g_lock);
    g_state = s;
    const std::string text = s.text();
    // An app that never crashed gets the file once a translation loads.
    if (text != old && (!old.empty() || !s.is_default())) save();
    g_engaged = true;
    ReleaseSRWLockExclusive(&g_lock);
    winapk::arm64::aot_set_loaded_hook(on_loaded);
    set_crash_hook(on_crash);
    return !s.off;
}

}  // namespace winapk::host
