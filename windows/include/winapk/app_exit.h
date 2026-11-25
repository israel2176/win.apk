// app_exit.h -- what an app's run tells by its exit code beyond 0 and not 0.
//
// winapk-runtime.exe ends an app's run with one of these when Android's system would show the user
// something about the app's end: the launcher reads them (launcher/app_runs.h) and shows Android's
// dialog. The Java side has the same numbers (Boot.CRASH_EXIT, InputTimeout.CLOSED_EXIT).

#pragma once

namespace winapk {

// The framework's crash handler ended the run: an exception no one caught, on any of the app's threads
// (RuntimeInit's KillApplicationHandler, which on a device reports the crash to ActivityManager, then
// kills the process and exits with 10).
constexpr unsigned long kExitAppCrashed = 0x1000000AUL;

// The user chose Close app in the "isn't responding" dialog the runtime asked (ActivityManager kills an
// app that is not responding with SIGKILL, 9, when Close app is chosen).
constexpr unsigned long kExitClosedNotResponding = 0x10000009UL;

// Whether a run that ended with `code` crashed as Android counts a crash: the framework's handler ended
// it, or a native fault did, which on a device debuggerd reports to the same handleApplicationCrash:
// an NTSTATUS exception code (0xC...) or abort() (3). A DLL that would not load is Win.apk's own install
// broken, not the app: those say so themselves (the launcher's runtime_failure_reason).
constexpr bool app_crashed(unsigned long code) {
    if (code == kExitAppCrashed || code == 3) return true;
    if (code < 0xC0000000UL) return false;
    return code != 0xC0000135UL && code != 0xC0000139UL && code != 0xC000007BUL && code != 0xC0000142UL;
}

}  // namespace winapk
