// installs.h -- the installs the library has queued, one state machine per install.
//
// An install is touched by two threads: the library's (install(), Cancel, kInstallStarted and
// kInstalled) and its own (install_thread: it waits for its turn, copies the file in, runs the
// runtime that converts the app, then keeps or undoes what it did). InstallTable is the one record
// both go by. It holds no lock of its own and makes no Windows call, so that
// test/launcher_state_test.cpp can drive it; launcher_main.cpp calls it only with g_installs_lock held
// (shared for the const members, exclusive for the rest). One install runs at a time: a thread holds
// g_install_lock from start() to ended().
//
//   state        what it means                                             the process
//   (none)       not queued, or over                                       -
//   kQueued      its thread waits for its turn                             none
//   kInstalling  its turn: the file is copied in, the runtime converts it  none, then held, then none
//   kCommitted   the runtime succeeded and the install is being kept       none
//   kEnded       its thread is done; the library has yet to hear           none
//
//   from                      to           by                when
//   (none)                    kQueued      library           begin(): install()
//   kQueued                   kInstalling  install's thread  start(): its turn, and not cancelled
//   kQueued                   kEnded       install's thread  start(): its turn, but cancelled: never started
//   kInstalling               kCommitted   install's thread  commit(): the runtime exited with 0, not cancelled
//   kInstalling, kCommitted   kEnded       install's thread  ended(): kept, or undone
//   kEnded                    (none)       library           finish(): kInstalled
//
// Beside the state, `cancelled`: cancel() sets it while the install can still be undone (kQueued,
// kInstalling) and ends the runtime when there is one; attach() tells a runtime started after the
// cancel to be ended at once. From commit() on a cancel is refused, so an install is never both kept
// and reported as cancelled, and a cancel that comes after the end leaves nothing behind for the next
// install of the same name.
//
// An install that is in no table (the install helper's, in a process of its own) cannot be cancelled:
// attach() and commit() say so.

#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace winapk::launcher {

enum class InstallState { kQueued, kInstalling, kCommitted, kEnded };

// How an install ended, for the library to tell the user.
struct InstallEnd {
    bool ok = false;           // the app is in the library
    bool app_running = false;  // not installed because the app runs (or was started meanwhile)
    bool cancelled = false;    // the user cancelled it before it was kept
    enum Kind { kDone, kFailed, kAppRunning, kCancelled };
    Kind kind() const {
        if (cancelled) return kCancelled;
        if (ok) return kDone;
        return app_running ? kAppRunning : kFailed;
    }
};

class InstallTable {
public:
    using Process = void*;  // a HANDLE; the table never looks inside

    // The library queues an install. False, changing nothing, when one of that name is queued, under
    // way, or over without the library having heard (the same file twice).
    bool begin(const std::wstring& name) {
        if (find(name)) return false;
        Install i;
        i.order = next_order_++;
        installs_.emplace(name, i);
        return true;
    }

    // The install's thread, its turn come. False when it was cancelled while it waited: it is over
    // (ended, cancelled) without having started, and the thread only reports it.
    bool start(const std::wstring& name) {
        Install* i = find(name);
        if (!i || i->state != InstallState::kQueued) return false;
        if (i->cancelled) {
            i->state = InstallState::kEnded;
            return false;
        }
        i->state = InstallState::kInstalling;
        return true;
    }

    // The install's thread: the runtime that converts the app started. True when the install was
    // cancelled meanwhile: the caller ends the process at once.
    bool attach(const std::wstring& name, Process process) {
        Install* i = find(name);
        if (!i || i->state != InstallState::kInstalling) return false;
        i->process = process;
        return i->cancelled;
    }

    // The install's thread: the runtime exited. Forgotten here, under the caller's lock, so that
    // cancel() cannot end a handle about to be closed.
    void detach(const std::wstring& name) {
        if (Install* i = find(name)) i->process = nullptr;
    }

    // The install's thread, the runtime having succeeded: may the install be kept? False when it was
    // cancelled: the caller undoes it. True otherwise, and from now on a cancel is refused.
    bool commit(const std::wstring& name) {
        Install* i = find(name);
        if (!i) return true;  // in no table: nobody can have cancelled it
        if (i->state != InstallState::kInstalling || i->cancelled) return false;
        i->state = InstallState::kCommitted;
        return true;
    }

    // The install's thread: it is over. `ok`: the app is in the library; `app_running`: it was left
    // alone because the app runs.
    bool ended(const std::wstring& name, bool ok, bool app_running) {
        Install* i = find(name);
        if (!i || i->state == InstallState::kEnded) return false;
        // Kept only through commit(): an install that was cancelled, or never committed, is not ok.
        i->ok = ok && i->state == InstallState::kCommitted;
        i->app_running = app_running;
        i->state = InstallState::kEnded;
        i->process = nullptr;
        return true;
    }

    // The library: cancel the install. `kill(process)` ends its runtime. True when it was cancelled:
    // one still waiting does not start, one under way is stopped and undone. False (nothing changed)
    // when there is none, it is being kept already, or it is over.
    template <class Kill>
    bool cancel(const std::wstring& name, Kill&& kill) {
        Install* i = find(name);
        if (!i || (i->state != InstallState::kQueued && i->state != InstallState::kInstalling)) return false;
        i->cancelled = true;
        if (i->process) kill(i->process);
        return true;
    }

    // The library, on kInstalled: the install is over and forgotten. Nothing when it is not at its end.
    std::optional<InstallEnd> finish(const std::wstring& name) {
        const auto it = installs_.find(name);
        if (it == installs_.end() || it->second.state != InstallState::kEnded) return std::nullopt;
        const InstallEnd end{it->second.ok, it->second.app_running, it->second.cancelled};
        installs_.erase(it);
        return end;
    }

    // An install is pending from begin() until the library has heard of its end.
    bool pending(const std::wstring& name) const { return find(name) != nullptr; }
    bool empty() const { return installs_.empty(); }
    std::optional<InstallState> state(const std::wstring& name) const {
        const Install* i = find(name);
        return i ? std::optional<InstallState>(i->state) : std::nullopt;
    }
    // The pending installs, in the order they were queued.
    std::vector<std::wstring> names() const {
        std::vector<std::pair<unsigned long long, std::wstring>> by_order;
        for (const auto& i : installs_) by_order.emplace_back(i.second.order, i.first);
        std::sort(by_order.begin(), by_order.end());
        std::vector<std::wstring> out;
        for (auto& i : by_order) out.push_back(std::move(i.second));
        return out;
    }
    // The install whose turn it is, empty when none is under way.
    std::wstring current() const {
        for (const auto& i : installs_) {
            if (i.second.state == InstallState::kInstalling || i.second.state == InstallState::kCommitted) return i.first;
        }
        return {};
    }

private:
    struct Install {
        InstallState state = InstallState::kQueued;
        Process process = nullptr;
        unsigned long long order = 0;
        bool cancelled = false, ok = false, app_running = false;
    };
    const Install* find(const std::wstring& name) const {
        const auto it = installs_.find(name);
        return it == installs_.end() ? nullptr : &it->second;
    }
    Install* find(const std::wstring& name) { return const_cast<Install*>(std::as_const(*this).find(name)); }

    std::map<std::wstring, Install> installs_;
    unsigned long long next_order_ = 0;
};

// ---- a batch's APK files as install sessions ----------------------------------------------------------------
//
// Several .apk files opened at once (dropped together, or chosen together in Add) are installed as a device's
// installer installs the files `adb install-multiple` gives it: those of one package are one session, the base
// and its splits (a bundle a store's site handed out as loose files, the base and config.arm64_v8a.apk). Each
// file's package and split name come from the runtime (com.winapk.tools.AppInstall --set: a line per file,
// "<package>\t<split name>\t<file>"); which files make which session is decided here, with no Windows call.
// Whether a session is an app (one base, the splits of its version, the splits it requires) is AppInstall's
// question at the install, as PackageInstallerSession's is: a set with no base is still one session, refused
// there with Android's "Full install must include a base package".

struct ApkPart {
    std::wstring file;
    std::wstring package;  // empty: the file has no manifest that could be read
    std::wstring split;    // empty: a base
};

// One install: the file the library keeps as <name>.apk, and the others as <name>.splits\<file name>.
struct InstallSet {
    std::wstring base;
    std::vector<std::pair<std::wstring, std::wstring>> splits;  // the file, its name in <name>.splits
};

// AppInstall --set's lines; a line that is not three fields is skipped.
inline std::vector<ApkPart> parse_apk_set(const std::wstring& text) {
    std::vector<ApkPart> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(L'\n', at);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        const size_t t1 = line.find(L'\t'), t2 = t1 == std::wstring::npos ? t1 : line.find(L'\t', t1 + 1);
        if (t2 == std::wstring::npos || t2 + 1 >= line.size()) continue;
        out.push_back({line.substr(t2 + 1), line.substr(0, t1), line.substr(t1 + 1, t2 - t1 - 1)});
    }
    return out;
}

// The batch's sessions, in the order each package first comes: a file whose package is unknown alone; the files
// of one package together, the base (the first file with no split name, else the first file) as the one the
// library keeps as the app's .apk. A split is kept as split_<split name>.apk, as a device names it, when that
// name is a safe file name (FileUtils.isFilenameSafe: letters, digits, + , - . = _), else split_<n>.apk; a name
// taken already gets .<n> before .apk (AppInstall reads each split's name from its manifest, not its file).
inline std::vector<InstallSet> group_install_sets(const std::vector<ApkPart>& parts) {
    auto safe = [](const std::wstring& s) {
        if (s.empty()) return false;
        for (wchar_t c : s) {
            const bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') ||
                            c == L'+' || c == L',' || c == L'-' || c == L'.' || c == L'=' || c == L'_';
            if (!ok) return false;
        }
        return true;
    };
    std::vector<InstallSet> out;
    std::vector<std::wstring> order;                       // the packages, as they first come
    std::map<std::wstring, std::vector<const ApkPart*>> of;  // a package's files
    for (const ApkPart& p : parts) {
        // A file of no known package is keyed by a name no package has (a package starts with a letter).
        const std::wstring key = p.package.empty() ? L"#" + std::to_wstring(order.size()) : p.package;
        if (!of.count(key)) order.push_back(key);
        of[key].push_back(&p);
    }
    for (const std::wstring& pkg : order) {
        const std::vector<const ApkPart*>& files = of[pkg];
        size_t base = 0;
        for (size_t i = 0; i < files.size(); ++i) {
            if (files[i]->split.empty()) {
                base = i;
                break;
            }
        }
        InstallSet set{files[base]->file, {}};
        std::vector<std::wstring> taken;
        for (size_t i = 0; i < files.size(); ++i) {
            if (i == base) continue;
            const std::wstring stem =
                L"split_" + (safe(files[i]->split) ? files[i]->split : std::to_wstring(set.splits.size() + 1));
            std::wstring name = stem + L".apk";
            for (int n = 2; std::find(taken.begin(), taken.end(), name) != taken.end(); ++n)
                name = stem + L"." + std::to_wstring(n) + L".apk";
            taken.push_back(name);
            set.splits.emplace_back(files[i]->file, name);
        }
        out.push_back(std::move(set));
    }
    return out;
}

}  // namespace winapk::launcher
