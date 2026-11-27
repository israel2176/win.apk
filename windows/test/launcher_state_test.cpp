// launcher_state_test.cpp -- winapk-launcher-state-test: the launcher's state machines
// (launcher/app_runs.h, launcher/installs.h, launcher/updates.h, launcher/store_app.h), which hold no lock and
// make no Windows call, driven step by step as the library's thread and a worker's thread drive them.
//
//   winapk-launcher-state-test
//
// An app's run: each path from start to end says the right thing (nothing, the splash goes, "didn't
// start", "stopped with an error"); Stop at each point of a run; steps out of order change nothing.
// An install: done, failed, left alone because the app runs; Cancel while it waits, while the file is
// copied, while the runtime converts it, once it is being kept and after its end; the queue's order.
// The update check and download: one check at a time; what each result leaves known and what the
// InfoBar says of it (once per release); a failed check keeps a newer version found before; the
// download's progress. The release offered (never a draft; a pre-release only while there is no full release),
// an installer downloaded only with GitHub's SHA-256 of it, and run only at the release's size, SHA-256, version
// and signer; a download's body kept only at the length HTTP announced. The Store (launcher/store_app.h): its
// apk-store.apk downloaded only with GitHub's SHA-256 of it and kept only at the release's size and SHA-256; first
// opening, the daily check's update, an update that waits while the store app runs, and each failure.
// The Threads window (launcher/debug_threads.h, include/winapk/debug_protocol.h): an agent's answer
// survives its own writer and a cut-off reply; every thread Windows lists is a row with or without an
// agent, and the agent's extra threads are rows too; CPU shares, the search and a sort that holds still.
// The library's files (launcher/library_files.h, std::filesystem only), on real files: a bundle kept as hard
// links to its installed APKs, that step done again over a cut-off one, an update's files set aside, then
// dropped or put back, never written.
// A batch's APK files (launcher/installs.h): AppInstall --set's lines read, and the files of one app made one
// install with its base kept as the app's .apk, a split named as a device names it, other apps' files apart.
// Background updates (launcher/maintenance.h): a maintenance window opens only with the setting on and the setup
// done, never beside a running window or broker, once every four hours (and at once with none noted, or when the
// clock went back), and waits while on battery, with no network or while an app starts; none once the user closed
// the library, and the hidden launcher waits for a window it opened only when the user did not close the library.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../launcher/app_runs.h"
#include "../launcher/debug_threads.h"
#include "../launcher/installs.h"
#include "../launcher/library_files.h"
#include "../launcher/maintenance.h"
#include "../launcher/store_app.h"
#include "../launcher/updates.h"

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

using winapk::launcher::RunEnd;
using winapk::launcher::RunState;
using winapk::launcher::RunTable;

// Stands in for TerminateProcess: counts what it was asked to end, and says what it is told to.
struct Killer {
    std::vector<void*> killed;
    bool works = true;
    bool operator()(void* p) {
        if (works) killed.push_back(p);
        return works;
    }
};

int g_process_a, g_process_b;  // two handles' worth of addresses
void* const kA = &g_process_a;
void* const kB = &g_process_b;

RunEnd::Kind kind_of(RunTable& t, const std::wstring& name) {
    const auto end = t.finish(name);
    return end ? end->kind() : static_cast<RunEnd::Kind>(-1);
}

void test_runs() {
    std::printf("an app's run\n");
    const std::wstring app = L"app", other = L"other";
    {
        RunTable t;
        check("no run: not active, nothing to stop, nothing to finish",
              !t.active(app) && t.empty() && !t.state(app) && !t.finish(app) && !t.expect_end(app));
        check("a run begins starting", t.begin(app) && t.state(app) == RunState::kStarting && t.active(app));
        check("a second start of it is refused", !t.begin(app));
        check("it has no process yet", t.process(app) == nullptr && !t.stop_requested(app));
        check("the runtime started: running, not to be ended", !t.attach(app, kA) && t.state(app) == RunState::kRunning);
        check("its process is the one given", t.process(app) == kA);
        check("its window is up: ready", t.ready(app) && t.state(app) == RunState::kReady);
        check("it is not over until the process is", !t.finish(app) && t.active(app));
        check("the process exited: ended, the handle forgotten",
              t.ended(app, true) && t.state(app) == RunState::kEnded && t.process(app) == nullptr);
        check("still counted as running until the library hears", t.active(app) && !t.begin(app));
        check("closed by its user: nothing to say", kind_of(t, app) == RunEnd::kQuiet);
        check("and it is forgotten", !t.active(app) && t.empty() && t.begin(app));
    }
    {
        RunTable t;
        t.begin(app);
        t.attach(app, kA);
        t.ready(app);
        t.ended(app, false);
        check("an error after its window was up: failed", kind_of(t, app) == RunEnd::kFailed);
    }
    {
        RunTable t;
        t.begin(app);
        t.attach(app, kA);
        t.ended(app, false);
        const auto end = t.finish(app);
        check("an error before its window: did not start", end && end->kind() == RunEnd::kDidNotStart && !end->clean);
        t.begin(app);
        t.attach(app, kA);
        t.ended(app, true);
        const auto clean = t.finish(app);
        check("exit code 0 before its window: closed by the app, quietly, as on a phone",
              clean && clean->kind() == RunEnd::kQuiet && clean->clean);
    }
    {
        RunTable t;
        t.begin(app);
        check("the runtime could not be started: ended from starting", t.ended(app, false));
        check("which is: did not start", kind_of(t, app) == RunEnd::kDidNotStart);
    }
    {  // Stop while the runtime is being started
        RunTable t;
        Killer kill;
        t.begin(app);
        check("stop while starting: taken, nothing to end yet", t.stop(app, kill) && kill.killed.empty());
        check("the run's thread sees it before it starts the runtime", t.stop_requested(app));
        t.ended(app, false);
        check("never started, by the user's word: stopped", kind_of(t, app) == RunEnd::kStopped);
    }
    {  // Stop lands between the thread's look and the runtime's start
        RunTable t;
        Killer kill;
        t.begin(app);
        const bool early = t.stop_requested(app);
        t.stop(app, kill);
        check("the thread saw no stop, then one came: the new runtime is to be ended at once",
              !early && t.attach(app, kA));
        check("it is recorded all the same", t.state(app) == RunState::kRunning && t.process(app) == kA);
        t.ended(app, false);
        check("its exit code 1 is the stop, not a failure", kind_of(t, app) == RunEnd::kStopped);
    }
    {  // Stop while it runs
        RunTable t;
        Killer kill;
        t.begin(app);
        t.attach(app, kA);
        check("stop before its window: the process is ended", t.stop(app, kill) && kill.killed == std::vector<void*>{kA});
        t.ended(app, false);
        check("stopped, not 'did not start'", kind_of(t, app) == RunEnd::kStopped);
        t.begin(app);
        t.attach(app, kB);
        t.ready(app);
        check("stop with its window up: the process is ended", t.stop(app, kill) && kill.killed.back() == kB);
        t.ended(app, false);
        check("stopped, not 'stopped with an error'", kind_of(t, app) == RunEnd::kStopped);
    }
    {  // A process that will not end
        RunTable t;
        Killer kill;
        kill.works = false;
        t.begin(app);
        t.attach(app, kA);
        t.ready(app);
        check("a process that would not end: not stopped", !t.stop(app, kill));
        t.ended(app, false);
        check("so its error is still told", kind_of(t, app) == RunEnd::kFailed);
    }
    {  // Stop after the end
        RunTable t;
        Killer kill;
        t.begin(app);
        t.attach(app, kA);
        t.ready(app);
        t.ended(app, false);
        check("stop after it ended: nothing to stop, nothing ended", !t.stop(app, kill) && kill.killed.empty());
        check("its error is still told", kind_of(t, app) == RunEnd::kFailed);
        t.begin(app);
        t.attach(app, kA);
        t.ended(app, false);
        check("an end the user asked for, heard of late, is expected", t.expect_end(app));
        check("and is no failure", kind_of(t, app) == RunEnd::kStopped);
    }
    {  // Ended by other means on the user's word: Remove, a permission taken away
        RunTable t;
        t.begin(app);
        t.attach(app, kA);
        t.ready(app);
        check("a running app whose runtime the launcher ends by other means: expected", t.expect_end(app));
        check("it is still running until its process is gone", t.state(app) == RunState::kReady);
        t.ended(app, false);
        check("its exit code 1 is no failure", kind_of(t, app) == RunEnd::kStopped);
    }
    {  // Steps out of order
        RunTable t;
        Killer kill;
        check("steps of a run that does not exist change nothing",
              !t.attach(app, kA) && !t.ready(app) && !t.ended(app, true) && !t.stop(app, kill) && t.empty());
        t.begin(app);
        check("ready before the runtime started: refused", !t.ready(app) && t.state(app) == RunState::kStarting);
        check("a runtime without a process: refused", !t.attach(app, nullptr) && t.state(app) == RunState::kStarting);
        t.attach(app, kA);
        check("a second runtime for the run: refused, the first kept", !t.attach(app, kB) && t.process(app) == kA);
        t.ready(app);
        check("ready twice: refused", !t.ready(app));
        t.ended(app, false);
        check("ended twice: refused, the first end kept", !t.ended(app, true));
        check("ready after the end: refused", !t.ready(app) && t.state(app) == RunState::kEnded);
        check("the first end is the one told", kind_of(t, app) == RunEnd::kFailed);
    }
    {  // Two apps
        RunTable t;
        Killer kill;
        t.begin(app);
        t.begin(other);
        t.attach(app, kA);
        t.attach(other, kB);
        t.ready(other);
        check("two runs are listed", t.names() == std::vector<std::wstring>{app, other});
        t.stop(app, kill);
        check("stopping one ends only its process", kill.killed == std::vector<void*>{kA});
        t.ended(app, false);
        t.ended(other, false);
        check("and excuses only its end", kind_of(t, app) == RunEnd::kStopped && kind_of(t, other) == RunEnd::kFailed);
    }
    {  // The exit codes Android's dialogs go by (include/winapk/app_exit.h)
        RunTable t;
        t.begin(app);
        t.attach(app, kA);
        t.ready(app);
        t.ended_with(app, winapk::kExitAppCrashed);
        check("the framework's crash handler ended it: crashed", kind_of(t, app) == RunEnd::kCrashed);
        t.begin(app);
        t.attach(app, kA);
        t.ended_with(app, winapk::kExitAppCrashed);
        check("a crash before its window is a crash too", kind_of(t, app) == RunEnd::kCrashed);
        t.begin(app);
        t.attach(app, kA);
        t.ready(app);
        t.ended_with(app, 0xC0000005UL);
        check("a native fault is a crash", kind_of(t, app) == RunEnd::kCrashed);
        t.begin(app);
        t.attach(app, kA);
        t.ended_with(app, 0xC0000135UL);
        check("a DLL that would not load is not the app's crash", kind_of(t, app) == RunEnd::kDidNotStart);
        t.begin(app);
        t.attach(app, kA);
        t.ready(app);
        t.ended_with(app, 10);
        check("the app's own System.exit(10) is no crash", kind_of(t, app) == RunEnd::kFailed);
        t.begin(app);
        t.attach(app, kA);
        t.ready(app);
        t.ended_with(app, winapk::kExitClosedNotResponding);
        check("Close app in the not-responding dialog: the user's word", kind_of(t, app) == RunEnd::kStopped);
        Killer kill;
        t.begin(app);
        t.attach(app, kA);
        t.stop(app, kill);
        t.ended_with(app, winapk::kExitAppCrashed);
        check("a crash while being stopped: stopped", kind_of(t, app) == RunEnd::kStopped);
    }
}

using winapk::launcher::InstallEnd;
using winapk::launcher::InstallState;
using winapk::launcher::InstallTable;

InstallEnd::Kind how(InstallTable& t, const std::wstring& name) {
    const auto end = t.finish(name);
    return end ? end->kind() : static_cast<InstallEnd::Kind>(-1);
}

void test_installs() {
    std::printf("an install\n");
    const std::wstring a = L"a", b = L"b", c = L"c";
    {
        InstallTable t;
        check("nothing queued: not pending, no current, nothing to finish",
              !t.pending(a) && t.empty() && t.current().empty() && !t.finish(a) && !t.state(a));
        check("an install is queued", t.begin(a) && t.state(a) == InstallState::kQueued && t.pending(a));
        check("the same file twice is refused", !t.begin(a));
        check("queued is not under way", t.current().empty());
        check("its turn: installing", t.start(a) && t.state(a) == InstallState::kInstalling && t.current() == a);
        check("the runtime started: not to be ended", !t.attach(a, kA));
        t.detach(a);
        check("the runtime succeeded: kept", t.commit(a) && t.state(a) == InstallState::kCommitted && t.current() == a);
        check("not over until its thread says so", !t.finish(a));
        check("ended", t.ended(a, true, false) && t.state(a) == InstallState::kEnded && t.current().empty());
        check("still pending until the library hears", t.pending(a) && !t.begin(a));
        check("it is done", how(t, a) == InstallEnd::kDone);
        check("and forgotten", !t.pending(a) && t.empty() && t.begin(a));
    }
    {
        InstallTable t;
        t.begin(a);
        t.start(a);
        t.attach(a, kA);
        t.detach(a);
        t.ended(a, false, false);
        check("the runtime failed: failed", how(t, a) == InstallEnd::kFailed);
        t.begin(a);
        t.start(a);
        t.ended(a, false, true);
        check("the app runs: left alone, and said so", how(t, a) == InstallEnd::kAppRunning);
        t.begin(a);
        t.start(a);
        t.attach(a, kA);
        t.detach(a);
        t.commit(a);
        t.ended(a, false, false);
        check("kept, but its name could not be written: failed", how(t, a) == InstallEnd::kFailed);
        t.begin(a);
        t.start(a);
        t.ended(a, true, false);
        check("ok without having been committed is not done", how(t, a) == InstallEnd::kFailed);
    }
    {  // Cancel while it waits for its turn
        InstallTable t;
        Killer kill;
        t.begin(a);
        check("cancel while queued: taken, nothing to end", t.cancel(a, kill) && kill.killed.empty());
        check("its turn comes: it does not start", !t.start(a) && t.state(a) == InstallState::kEnded);
        check("the thread's report changes nothing", !t.ended(a, false, false));
        check("cancelled", how(t, a) == InstallEnd::kCancelled);
    }
    {  // Cancel while the file is copied in, before the runtime
        InstallTable t;
        Killer kill;
        t.begin(a);
        t.start(a);
        check("cancel before the runtime: taken, nothing to end yet", t.cancel(a, kill) && kill.killed.empty());
        check("the runtime, started after it, is to be ended at once", t.attach(a, kA));
        t.detach(a);
        check("and the install is not kept", !t.commit(a) && t.state(a) == InstallState::kInstalling);
        t.ended(a, false, false);
        check("cancelled", how(t, a) == InstallEnd::kCancelled);
    }
    {  // Cancel while the runtime converts the app
        InstallTable t;
        Killer kill;
        t.begin(a);
        t.start(a);
        t.attach(a, kA);
        check("cancel under way: its runtime is ended", t.cancel(a, kill) && kill.killed == std::vector<void*>{kA});
        t.detach(a);
        t.ended(a, false, false);
        check("cancelled, not failed", how(t, a) == InstallEnd::kCancelled);
    }
    {  // The runtime succeeded, then Cancel: the cancel wins while the install is not kept yet
        InstallTable t;
        Killer kill;
        t.begin(a);
        t.start(a);
        t.attach(a, kA);
        t.detach(a);
        check("cancel after the runtime exited, before it is kept: taken", t.cancel(a, kill) && kill.killed.empty());
        check("so it is not kept", !t.commit(a));
        t.ended(a, true, false);
        check("cancelled, whatever the thread reports", how(t, a) == InstallEnd::kCancelled);
    }
    {  // Cancel once it is being kept
        InstallTable t;
        Killer kill;
        t.begin(a);
        t.start(a);
        t.attach(a, kA);
        t.detach(a);
        t.commit(a);
        check("cancel once it is being kept: refused", !t.cancel(a, kill));
        t.ended(a, true, false);
        check("it is done, never 'cancelled' with the app installed", how(t, a) == InstallEnd::kDone);
    }
    {  // Cancel after the end, before the library heard
        InstallTable t;
        Killer kill;
        t.begin(a);
        t.start(a);
        t.ended(a, false, false);
        check("cancel after the end: refused", !t.cancel(a, kill));
        check("its failure is still told", how(t, a) == InstallEnd::kFailed);
        check("cancel of one that is gone: refused", !t.cancel(a, kill));
        t.begin(a);
        check("the next install of that name starts: no cancel was left behind", t.start(a));
    }
    {  // The queue
        InstallTable t;
        Killer kill;
        t.begin(c);
        t.begin(a);
        t.begin(b);
        check("pending installs are listed in the order queued", t.names() == std::vector<std::wstring>{c, a, b});
        t.start(c);
        t.attach(c, kA);
        check("cancelling a waiting one ends nobody's runtime", t.cancel(a, kill) && kill.killed.empty());
        check("cancelling the one under way ends its own", t.cancel(c, kill) && kill.killed == std::vector<void*>{kA});
        t.detach(c);
        t.ended(c, false, false);
        t.start(a);
        t.start(b);
        t.attach(b, kB);
        check("the next one's runtime was not touched", kill.killed.size() == 1 && t.current() == b);
        check("what is left is listed", t.names() == std::vector<std::wstring>{c, a, b});
        check("each ends its own way", how(t, c) == InstallEnd::kCancelled && how(t, a) == InstallEnd::kCancelled);
        check("the queue shrinks", t.names() == std::vector<std::wstring>{b});
    }
    {  // Steps out of order, and an install in no table
        InstallTable t;
        Killer kill;
        check("an install in no table (the install helper's) cannot be cancelled: kept",
              t.commit(a) && !t.attach(a, kA) && !t.cancel(a, kill) && t.empty());
        check("steps of one that does not exist change nothing", !t.start(a) && !t.ended(a, true, false) && t.empty());
        t.begin(a);
        check("a runtime before its turn: refused", !t.attach(a, kA) && !t.commit(a));
        check("cancel does not reach a process that was refused", t.cancel(a, kill) && kill.killed.empty());
        t.start(a);
        t.finish(a);
        t.begin(b);
        t.start(b);
        check("its turn twice: refused", !t.start(b) && t.state(b) == InstallState::kInstalling);
        t.commit(b);
        check("kept twice: refused", !t.commit(b));
        t.ended(b, true, false);
        check("ended twice: refused, the first end kept", !t.ended(b, false, true) && how(t, b) == InstallEnd::kDone);
    }
}

struct TestRelease {
    std::wstring version;
};
using Tracker = winapk::launcher::UpdateTracker<TestRelease>;
using winapk::launcher::CheckResult;

// A check's result: GitHub asked or the kept answer read, by hand or not, and what came back.
CheckResult result(bool fetch, bool manual, bool reached, bool ok, bool found) {
    CheckResult r;
    r.fetch = fetch;
    r.manual = manual;
    r.reached = reached;
    r.ok = ok;
    r.found = found;
    return r;
}

void test_updates() {
    std::printf("the update check and download\n");
    using Say = Tracker::Say;
    using Known = Tracker::Known;
    const TestRelease v2{L"2.0"}, v3{L"3.0"}, same{L"1.0"};
    {
        Tracker t;
        check("at start: idle, nothing known, nothing to download",
              t.check() == Tracker::Check::kIdle && t.known() == Known::kNothing && !t.available() &&
                  !t.fetch_failed() && !t.downloading() && t.percent() == -1 && !t.begin_download());
        check("reading the kept answer is a check, but not asking",
              t.begin_check(false) && t.check() == Tracker::Check::kReadingKept && !t.asking());
        check("one check at a time", !t.begin_check(true) && t.check() == Tracker::Check::kReadingKept);
        check("no kept answer: nothing known, nothing said, no failure",
              t.checked(result(false, false, true, false, false), {}, false, true) == Say::kNothing &&
                  t.known() == Known::kNothing && !t.fetch_failed() && t.check() == Tracker::Check::kIdle);
        check("asking GitHub", t.begin_check(true) && t.asking());
        t.abort_check();
        check("a check whose thread did not start is over", t.check() == Tracker::Check::kIdle && t.begin_check(true));
        check("the daily check finds the same version: known, nothing said",
              t.checked(result(true, false, true, true, true), same, false, true) == Say::kNothing &&
                  t.known() == Known::kRelease && !t.available() && !t.fetch_failed());
        t.begin_check(true);
        check("'Check' finds the same version: up to date",
              t.checked(result(true, true, true, true, true), same, false, true) == Say::kUpToDate);
    }
    {
        Tracker t;
        t.begin_check(true);
        check("the daily check finds a newer version: said",
              t.checked(result(true, false, true, true, true), v2, true, true) == Say::kNewVersion && t.available() &&
                  t.latest().version == L"2.0");
        t.begin_check(true);
        check("the next check finds it again: said once only",
              t.checked(result(true, false, true, true, true), v2, true, true) == Say::kNothing && t.available());
        t.begin_check(true);
        check("'Check' finds it again: still nothing new to say, and not 'up to date'",
              t.checked(result(true, true, true, true, true), v2, true, true) == Say::kNothing);
        t.begin_check(true);
        check("a newer one still: said again",
              t.checked(result(true, false, true, true, true), v3, true, true) == Say::kNewVersion &&
                  t.latest().version == L"3.0");
    }
    {
        Tracker t;
        t.begin_check(false);
        check("the daily check off, a newer version in the kept answer: known, not said",
              t.checked(result(false, false, true, true, true), v2, true, false) == Say::kNothing && t.available());
        t.begin_check(true);
        check("'Check' with the daily check off: said",
              t.checked(result(true, true, true, true, true), v2, true, false) == Say::kNewVersion);
    }
    {
        Tracker t;
        t.begin_check(true);
        t.checked(result(true, false, true, true, true), v2, true, true);
        t.begin_check(true);
        check("GitHub not reached, by hand: could not check",
              t.checked(result(true, true, false, true, true), v2, true, true) == Say::kCouldNotCheck &&
                  t.fetch_failed());
        check("the newer version found before is still known", t.available() && t.latest().version == L"2.0");
        t.begin_check(true);
        check("GitHub not reached and nothing kept: failed, what was known stays",
              t.checked(result(true, false, false, false, false), {}, false, true) == Say::kNothing &&
                  t.fetch_failed() && t.known() == Known::kRelease && t.available());
        t.begin_check(false);
        check("reading the kept answer does not clear the failure",
              t.checked(result(false, false, true, true, true), v2, true, true) == Say::kNothing && t.fetch_failed());
        t.begin_check(true);
        check("GitHub answers again: no failure",
              t.checked(result(true, false, true, true, true), v2, true, true) == Say::kNothing && !t.fetch_failed());
        t.begin_check(true);
        check("an answer that cannot be read is a failure too",
              t.checked(result(true, true, true, false, false), {}, false, true) == Say::kCouldNotCheck);
        t.begin_check(true);
        check("a list without a release: none known, by hand 'up to date'",
              t.checked(result(true, true, true, true, false), {}, false, true) == Say::kUpToDate &&
                  t.known() == Known::kNoRelease && !t.available());
        t.begin_check(true);
        check("the release is back: news again",
              t.checked(result(true, false, true, true, true), v2, true, true) == Say::kNewVersion);
    }
    {
        Tracker t;
        t.progress(40);
        check("progress without a download changes nothing", !t.downloading() && t.percent() == -1);
        t.begin_check(true);
        t.checked(result(true, false, true, true, true), v2, true, true);
        check("the download starts at 0", t.begin_download() && t.downloading() && t.percent() == 0);
        check("one download at a time", !t.begin_download());
        t.progress(57);
        check("it moves", t.percent() == 57);
        check("a check may run meanwhile", t.begin_check(true) && t.downloading());
        t.checked(result(true, false, true, true, true), v2, true, true);
        check("and leaves the download alone", t.percent() == 57);
        t.downloaded();
        check("it ends", !t.downloading() && t.percent() == -1 && t.begin_download());
    }
}

// The rules a release, its installer and a download's body are held to (updates.h): GitHub's release fields, the
// file's facts, HTTP's framing.
void test_update_integrity() {
    using namespace winapk::launcher;
    std::printf("the release and its installer\n");
    check("versions: v0.10.0 > 0.9.1, 0.2 = 0.2.0, the suffix not counted",
          compare_versions(L"v0.10.0", L"0.9.1") > 0 && compare_versions(L"0.2", L"0.2.0") == 0 &&
              compare_versions(L"0.3.0-preview", L"0.3.0") == 0 && version_numbers(L"preview").empty());

    const std::string hex(64, 'a');
    check("a digest: sha256: and 64 hex digits, read in any case, given in lower case",
          asset_sha256("sha256:" + hex) == hex && asset_sha256("sha256:" + std::string(64, 'A')) == hex &&
              asset_sha256("SHA256:" + hex) == hex);
    check("another algorithm, a digit short or long, a letter past f, nothing: no SHA-256",
          asset_sha256("sha1:" + hex).empty() && asset_sha256("sha512:" + hex).empty() &&
              asset_sha256("sha256:" + hex.substr(1)).empty() && asset_sha256("sha256:" + hex + "a").empty() &&
              asset_sha256("sha256:" + std::string(63, 'a') + "g").empty() && asset_sha256("sha256:").empty() &&
              asset_sha256("").empty() && asset_sha256(" sha256:" + hex.substr(1)).empty());

    auto rel = [&](const wchar_t* tag, bool draft, bool pre) {
        ListedRelease r;
        r.tag = tag;
        r.draft = draft;
        r.prerelease = pre;
        r.asset = true;
        r.asset_state = "uploaded";
        r.digest = "sha256:" + hex;
        r.size = 1000;
        return r;
    };
    check("an installer uploaded whole, of a believable size, with GitHub's SHA-256: downloaded",
          installer_checkable(rel(L"v1", 0, 0)));
    {
        ListedRelease r = rel(L"v1", 0, 0);
        r.digest.clear();
        check("no digest: not downloaded, its size known or not", !installer_checkable(r));
        r.digest = "sha1:" + std::string(40, 'a');
        check("a digest of another algorithm: not downloaded", !installer_checkable(r));
        r = rel(L"v1", 0, 0);
        r.asset_state = "open";
        check("an upload still running: not downloaded", !installer_checkable(r));
        r.asset_state.clear();
        check("no state at all: not downloaded", !installer_checkable(r));
        r = rel(L"v1", 0, 0);
        r.size = 0;
        check("a size of 0: not downloaded", !installer_checkable(r));
        r.size = kMaxSetupBytes;
        check("the largest size believed: downloaded", installer_checkable(r));
        r.size = kMaxSetupBytes + 1;
        check("past it: not downloaded", !installer_checkable(r));
        r = rel(L"v1", 0, 0);
        r.asset = false;
        check("no installer asset: nothing to download", !installer_checkable(r));
    }

    check("an empty list offers nothing", pick_release({}) == -1);
    check("the highest version wins, wherever it is listed",
          pick_release({rel(L"v0.8.0", 0, 0), rel(L"v0.10.0", 0, 0), rel(L"v0.9.0", 0, 0)}) == 1);
    check("equal numbers: the one listed first (GitHub lists the newest first)",
          pick_release({rel(L"v1.0.0", 0, 0), rel(L"1.0", 0, 0)}) == 0);
    check("never a draft, even the highest", pick_release({rel(L"v0.8.0", 0, 0), rel(L"v0.9.0", 1, 0)}) == 0 &&
                                                pick_release({rel(L"v0.9.0", 1, 0)}) == -1);
    check("only pre-releases: the highest of them",
          pick_release({rel(L"v0.7.0", 0, 1), rel(L"v0.8.0", 0, 1)}) == 1);
    check("a full release listed: no pre-release, newer or not",
          pick_release({rel(L"v1.1.0-beta", 0, 1), rel(L"v1.0.0", 0, 0)}) == 1 &&
              pick_release({rel(L"v2.0.0", 0, 1), rel(L"v0.1.0", 0, 0), rel(L"v0.9.0", 0, 1)}) == 1);
    check("a full release that is a draft does not hide the pre-releases",
          pick_release({rel(L"v1.0.0", 1, 0), rel(L"v0.9.0", 0, 1)}) == 1);
    check("a full release whose tag is no version does not hide them either",
          pick_release({rel(L"nightly", 0, 0), rel(L"v0.9.0", 0, 1)}) == 1);
    check("a tag that is no version is never offered", pick_release({rel(L"nightly", 0, 0)}) == -1);

    check("a version as text: no v, no padding, no NUL",
          plain_version(L"v0.9.0") == L"0.9.0" && plain_version(L"  0.9.0-preview   ") == L"0.9.0-preview" &&
              plain_version(std::wstring(L"1.0\0", 4)) == L"1.0" && plain_version(L"   ").empty());

    SetupFacts f;
    f.size = 1000;
    f.sha256 = hex;
    f.product_version = L"0.9.0-preview   ";  // Inno Setup pads it
    f.signature = Signature::kUnsigned;
    const std::wstring v = L"0.9.0-preview";
    check("unsigned build, unsigned installer of the release's size, SHA-256 and version: runs, said unsigned",
          judge_setup(f, 1000, hex, v, L"") == SetupVerdict::kOkUnsigned && setup_runs(SetupVerdict::kOkUnsigned));
    check("the version asked as the tag", judge_setup(f, 1000, hex, L"v0.9.0-preview", L"") ==
                                              SetupVerdict::kOkUnsigned);
    check("shorter or longer than the release's asset: refused",
          judge_setup(f, 999, hex, v, L"") == SetupVerdict::kSize &&
              judge_setup(f, 1001, hex, v, L"") == SetupVerdict::kSize && !setup_runs(SetupVerdict::kSize));
    check("the size is judged first: a wrong size with a wrong SHA-256 says size",
          judge_setup(f, 999, std::string(64, 'b'), v, L"") == SetupVerdict::kSize);
    check("an asset of no size never matches", [&] {
        SetupFacts e = f;
        e.size = 0;
        return judge_setup(e, 0, hex, v, L"") == SetupVerdict::kSize;
    }());
    check("another SHA-256: refused", judge_setup(f, 1000, std::string(64, 'b'), v, L"") == SetupVerdict::kDigest);
    check("no SHA-256 to hold it to: refused", judge_setup(f, 1000, "", v, L"") == SetupVerdict::kDigest);
    {
        SetupFacts g = f;
        g.product_version = L"0.8.0";
        check("another version inside: refused", judge_setup(g, 1000, hex, v, L"") == SetupVerdict::kVersion);
        g.product_version = L"0.9.0";
        check("the same numbers without the suffix: refused",
              judge_setup(g, 1000, hex, v, L"") == SetupVerdict::kVersion);
        g.product_version.clear();
        check("no version inside: refused", judge_setup(g, 1000, hex, v, L"") == SetupVerdict::kVersion);
        check("no version asked: refused", judge_setup(f, 1000, hex, L"", L"") == SetupVerdict::kVersion);
        g = f;
        g.signature = Signature::kInvalid;
        check("a broken signature: refused, by a signed build or not",
              judge_setup(g, 1000, hex, v, L"") == SetupVerdict::kBadSignature &&
                  judge_setup(g, 1000, hex, v, L"CN=Win.apk | CN=CA") == SetupVerdict::kBadSignature);
        g = f;
        g.signature = Signature::kValid;
        g.signer = L"CN=Win.apk | CN=CA";
        check("signed, by an unsigned build: runs", judge_setup(g, 1000, hex, v, L"") == SetupVerdict::kOk);
        check("signed by this build's signer: runs",
              judge_setup(g, 1000, hex, v, L"CN=Win.apk | CN=CA") == SetupVerdict::kOk);
        check("the same name from another authority: refused",
              judge_setup(g, 1000, hex, v, L"CN=Win.apk | CN=Other CA") == SetupVerdict::kSignerChanged);
        check("signed by another: refused",
              judge_setup(g, 1000, hex, v, L"CN=Someone else | CN=CA") == SetupVerdict::kSignerChanged);
        check("a signed build, an unsigned installer: refused",
              judge_setup(f, 1000, hex, v, L"CN=Win.apk | CN=CA") == SetupVerdict::kNotSigned);
        g.product_version = L"0.8.0";
        check("signed as it should be but another version inside: refused",
              judge_setup(g, 1000, hex, v, L"CN=Win.apk | CN=CA") == SetupVerdict::kVersion);
    }

    std::printf("a download's body\n");
    auto body = [](const wchar_t* length, const wchar_t* encoding = L"", const wchar_t* transfer = L"") {
        return expected_body(length, encoding, transfer, true);
    };
    check("no Content-Length: any length is whole", body(L"").kind == BodyLength::kUnknown && body(L"").whole(5));
    check("a Content-Length: exactly that many bytes",
          body(L"1000").exact() && body(L"1000").bytes == 1000 && body(L"1000").whole(1000) &&
              !body(L"1000").whole(999) && !body(L"1000").whole(1001));
    check("past 4 GB", body(L"5000000000").exact() && body(L"5000000000").bytes == 5000000000ull);
    check("a repeated header of one value counts; of two, the response is broken",
          body(L"1000, 1000").exact() && body(L"1000, 1000").bytes == 1000 &&
              body(L"1000, 999").kind == BodyLength::kBroken && !body(L"1000, 999").whole(1000));
    check("a value that is no number, signed, empty in a list or too long: broken",
          body(L"abc").kind == BodyLength::kBroken && body(L"-1").kind == BodyLength::kBroken &&
              body(L"+1").kind == BodyLength::kBroken && body(L"1000,").kind == BodyLength::kBroken &&
              body(L"12 34").kind == BodyLength::kBroken &&
              body(L"99999999999999999999").kind == BodyLength::kBroken);
    check("padding around it is not part of it", body(L" 1000 ").exact() && body(L" 1000 ").bytes == 1000);
    check("an encoding the client inflates: the length counts for nothing",
          body(L"1000", L"gzip").kind == BodyLength::kUnknown && body(L"1000", L"gzip").whole(3000));
    check("identity is no encoding", body(L"1000", L"Identity").exact());
    check("an encoding left as it came: the length holds",
          expected_body(L"1000", L"gzip", L"", false).exact());
    check("a Transfer-Encoding frames the body: the length is ignored, broken or not",
          body(L"1000", L"", L"chunked").kind == BodyLength::kUnknown &&
              body(L"1000, 5", L"", L"chunked").kind == BodyLength::kUnknown);
}

void test_store_setup() {
    using namespace winapk::launcher;
    using Step = StoreSetup::Step;
    using Failure = StoreSetup::Failure;
    std::printf("the Store's setup\n");
    ListedRelease r;
    r.tag = L"v1.4";
    r.asset = true;
    r.asset_state = "uploaded";
    r.size = 1588099;
    r.digest = "sha256:" + std::string(64, 'a');
    check("an uploaded apk-store.apk with GitHub's SHA-256: checkable", store_asset_checkable(r));
    ListedRelease bad = r;
    bad.digest.clear();
    check("no digest: not downloaded", !store_asset_checkable(bad));
    bad = r;
    bad.size = kMaxStoreBytes + 1;
    check("an unbelievable size: not downloaded", !store_asset_checkable(bad));
    bad = r;
    bad.asset_state = "open";
    check("still uploading: not downloaded", !store_asset_checkable(bad));
    const std::string sha(64, 'a');
    check("the release's size and SHA-256: kept", judge_store_file(1588099, sha, 1588099, sha) == StoreFile::kOk);
    check("another size: refused before its hash",
          judge_store_file(1588098, sha, 1588099, sha) == StoreFile::kWrongSize);
    check("another SHA-256: refused",
          judge_store_file(1588099, std::string(64, 'b'), 1588099, sha) == StoreFile::kWrongSha256);
    check("an empty file: refused", judge_store_file(0, "", 0, "") == StoreFile::kWrongSize);

    {
        StoreSetup s;
        check("first opening: the check starts", s.begin_check() && s.step() == Step::kChecking && s.busy());
        check("one at a time", !s.begin_check());
        check("a release, nothing installed: download", s.checked(true, true, false, false) &&
                                                            s.step() == Step::kDownloading && s.percent() == 0);
        s.progress(42);
        check("its progress", s.percent() == 42);
        check("checked, not running: install now", s.downloaded(true, true, false) && s.step() == Step::kInstalling);
        check("an install's end out of order changes nothing", !s.begin_check());
        s.installed(true);
        check("installed: idle", s.step() == Step::kIdle && s.failure() == Failure::kNone && !s.busy());
    }
    {
        StoreSetup s;
        s.begin_check();
        check("GitHub unreached, nothing installed: failed, unreached",
              !s.checked(false, false, false, false) && s.step() == Step::kFailed &&
                  s.failure() == Failure::kUnreached && !s.busy());
        check("Try again: a new check", s.begin_check() && s.failure() == Failure::kNone);
        check("no checkable release: failed, no release",
              !s.checked(true, false, false, false) && s.failure() == Failure::kNoRelease);
        s.begin_check();
        s.checked(true, true, false, false);
        check("a file that is not the release's: failed, never installed",
              !s.downloaded(true, false, false) && s.step() == Step::kFailed && s.failure() == Failure::kUnverified);
        s.begin_check();
        s.checked(true, true, false, false);
        check("a download that broke off: failed, unreached",
              !s.downloaded(false, false, false) && s.failure() == Failure::kUnreached);
        s.begin_check();
        s.checked(true, true, false, false);
        s.downloaded(true, true, false);
        s.installed(false);
        check("an install that failed: failed, install", s.step() == Step::kFailed && s.failure() == Failure::kInstall);
    }
    {
        StoreSetup s;
        s.begin_check();
        check("the daily check, the installed copy is the release's: nothing to do",
              !s.checked(true, true, true, true) && s.step() == Step::kIdle);
        s.begin_check();
        check("installed, GitHub unreached: the installed store stays, no failure shown",
              !s.checked(false, false, false, true) && s.step() == Step::kIdle && s.failure() == Failure::kNone);
        s.begin_check();
        check("a newer release: download", s.checked(true, true, false, true));
        check("checked while the store runs: it waits", !s.downloaded(true, true, true) && s.step() == Step::kWaiting);
        check("still running: still waits", !s.may_install(true) && s.step() == Step::kWaiting && s.busy());
        check("no check while it waits", !s.begin_check());
        check("the store stopped: install now", s.may_install(false) && s.step() == Step::kInstalling);
        s.installed(true);
        check("updated", s.step() == Step::kIdle);
        check("may_install with nothing waiting does nothing", !s.may_install(false) && s.step() == Step::kIdle);
    }
}

void test_debug_threads() {
    using namespace winapk::launcher;
    namespace dbg = winapk::debug;
    std::printf("the Threads window\n");
    dbg::AgentThread main_thread;
    main_thread.tid = 100;
    main_thread.kind = "java";
    main_thread.jid = 1;
    main_thread.state = "in native code";
    main_thread.start = "winapk-runtime.exe+0x1000";
    main_thread.name = "main";
    dbg::AgentThread worker;
    worker.tid = 101;
    worker.kind = "guest";
    worker.start = "libgame.so!render_loop+0x0";
    worker.name = "Render\tThread\n";  // a name may hold anything
    dbg::AgentThread finalizer;  // a Java thread whose Windows thread the agent does not know
    finalizer.kind = "java";
    finalizer.jid = 9;
    finalizer.daemon = true;
    finalizer.state = "waiting";
    finalizer.name = "Finalizer";
    dbg::AgentThread late;  // started after Windows' list was taken
    late.tid = 300;
    late.kind = "java";
    late.jid = 40;
    late.state = "running";
    late.name = "pool-1-thread-1";
    const std::string text = dbg::format_header() + dbg::format_thread(main_thread) + dbg::format_thread(worker) +
                             dbg::format_thread(finalizer) + dbg::format_thread(late);
    const dbg::AgentReply reply = dbg::parse_threads(text);
    check("an answer reads back", reply.ok && reply.threads.size() == 4 && reply.threads[0].name == "main" &&
                                      reply.threads[0].tid == 100 && reply.threads[0].jid == 1 &&
                                      reply.threads[0].state == "in native code" &&
                                      reply.threads[2].daemon && reply.threads[2].tid == 0);
    check("a tab or a line break in a name shifts nothing",
          reply.threads[1].name == "Render Thread " && reply.threads[1].start == "libgame.so!render_loop+0x0");
    check("a reply cut short keeps its whole lines",
          dbg::parse_threads(text.substr(0, text.size() - 5)).threads.size() == 3);
    check("another program's answer is not one", !dbg::parse_threads("hello\nT\t1\tjava\t1\t0\t\t\tx\n").ok);
    check("a newer agent's answer is not guessed at",
          !dbg::parse_threads("winapk-debug 2\nT\t1\tjava\t1\t0\t\t\tx\n").ok);
    check("the pipe is the process's", dbg::pipe_name(42) == L"\\\\.\\pipe\\winapk-debug-42");

    std::vector<OsThread> os(4);
    os[0].tid = 100;
    os[0].timed = true;
    os[0].cpu = 1000;
    os[0].start = "winapk-runtime.exe";
    os[1].tid = 101;
    os[1].timed = true;
    os[1].cpu = 5000;
    os[1].name = "Render";
    os[1].start = "winapk-runtime.exe";
    os[2].tid = 102;  // Windows' own, which no agent names
    os[2].start = "ntdll.dll";
    os[3].tid = 103;
    os[3].start = "jvm.dll";
    {
        const std::vector<ThreadRow> rows = merge_threads(7, "winapk-runtime.exe", os, nullptr);
        check("without an agent every Windows thread is a row", rows.size() == 4 && rows[1].name == "Render");
        check("and is not called what it may not be",
              rows[0].kind == ThreadKind::kUnknown && rows[3].kind == ThreadKind::kUnknown &&
                  rows[2].kind == ThreadKind::kSystem && !rows[0].agent);
        dbg::AgentReply bad;
        bad.threads = reply.threads;
        check("an answer that is not one adds nothing", merge_threads(7, "x", os, &bad).size() == 4);
    }
    std::vector<ThreadRow> rows = merge_threads(7, "winapk-runtime.exe", os, &reply);
    check("with an agent no Windows thread is lost", rows.size() == 6 && rows[2].tid == 102 && rows[3].tid == 103);
    check("a Java thread takes its name and state",
          rows[0].kind == ThreadKind::kJava && rows[0].name == "main" && rows[0].state == "in native code" &&
              rows[0].jid == 1 && rows[0].agent && rows[0].timed);
    check("an app's native thread says where it started",
          rows[1].kind == ThreadKind::kApp && rows[1].start == "libgame.so!render_loop+0x0");
    check("a thread the agent does not name keeps what Windows says",
          !rows[2].agent && rows[2].kind == ThreadKind::kSystem && rows[2].start == "ntdll.dll");
    check("a Java thread without a Windows thread is a row", rows[4].tid == 0 && rows[4].name == "Finalizer" &&
                                                                  rows[4].daemon && !rows[4].timed);
    check("a thread that started since is a row", rows[5].tid == 300 && rows[5].name == "pool-1-thread-1");
    const ThreadCounts counts = count_threads(rows);
    check("the counts add up", counts.total == 6 && counts.java == 3 && counts.app == 1 && counts.system == 1 &&
                                   counts.unknown == 1);

    CpuTracker cpu;
    cpu.apply(rows, 10'000'000);
    check("no share at the first look", rows[0].cpu_pct < 0 && rows[1].cpu_pct < 0);
    os[0].cpu += 2'500'000;  // a quarter of the second
    os[1].cpu += 10'000'000;
    rows = merge_threads(7, "winapk-runtime.exe", os, &reply);
    cpu.apply(rows, 20'000'000);
    check("a share of one core over the interval", rows[0].cpu_pct > 24.9 && rows[0].cpu_pct < 25.1 &&
                                                       rows[1].cpu_pct > 99.9 && rows[1].cpu_pct < 100.1);
    check("none for a thread that cannot be timed", rows[2].cpu_pct < 0 && rows[4].cpu_pct < 0);
    {
        std::vector<ThreadRow> other = merge_threads(8, "x", os, nullptr);  // the same thread ids, another process
        cpu.apply(other, 30'000'000);
        check("another process's thread of the same id is another thread", other[0].cpu_pct < 0);
    }

    check("the search finds a name, whatever its case", row_matches(rows[4], "final") && !row_matches(rows[0], "final"));
    check("and a kind, a state, a start, a thread id",
          row_matches(rows[1], "app native") && row_matches(rows[0], "native") && row_matches(rows[1], "libgame") &&
              row_matches(rows[2], "102") && !row_matches(rows[2], "java"));
    check("every word must be there", row_matches(rows[0], "main java") && !row_matches(rows[0], "main jvm"));
    check("an empty search has every row", row_matches(rows[2], "") && row_matches(rows[2], "  "));

    sort_rows(rows, ThreadColumn::kCpuPct, true);
    check("sorted by CPU, the busiest first", rows[0].tid == 101 && rows[1].tid == 100);
    check("equal rows keep one order", rows[2].tid == 0 && rows[3].tid == 102 && rows[4].tid == 103 && rows[5].tid == 300);
    sort_rows(rows, ThreadColumn::kName, false);
    check("sorted by name", rows[0].name.empty() && rows[2].name == "Finalizer" && rows[3].name == "main");
    sort_rows(rows, ThreadColumn::kTid, true);
    check("sorted by thread id, downwards", rows[0].tid == 300 && rows[5].tid == 0);
}

// The library's files (launcher/library_files.h), on real files in a scratch folder: a bundle kept as links
// to its installed APKs, that step redone over what a cut-off one left, an update's set-aside, kept and
// put back, and what counts as the app's entries.
void test_library_files() {
    using namespace winapk::launcher;
    namespace fs = std::filesystem;
    std::printf("the library's files\n");
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / ("winapk-library-test-" + std::to_string(std::rand()));
    fs::remove_all(root, ec);
    const fs::path library = root / "library", copy = root / "android" / "app" / "com.example-Qm";
    fs::create_directories(library);
    fs::create_directories(copy);
    auto write = [](const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; };
    auto read = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    write(copy / "base.apk", "base v1");
    write(copy / "split_config.arm64_v8a.apk", "abi v1");
    write(copy / "split_config.xxhdpi.apk", "dpi v1");
    write(copy / ".installed", "3 100:1700000000000\n");
    const fs::path bundle = library / "App.xapk", apk = library / "App.apk", splits = library / "App.splits";
    write(bundle, "the whole bundle");

    check("bundles by extension, in any case", is_bundle_file("a.xapk") && is_bundle_file("a.APKM") &&
                                                   is_bundle_file("a.apks") && !is_bundle_file("a.apk"));
    check("stamp: same size and time", stamp_matches("3 100:1700000000000\n", 100, 1700000000000LL));
    check("stamp: another time or size is not", !stamp_matches("3 100:1700000000000\n", 100, 1700000000001LL) &&
                                                     !stamp_matches("3 100:1700000000000", 101, 1700000000000LL));
    check("stamp: no stamp is not", !stamp_matches("", 0, 0) && !stamp_matches("garbage", 0, 0));

    // Installed: the bundle becomes links to the installed APKs.
    fs::path kept = keep_installed(bundle, copy);
    check("kept as <name>.apk", kept == apk && fs::is_regular_file(apk, ec));
    check("the bundle is gone", !fs::exists(bundle, ec));
    check("<name>.apk is the installed base.apk", fs::equivalent(apk, copy / "base.apk", ec));
    check("the splits are the installed ones", fs::equivalent(splits / "split_config.arm64_v8a.apk",
                                                              copy / "split_config.arm64_v8a.apk", ec) &&
                                                   fs::equivalent(splits / "split_config.xxhdpi.apk",
                                                                  copy / "split_config.xxhdpi.apk", ec));
    check("one copy: two names each", fs::hard_link_count(copy / "base.apk", ec) == 2 &&
                                          fs::hard_link_count(copy / "split_config.xxhdpi.apk", ec) == 2);
    check("nothing but the APKs in .splits",
          std::distance(fs::directory_iterator(splits), fs::directory_iterator()) == 2);
    check("nothing left half made", !fs::exists(library / "App.apk.new", ec) &&
                                        !fs::exists(library / "App.splits.new", ec));

    // Cut off before the bundle went, with a half-made attempt beside it: done again.
    write(bundle, "the whole bundle");
    write(library / "App.apk.new", "half");
    fs::create_directory(library / "App.splits.new", ec);
    kept = keep_installed(bundle, copy);
    check("redone: the bundle goes", kept == apk && !fs::exists(bundle, ec) &&
                                         !fs::exists(library / "App.apk.new", ec) &&
                                         !fs::exists(library / "App.splits.new", ec));
    check("redone: still one copy", fs::equivalent(apk, copy / "base.apk", ec) &&
                                        fs::hard_link_count(copy / "base.apk", ec) == 2);

    // No installed copy: the bundle stays.
    write(library / "Other.xapk", "other");
    check("no installed copy: the bundle stays", keep_installed(library / "Other.xapk", root / "nowhere").empty() &&
                                                     fs::exists(library / "Other.xapk", ec));

    // The app's entries, and only its own.
    write(library / "App.extra.apk", "x");
    write(library / "Apps.apk", "x");
    write(library / "App.label", "x");
    check("entries: the .apk and .splits only", app_entries(library, L"App", false).size() == 2);
    check("entries: none set aside", app_entries(library, L"App", true).empty());
    check("put back with nothing set aside: no", !put_back(library, L"App"));

    // An update that goes through: the kept files set aside, never written, and dropped.
    check("set aside", set_aside(library, L"App") && !fs::exists(apk, ec) && !fs::exists(splits, ec) &&
                           fs::exists(library / "App.apk.previous", ec) &&
                           fs::exists(library / "App.splits.previous", ec));
    write(apk, "base v2");  // a plain APK's update
    check("the installed copy was not written", read(copy / "base.apk") == "base v1");
    drop_previous(library, L"App");
    check("kept: the .previous ones go", app_entries(library, L"App", true).empty() && read(apk) == "base v2" &&
                                             !fs::exists(splits, ec));
    check("kept: the installed copy is no longer linked", fs::hard_link_count(copy / "base.apk", ec) == 1);

    // An update that does not go through: what replaced the app goes, and the app is back.
    fs::remove(apk, ec);
    write(bundle, "bundle v1");
    kept = keep_installed(bundle, copy);
    check("set aside again", set_aside(library, L"App"));
    write(library / "App.apks", "bundle v2, half copied");
    check("put back", put_back(library, L"App"));
    check("put back: the new file is gone", !fs::exists(library / "App.apks", ec));
    check("put back: the kept files are back, still the installed copy",
          fs::equivalent(apk, copy / "base.apk", ec) &&
              fs::equivalent(splits / "split_config.xxhdpi.apk", copy / "split_config.xxhdpi.apk", ec) &&
              app_entries(library, L"App", true).empty());

    // A bundle of one APK: no .splits folder.
    const fs::path single = root / "android" / "app" / "com.single-Qm";
    fs::create_directories(single);
    write(single / "base.apk", "single");
    write(library / "Single.apkm", "bundle");
    kept = keep_installed(library / "Single.apkm", single);
    check("one APK: <name>.apk and no .splits", kept == library / "Single.apk" &&
                                                    !fs::exists(library / "Single.splits", ec) &&
                                                    !fs::exists(library / "Single.apkm", ec));
    fs::remove_all(root, ec);
}

}  // namespace

void test_maintenance() {
    std::printf("background updates' maintenance window\n");
    namespace mw = winapk::launcher::maintenance;
    using D = mw::Decision;
    mw::Conditions ok;
    ok.setting_on = ok.apps_ready = ok.on_mains = ok.on_network = true;
    const int64_t now = 1'800'000'000, hour = 3600;
    check("never run: due at once (the launcher's start)", mw::decide(now, 0, ok) == D::kStart);
    check("four hours after the last: due", mw::decide(now, now - 4 * hour, ok) == D::kStart);
    check("a minute short of four hours: not due", mw::decide(now, now - 4 * hour + 60, ok) == D::kNotDue);
    check("just run: not due", mw::decide(now, now, ok) == D::kNotDue);
    check("the clock went back past the last: due", mw::decide(now, now + hour, ok) == D::kStart);
    mw::Conditions c = ok;
    c.setting_on = false;
    check("setting off: nothing", mw::decide(now, 0, c) == D::kOff);
    c = ok;
    c.apps_ready = false;
    check("Google's three apps not all installed: nothing", mw::decide(now, 0, c) == D::kOff);
    c = ok;
    c.window_running = true;
    check("a window this launcher opened runs: not a second", mw::decide(now, 0, c) == D::kRunning);
    c = ok;
    c.broker_running = true;
    check("a broker runs (an app's, another launcher's): not a second", mw::decide(now, 0, c) == D::kRunning);
    c = ok;
    c.on_mains = false;
    check("on battery: wait", mw::decide(now, 0, c) == D::kWait);
    check("on battery, not due: not due", mw::decide(now, now, c) == D::kNotDue);
    c = ok;
    c.on_network = false;
    check("no network: wait", mw::decide(now, 0, c) == D::kWait);
    c = ok;
    c.app_starting = true;
    check("an app starting: wait", mw::decide(now, 0, c) == D::kWait);
    check("off wins over everything", mw::decide(now, 0, mw::Conditions{}) == D::kOff);
    c = ok;
    c.closed_by_user = true;
    check("the user closed the library: no window", mw::decide(now, 0, c) == D::kClosed);
    c.window_running = true;
    check("the user closed the library while one runs: not a second", mw::decide(now, 0, c) == D::kClosed);
    c.setting_on = false;
    check("closed, setting off: off", mw::decide(now, 0, c) == D::kOff);
    check("never shown, a window runs: the launcher waits for it", mw::wait_for_window(true, false));
    check("library closed by the user, a window runs: quit, ending it", !mw::wait_for_window(true, true));
    check("no window: nothing to wait for", !mw::wait_for_window(false, false) && !mw::wait_for_window(false, true));
}

void test_install_sets() {
    std::printf("A batch's APK files as installs\n");
    using winapk::launcher::ApkPart;
    using winapk::launcher::InstallSet;
    const std::vector<ApkPart> parts = winapk::launcher::parse_apk_set(
        L"com.paamonim\tconfig.arm64_v8a\tC:\\d\\com.paamonim.config.arm64_v8a.apk\r\n"
        L"com.other\t\tC:\\d\\other.apk\n"
        L"com.paamonim\t\tC:\\d\\com.paamonim.apk\n"
        L"\t\tC:\\d\\broken.apk\n"
        L"com.paamonim\tconfig.xxhdpi\tC:\\d\\dpi.apk\n"
        L"not a line\n");
    check("--set: five lines read, the one that is not skipped", parts.size() == 5);
    check("--set: a line's fields", parts.size() == 5 && parts[0].package == L"com.paamonim" &&
                                        parts[0].split == L"config.arm64_v8a" &&
                                        parts[0].file == L"C:\\d\\com.paamonim.config.arm64_v8a.apk");
    const std::vector<InstallSet> sets = winapk::launcher::group_install_sets(parts);
    check("three installs: the app, the other app, the unreadable file", sets.size() == 3);
    check("the app: its base kept as the app, the split first given second",
          sets.size() == 3 && sets[0].base == L"C:\\d\\com.paamonim.apk" && sets[0].splits.size() == 2 &&
              sets[0].splits[0].first == L"C:\\d\\com.paamonim.config.arm64_v8a.apk" &&
              sets[0].splits[0].second == L"split_config.arm64_v8a.apk" &&
              sets[0].splits[1].second == L"split_config.xxhdpi.apk");
    check("the other app and the unreadable file alone", sets.size() == 3 && sets[1].base == L"C:\\d\\other.apk" &&
                                                            sets[1].splits.empty() &&
                                                            sets[2].base == L"C:\\d\\broken.apk");
    // Splits alone: still one install, its first file as the app's .apk (AppInstall refuses it, as Android does).
    const std::vector<InstallSet> lone = winapk::launcher::group_install_sets(
        {{L"a.apk", L"com.x", L"config.arm64_v8a"}, {L"b.apk", L"com.x", L"config.arm64_v8a"},
         {L"c.apk", L"com.x", L"..\\evil"}});
    check("no base: one install, the first file kept as the app", lone.size() == 1 && lone[0].base == L"a.apk");
    check("a name taken gets a number; an unsafe split name is not a file name",
          lone.size() == 1 && lone[0].splits.size() == 2 && lone[0].splits[0].second == L"split_config.arm64_v8a.apk" &&
              lone[0].splits[1].second == L"split_2.apk");
}

int main() {
    test_runs();
    test_install_sets();
    test_installs();
    test_updates();
    test_update_integrity();
    test_store_setup();
    test_debug_threads();
    test_library_files();
    test_maintenance();
    std::printf("%d failed\n", g_failures);
    return g_failures ? 1 : 0;
}
