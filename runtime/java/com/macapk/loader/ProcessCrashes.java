// ProcessCrashes -- the system broker's record of the processes that crashed, as
// ActivityManagerService's AppErrors keeps it (Win.apk, 2026-11-26).
//
// On a device a process that crashes again within MIN_CRASH_INTERVAL of its last crash "has crashed
// too many times": AppErrors.handleAppCrashLocked marks it bad and removes it, so that its services
// are not restarted, and ProcessList.startProcessLocked silently refuses a start of a bad process that
// does not come from the user. The user's own start of the app clears it (resetProcessCrashTime,
// clearBadProcess), as a force stop clears the package's crash times. Without this the broker started
// Play services again for every bind the Play Store made while Play services crashed as it started:
// 276 crashes in one log, each a JVM booted for nothing.
//
// The broker only starts a process for someone else (a bind, a start, a provider, a broadcast, a job,
// the boot, a revival): every one of its starts is a start "from the background" in AMS's sense. The
// user's start is the launcher's, which the broker sees as an app it did not start attaching.
//
// Only the policy, with no process or clock of its own, so a test drives it (ProcessCrashesTest).
package com.macapk.loader;

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

final class ProcessCrashes {
    /** ActivityManagerConstants.MIN_CRASH_INTERVAL in AOSP 14: two minutes. */
    static final long MIN_CRASH_INTERVAL_MS = 2 * 60_000L;

    /** Last crash time by process ("pkg/proc"), and the processes that are bad. */
    private final Map<String, Long> lastCrash = new HashMap<>();
    private final Set<String> bad = new HashSet<>();

    static String key(String pkg, String proc) { return pkg + "/" + proc; }

    /** Whether a process's exit code is a crash as Android counts one: the runtime's crash handler
     *  ended it (winapk::kExitAppCrashed), abort() did (3), or a native fault did (an NTSTATUS
     *  exception code, but not a DLL that would not load, which is Win.apk's install broken).
     *  The same rule as windows/include/winapk/app_exit.h app_crashed. */
    static boolean crashExit(int exitValue) {
        long code = exitValue & 0xffffffffL;
        if (code == 0x1000000AL || code == 3) return true;
        if (code < 0xC0000000L) return false;
        return code != 0xC0000135L && code != 0xC0000139L && code != 0xC000007BL && code != 0xC0000142L;
    }

    /** A process crashed at `now`. True when that made it bad: it had crashed less than
     *  MIN_CRASH_INTERVAL before. */
    synchronized boolean crashed(String pkg, String proc, long now) {
        String k = key(pkg, proc);
        Long last = lastCrash.put(k, now);
        if (last != null && now - last < MIN_CRASH_INTERVAL_MS && bad.add(k)) return true;
        return false;
    }

    /** Whether a start of `proc` that is not the user's must be refused. */
    synchronized boolean isBad(String pkg, String proc) { return bad.contains(key(pkg, proc)); }

    /** The user started the process (opened the app): its crash time and badness go. */
    synchronized void userStarted(String pkg, String proc) {
        String k = key(pkg, proc);
        lastCrash.remove(k);
        bad.remove(k);
    }

    /** The package was force-stopped, updated or removed: every process of it starts clean. */
    synchronized void forgetPackage(String pkg) {
        String prefix = pkg + "/";
        lastCrash.keySet().removeIf(k -> k.startsWith(prefix));
        bad.removeIf(k -> k.startsWith(prefix));
    }
}
