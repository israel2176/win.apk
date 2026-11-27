// android.os.Process -- Linux process / thread priorities. v2 has one
// process; we map myPid/myUid to constants and ignore priority calls.
package android.os;

public class Process {
    public static final int SYSTEM_UID    = 1000;
    public static final int FIRST_APPLICATION_UID = 10000;
    public static final int PHONE_UID     = 1001;
    public static final int SHELL_UID     = 2000;
    public static final int LAST_APPLICATION_UID = 19999;

    public static final int THREAD_PRIORITY_DEFAULT     = 0;
    public static final int THREAD_PRIORITY_LOWEST      = 19;
    public static final int THREAD_PRIORITY_BACKGROUND  = 10;
    public static final int THREAD_PRIORITY_FOREGROUND  = -2;
    public static final int THREAD_PRIORITY_DISPLAY     = -4;
    public static final int THREAD_PRIORITY_URGENT_DISPLAY = -8;
    public static final int THREAD_PRIORITY_MORE_FAVORABLE = -1;
    public static final int THREAD_PRIORITY_LESS_FAVORABLE = 1;
    // AOSP core/java/android/os/Process.java -- values verbatim.
    public static final int THREAD_PRIORITY_VIDEO        = -10;
    public static final int THREAD_PRIORITY_AUDIO        = -16;
    public static final int THREAD_PRIORITY_URGENT_AUDIO = -19;

    // AOSP :291-:317 / uid + pid sentinels.
    public static final int INVALID_UID   = -1;
    public static final int INVALID_PID   = -1;
    public static final int ROOT_UID      = 0;
    public static final int WIFI_UID      = 1010;
    public static final int BLUETOOTH_UID = 1002;
    // AOSP FIRST/LAST_SDK_SANDBOX_UID. Public in the API-36 surface only
    // indirectly (via the sandbox-uid helpers below), but the helpers need
    // them and they are the same literals upstream.
    static final int FIRST_SDK_SANDBOX_UID = 20000;
    static final int LAST_SDK_SANDBOX_UID  = 29999;

    // Signal numbers, AOSP verbatim (these are the POSIX values, identical on
    // Darwin: SIGQUIT 3, SIGKILL 9, SIGUSR1 10).
    public static final int SIGNAL_QUIT = 3;
    public static final int SIGNAL_KILL = 9;
    public static final int SIGNAL_USR1 = 10;

    public static int  myPid()  { return (int) ProcessHandle.current().pid(); }
    /** Linux gives a process's main thread the process's own id as its tid, and apps rely on it:
     *  `Process.myPid() == Process.myTid()` is how a packer tells it runs on the main thread (clalit's
     *  loaded its native library only then, and died on UnsatisfiedLinkError, issue #208). The main
     *  looper's thread answers myPid() here too; another thread its Java thread id. Not pinning a
     *  main thread: getMainLooper() is asked only once one is registered. */
    public static int  myTid()  {
        Thread current = Thread.currentThread();
        if (Looper.macapkHasMainThread() && Looper.getMainLooper().getThread() == current) return myPid();
        return (int) current.getId();
    }
    /** This app's uid. With the Mac.apk system broker on, each package has its
     *  own (com.macapk.loader.AppUids, as PMS assigns them) so another process
     *  can tell callers apart; otherwise the one app uid, as before. */
    public static int  myUid()  {
        int u = com.macapk.loader.AppUids.selfUid();
        return u > 0 ? u : FIRST_APPLICATION_UID;
    }
    public static UserHandle myUserHandle() { return UserHandle.CURRENT; }
    // AOSP Process.isApplicationUid(uid) delegates to UserHandle.isApp(uid):
    // true if the uid's app-id falls in [FIRST_APPLICATION_UID, LAST_APPLICATION_UID]
    // (frameworks/base/core/java/android/os/{Process,UserHandle}.java). Mac.apk
    // runs the app at FIRST_APPLICATION_UID, so app uids qualify. (Google
    // Calculator calls this in its Application.onCreate.)
    //
    // This used to omit the upper bound (and the uid > 0 guard), so any uid
    // >= LAST_APPLICATION_UID (19999) was silently misclassified as an app uid
    // -- never observed against our own single uid=10000, but wrong for any
    // caller that probes a synthesized/foreign uid.
    public static boolean isApplicationUid(int uid) {
        return UserHandle.isApp(uid);
    }

    // Per AOSP frameworks/base/core/java/android/os/Process.java,
    // isIsolated() == isIsolatedUid(myUid()), where the isolated_app
    // UID range is [99000, 99999] (FIRST_ISOLATED_UID..LAST_ISOLATED_UID)
    // or the app-zygote isolated range. Mac.apk runs the app in our
    // host process (not an Android isolated_app sandbox) and myUid()
    // returns FIRST_APPLICATION_UID (10000), which is outside both
    // ranges -- so the canonical AOSP-equivalent value is always false.
    // Added because Google Calculator's CalculatorApplication.onCreate
    // routes through a$$ExternalSyntheticApiModelOutline2.m which calls
    // android.os.Process.isIsolated() -- absent, it threw
    // NoSuchMethodError before reaching any UI.
    public static final boolean isIsolated() { return false; }

    // Companion for callers that pass a UID directly. Same reasoning:
    // FIRST_APPLICATION_UID (10000) and any normal-app UID never falls
    // in the [99000, 99999] isolated range, so this is always false in
    // our single-host-process model.
    public static final boolean isIsolatedUid(int uid) { return false; }

    // Android nice-levels (-20..19) do not map onto macOS scheduling in a way
    // that helps us: Mac.apk manages the UI thread's QoS natively (USER_INTERACTIVE,
    // native_methods.cpp) to keep the frame clock from starving, and honoring an
    // app's request to *lower* a background worker's nice level on macOS would risk
    // re-introducing exactly the coroutine-worker starvation we fixed. So we do not
    // change the OS scheduling here. We DO, however, record the last requested
    // priority per tid and return it from getThreadPriority, because some libraries
    // (and AOSP's own StrictMode/JankTracker probes) set-then-read-back their thread
    // priority and assert the value round-trips; a flat DEFAULT broke that contract.
    private static final java.util.concurrent.ConcurrentHashMap<Integer, Integer> sThreadPrio =
            new java.util.concurrent.ConcurrentHashMap<>();
    public static int  getThreadPriority(int tid) {
        Integer p = sThreadPrio.get(tid);
        return p != null ? p : THREAD_PRIORITY_DEFAULT;
    }
    public static void setThreadPriority(int priority) {
        sThreadPrio.put(myTid(), priority);
    }
    public static void setThreadPriority(int tid, int priority) {
        sThreadPrio.put(tid, priority);
    }
    /** AOSP android_os_Process_getElapsedCpuTime: the process's CPU time in ms
     *  (clock_gettime(CLOCK_PROCESS_CPUTIME_ID)), never 0 in a running app. Was a
     *  constant 0: AppsFlyer's obfuscated loader (bit) adds getElapsedCpuTime() > 0
     *  into the length of a class name it decrypts, and looked up
     *  "...AFi1fSDKT861" for "...AFi1fSDKT8615". The JVM's own count of the
     *  process's CPU time (GetProcessTimes on Windows); the uptime when the JVM
     *  has none, which keeps it positive. */
    public static long getElapsedCpuTime() {
        try {
            java.lang.management.OperatingSystemMXBean b =
                    java.lang.management.ManagementFactory.getOperatingSystemMXBean();
            if (b instanceof com.sun.management.OperatingSystemMXBean) {
                long ns = ((com.sun.management.OperatingSystemMXBean) b).getProcessCpuTime();
                if (ns >= 0) return ns / 1_000_000L;
            }
        } catch (Throwable ignored) {
            // no jdk.management: the fallback below
        }
        return SystemClock.uptimeMillis();
    }

    // AOSP core/java/android/os/Process.java :800-810 -- these two fields plus
    // setStartTimes(), set from ActivityThread.handleBindApplication (L7420)
    // before any app code runs: "sStartElapsedRealtime"/"sStartUptimeMillis",
    // read back by getStartElapsedRealtime()/getStartUptimeMillis(). Under
    // MACAPK_GUEST_CLOCK_NANOTIME (default ON) SystemClock.uptimeMillis() is
    // host-uptime sized (2.18e9 ms on a 25-day-up host), so an app-start
    // instrumentation call computing `SystemClock.uptimeMillis() -
    // Process.getStartUptimeMillis()` needs a REAL start stamp here or it
    // reads a bogus multi-day cold start; the old hardcoded 0 only happened to
    // work because SystemClock itself used to be process-relative (0 ~= start).
    // Set once, from com/macapk/loader/Boot.java at the handleBindApplication-
    // ordered point (before makeApplicationInner / installContentProviders),
    // never re-armed later in the process (matches the one-shot AOSP setter).
    private static volatile long sStartElapsedRealtime;
    private static volatile long sStartUptimeMillis;

    /** @hide Boot.java's ordering surrogate for
     *  ActivityThread.handleBindApplication's own setStartTimes() call.
     *  Under MACAPK_GUEST_CLOCK_NANOTIME=0 this is a deliberate no-op: keep
     *  returning 0 there so the =0 arm stays byte-identical to the pre-2026-09-28
     *  tree (0 ~= "process start" when SystemClock was itself process-relative). */
    public static void setStartTimes(long elapsedRealtime, long uptimeMillis) {
        sStartElapsedRealtime = elapsedRealtime;
        sStartUptimeMillis = uptimeMillis;
    }
    public static long getStartElapsedRealtime() { return sStartElapsedRealtime; }
    public static long getStartUptimeMillis() { return sStartUptimeMillis; }
    // AOSP :822/:835 -- "the elapsedRealtime()/uptimeMillis() at which the
    // system was about to start this process, i.e. BEFORE a zygote fork".
    // There is no zygote here: the request to start the app and the start
    // itself are the same instant, so these must agree with
    // getStartElapsedRealtime()/getStartUptimeMillis() above rather than
    // carry an independently-invented value. AOSP guarantees
    // requested <= actual, which equality satisfies.
    public static long getStartRequestedElapsedRealtime() { return getStartElapsedRealtime(); }
    public static long getStartRequestedUptimeMillis() { return getStartUptimeMillis(); }

    /** AOSP :1323 -- `return true;` verbatim (a legacy probe from the
     *  single-process-Android days). */
    public static final boolean supportsProcesses() { return true; }

    /** AOSP :1222 -- native. Documented: "The array will have length zero if
     *  no CPU cores are exclusively reserved for this process at this point in
     *  time." macOS reserves no core for a process, so length zero is the
     *  device-accurate answer, not a placeholder. */
    public static final int[] getExclusiveCores() { return new int[0]; }

    /** AOSP :961 -- `uid = UserHandle.getAppId(uid); return uid >=
     *  FIRST_SDK_SANDBOX_UID && uid <= LAST_SDK_SANDBOX_UID;` Ported
     *  verbatim; consistent with isSdkSandbox() below, which is false because
     *  myUid() is an application uid. */
    public static final boolean isSdkSandboxUid(int uid) {
        uid = UserHandle.getAppId(uid);
        return (uid >= FIRST_SDK_SANDBOX_UID && uid <= LAST_SDK_SANDBOX_UID);
    }

    /** AOSP :977 -- verbatim, including the IllegalArgumentException. */
    public static final int getAppUidForSdkSandboxUid(int uid) {
        if (!isSdkSandboxUid(uid)) {
            throw new IllegalArgumentException("Input UID is not an SDK sandbox UID");
        }
        return uid - (FIRST_SDK_SANDBOX_UID - FIRST_APPLICATION_UID);
    }

    /** AOSP :1009 -- verbatim, including the IllegalArgumentException. */
    public static final int getSdkSandboxUidForAppUid(int uid) {
        if (!isApplicationUid(uid)) {
            throw new IllegalArgumentException("Input UID is not an app UID");
        }
        return uid + (FIRST_SDK_SANDBOX_UID - FIRST_APPLICATION_UID);
    }
    public static boolean is64Bit() { return true; }
    /** AOSP android_os_Process_getUidForName / getGidForName (core/jni/
     *  android_util_Process.cpp): a null name throws NullPointerException; a name
     *  of digits only is atoi'd; any other name goes to bionic's getpwnam /
     *  getgrnam, which know only the AIDs (android_filesystem_config.h) and the
     *  per-user names u<user>_a<n>, u<user>_i<n>, u<user>_<aid>; an unknown or
     *  empty name is -1. Was 0 for every name: SDKs that fold
     *  getGidForName("") into their arithmetic as a constant -1 (AppsFlyer's
     *  obfuscated loader, in bit) computed another value than on a phone. */
    public static int  getUidForName(String name) { return idForName(name); }
    public static int  getGidForName(String name) { return idForName(name); }

    private static int idForName(String name) {
        if (name == null) throw new NullPointerException();
        int n = name.length();
        if (n == 0) return -1;
        boolean digits = true;
        for (int i = 0; i < n && digits; i++) digits = name.charAt(i) >= '0' && name.charAt(i) <= '9';
        if (digits) {
            // bionic's atoi is (int) strtol: saturated at LONG_MAX, then truncated.
            long v = 0;
            for (int i = 0; i < n; i++) {
                int d = name.charAt(i) - '0';
                if (v > (Long.MAX_VALUE - d) / 10) { v = Long.MAX_VALUE; break; }
                v = v * 10 + d;
            }
            return (int) v;
        }
        Integer aid = ANDROID_IDS.get(name);
        if (aid != null) return aid;
        return appIdForName(name);
    }

    /** bionic's android_ids (from android_filesystem_config.h): the system ids
     *  whose number has been stable since the early releases. */
    private static final java.util.Map<String, Integer> ANDROID_IDS = new java.util.HashMap<>();
    static {
        String[] names = { "root", "system", "radio", "bluetooth", "graphics", "input", "audio",
            "camera", "log", "compass", "mount", "wifi", "adb", "install", "media", "dhcp",
            "sdcard_rw", "vpn", "keystore", "usb", "drm", "mdnsr", "gps" };
        int[] ids = { 0, 1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 1011,
            1012, 1013, 1014, 1015, 1016, 1017, 1018, 1019, 1020, 1021 };
        for (int i = 0; i < names.length; i++) ANDROID_IDS.put(names[i], ids[i]);
        ANDROID_IDS.put("media_rw", 1023);
        ANDROID_IDS.put("mtp", 1024);
        ANDROID_IDS.put("drmrpc", 1026);
        ANDROID_IDS.put("nfc", 1027);
        ANDROID_IDS.put("sdcard_r", 1028);
        ANDROID_IDS.put("clat", 1029);
        ANDROID_IDS.put("shell", 2000);
        ANDROID_IDS.put("cache", 2001);
        ANDROID_IDS.put("diag", 2002);
        ANDROID_IDS.put("net_bt_admin", 3001);
        ANDROID_IDS.put("net_bt", 3002);
        ANDROID_IDS.put("inet", 3003);
        ANDROID_IDS.put("net_raw", 3004);
        ANDROID_IDS.put("net_admin", 3005);
        ANDROID_IDS.put("net_bw_stats", 3006);
        ANDROID_IDS.put("net_bw_acct", 3007);
        ANDROID_IDS.put("readproc", 3009);
        ANDROID_IDS.put("wakelock", 3010);
        ANDROID_IDS.put("everybody", 9997);
        ANDROID_IDS.put("misc", 9998);
        ANDROID_IDS.put("nobody", 9999);
    }

    /** bionic app_id_from_name: "u<user>_a<n>" is user * 100000 + 10000 + n,
     *  "u<user>_i<n>" an isolated process (90000 + n), "u<user>_<aid>" that
     *  AID in the user; anything else -1. */
    private static int appIdForName(String name) {
        if (name.length() < 4 || name.charAt(0) != 'u') return -1;
        int us = name.indexOf('_');
        if (us < 2) return -1;
        int user;
        try { user = Integer.parseInt(name.substring(1, us)); } catch (NumberFormatException e) { return -1; }
        if (user < 0 || user > 21473) return -1;
        String rest = name.substring(us + 1);
        int appid = -1;
        if (rest.length() > 1 && (rest.charAt(0) == 'a' || rest.charAt(0) == 'i')
                && rest.substring(1).chars().allMatch(c -> c >= '0' && c <= '9') && rest.length() <= 10) {
            long nr = Long.parseLong(rest.substring(1));
            if (rest.charAt(0) == 'a' && nr <= LAST_APPLICATION_UID - FIRST_APPLICATION_UID) {
                appid = (int) (FIRST_APPLICATION_UID + nr);
            } else if (rest.charAt(0) == 'i' && nr <= 99999 - 90000) {
                appid = (int) (90000 + nr);
            }
        } else {
            Integer aid = ANDROID_IDS.get(rest);
            if (aid != null) appid = aid;
        }
        return appid < 0 ? -1 : user * 100000 + appid;
    }
    // ---- self-kill: the app asking for its OWN process to end ---------------
    //
    // AOSP, frameworks/base/core/java/android/os/Process.java (killProcess is
    // literally two lines, and the javadoc is the whole contract):
    //   /** Kill the process with the given PID. Note that, though this API
    //    * allows us to request to kill any process based on its PID, the
    //    * kernel will still impose standard restrictions on which PIDs you
    //    * are actually able to kill. Typically this means only the process
    //    * running the caller's packages/application and any additional
    //    * processes created by that app ... */
    //   public static final void killProcess(int pid) {
    //       sendSignal(pid, SIGNAL_KILL);
    //   }
    //   public static final native void sendSignal(int pid, int signal);
    // https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/master/core/java/android/os/Process.java
    //
    // SIGKILL is uncatchable, so on a device the effect is absolute and
    // immediate: every activity, receiver, service, thread, coroutine scope and
    // in-memory cache in that process ceases to exist. onDestroy is NOT called
    // (Android documents it as not guaranteed for exactly this reason), and a
    // BroadcastReceiver halfway through onReceive simply stops mid-method. What
    // SURVIVES is anything already handed to a DIFFERENT process: a startActivity
    // request already delivered to the ActivityManager still starts, in a brand
    // new process. That asymmetry -- my state dies, my pending launch lives -- is
    // the entire basis of the "restart myself" idiom, and it is what apps that
    // call this are relying on.
    //
    // WHY WE CANNOT DO THE LITERAL THING. Mac.apk hosts the guest IN the runtime
    // process by design (no emulation, no virtualization, no Zygote), so there is
    // no process to kill that is not also ours. Actually leaving would take the
    // runtime down, and the only sanctioned exit is macapk_terminate_now (see
    // cocoa_bridge.mm: System.exit/Runtime.halt both reach libc exit ->
    // __cxa_finalize, and a guest static destructor that re-enters JNI then
    // deadlocks against JavaThread::block_if_vm_exited).
    //
    // WHAT WE DO INSTEAD, and why this is the honest half rather than a lie: the
    // half of the contract we CAN honour is "everything this process was running
    // is gone". ActivityHost.macapkTerminate drives every activity on the stack
    // through the documented teardown (onPause/onStop/onDestroy plus the androidx
    // ON_DESTROY that clears ViewModelStores and tears down Compose recomposers)
    // and empties the stack. Anything the app has already POSTED -- an activity
    // launch queued on the main Looper, or one it starts on the next line -- still
    // runs, and lands in an empty stack, which is precisely what a device gives
    // it: a fresh process with a fresh task. The irreducible divergence is that
    // the calling thread keeps executing past this call; on a device it would not.
    // For the dominant restart idiom that divergence is actually correct, because
    // there the caller lives in a SEPARATE process that the kill does not touch
    // (JakeWharton's ProcessPhoenix runs its PhoenixActivity under
    // android:process=":phoenix", kills the main pid, then calls startActivities
    // from the surviving process).
    //
    // WHAT THIS FIXES. Without it, killProcess was a no-op, so a restart request
    // stacked a SECOND copy of the app's root activity on top of the first, still
    // live and merely paused -- two activity instances, two renderers bound to one
    // window, and a composition that is not the one being presented. Witnessed on
    // Aurora Store 4.8.4 finishing onboarding (two Phoenix rounds, ending on a
    // black screen). This is not keyed to that app: the same shape appears
    // wherever the idiom is used.
    //
    // BLAST RADIUS is real, which is why the gate exists. Of the installed
    // corpus: Aurora Store and NewPipe ship ProcessPhoenix; F-Droid and NewPipe
    // ship ACRA (ProcessFinisher / ReportBuilder kill the process after a crash
    // report); VLC's UiTools.restartDialog kills with NO restart intent at all
    // (on a device the window simply disappears -- here the window would go
    // empty); AnkiDroid kills from a delayed background thread; Duolingo's
    // UnityPlayer kills on quit; and several games (Subway Surfers, Hill Climb,
    // Geometry Dash, Fruit Ninja, Jetpack Joyride) kill on their own exit path.
    //
    // GATE: MACAPK_PROCESS_SELF_KILL=1. Default OFF -- with it off both methods
    // are the empty bodies they have always been. RETIRE CONDITION: an
    // interleaved protected-sweep + fleet A/B showing no regression on the apps
    // above, after which this becomes unconditional and the flag is deleted.


    /** AOSP :1020 -- `sendSignal(pid, SIGNAL_KILL);`, verbatim. */
    public static void killProcess(int pid) { sendSignal(pid, SIGNAL_KILL); }

    public static void sendSignal(int pid, int signal) {
        return;
    }

    /** AOSP Process.isSdkSandbox (API 34): whether the caller runs in the SDK
     *  sandbox. Mac.apk loads SDKs in-process, so false. */
    public static boolean isSdkSandbox() { return false; }

    /** AOSP Process.myProcessName (API 33): ActivityThread.currentProcessName(),
     *  i.e. the package name in a package's default process and the real
     *  `<pkg>:name` in one the broker spawned for another android:process. */
    public static String myProcessName() { return android.app.ActivityThread.currentProcessName(); }
}


