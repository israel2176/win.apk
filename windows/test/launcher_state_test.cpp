// launcher_state_test.cpp -- winapk-launcher-state-test: the launcher's state machines
// (launcher/app_runs.h), which hold no lock and make no Windows call, driven step by step as the
// library's thread and a worker's thread drive them.
//
//   winapk-launcher-state-test
//
// An app's run: each path from start to end says the right thing (nothing, the splash goes, "didn't
// start", "stopped with an error"); Stop at each point of a run; steps out of order change nothing.

#include <cstdio>
#include <string>
#include <vector>

#include "../launcher/app_runs.h"

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
        check("exit code 0 before its window: did not start, cleanly",
              clean && clean->kind() == RunEnd::kDidNotStart && clean->clean);
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
}

}  // namespace

int main() {
    test_runs();
    std::printf("%d failed\n", g_failures);
    return g_failures ? 1 : 0;
}
