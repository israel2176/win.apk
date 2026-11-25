// com.macapk.loader.SystemServer -- the Mac.apk system broker: the one process
// every app finds by name, standing in for the parts of Android's
// system_server (ActivityManagerService + PackageManagerService) that let one
// app reach another app's components.
//
// WHY IT EXISTS. Each Mac.apk app runs in its own process (owner decision
// 2026-10-30), and Binder crosses processes over Mach ports
// (android.os.MacApkBinderTransport). What a process cannot do alone is FIND
// another app's service, or start that app when it is not running. On a device
// ActivityManagerService does both: bindService resolves the Intent against the
// installed packages' manifests, starts the target's process if needed, waits
// for it to attach, schedules the bind there, and hands the IBinder back. This
// is that, for Mac.apk. It is how the real Play Store reaches the real Google
// Play services: nothing is synthesized, each package serves its own binders.
//
// THE PROTOCOL (descriptor android.app.IMacApkSystem):
//   REGISTER(pkg, IBinder appThread, apk)   an app process attaches (AMS
//                                           attachApplication); death of its
//                                           appThread unregisters it
//   BIND(callerPkg, Intent) -> ok, ComponentName, IBinder
//                                           AMS bindService: resolve, enforce
//                                           android:exported, start the target
//                                           process on demand, schedule the bind
//   RESOLVE(callerPkg, Intent) -> answer, component
//                                           the synchronous half of bindService:
//                                           none / ok / not exported, no process start
//   GET_PROVIDER(callerPkg, authority) -> IBinder
//                                           AMS getContentProvider: the package
//                                           declaring the authority, exported
//                                           check, process started on demand,
//                                           its provider's binder (phase 4)
//   START(callerPkg, Intent)                AMS startService for another app's
//                                           service (resolved first via RESOLVE)
//   BROADCAST(callerPkg, Intent, proc, requiredPermissions) (oneway)
//                                           AMS broadcastIntent across apps:
//                                           every running app's receivers, plus
//                                           a stopped app's manifest receiver
//                                           for an explicit target or a
//                                           boot-time action (started on demand)
//   HAS_PACKAGE(pkg) -> boolean, apkPath    is this package known here, and
//                                           where its installed APK is
//   PROVIDER_PACKAGE(authority) -> pkg      PMS resolveContentProvider: which
//                                           package declares the authority (null:
//                                           none); no process is started
//   CHECK_PROVIDER(pid, uri, write) -> answer, reason
//                                           Win.apk: ContentProvider.Transport's
//                                           enforceRead/WritePermission, asked by
//                                           the provider's own process for its caller
//   CHECK_PERMISSION(perm, pkg) -> granted  PackageManager.checkPermission
//   CHECK_PERMISSION_PID(perm, pid) -> granted
//                                           Context.checkPermission for another process
//   GRANT_URI(target, uri, mode) / REVOKE_URI(target, uri, mode) / CHECK_URI(pid, uri, mode)
//                                           Context.grantUriPermission and its kin
//   PACKAGE_CHANGED(op, pkg, apk, dataRemoved)
//                                           Win.apk: PMS's commit of an install,
//                                           an update or a removal, from the
//                                           launcher only; the broker's package
//                                           table follows it and the PACKAGE_*
//                                           broadcasts go out (packageChanged)
// and each app's thread (android.app.IMacApkApplicationThread):
//   SCHEDULE_BIND(Intent) -> IBinder        run the bind on the app's main
//                                           looper (ActivityThread.handleBindService)
//   ACQUIRE_PROVIDER(authority) -> IBinder  the app's own provider, as a binder
//   SCHEDULE_START(Intent)                  startService on the app's main looper
//   SCHEDULE_BROADCAST(Intent, fromSystem, senderPkg) (oneway)
//                                           deliver to the app's own receivers
//
// PROCESSES. An app is not one process but one per android:process its manifest
// uses, as on a device: the package's default process (its application-level
// android:process, else the package name) plus every ":name" / global name some
// component declares. The broker keys running apps by PROCESS NAME, spawns the
// one a component lives in on demand (MACAPK_PROCESS_NAME tells the new runtime
// which it is, and it reports exactly that from ActivityThread.currentProcessName),
// and routes every bind, start, provider, job and receiver to the process its
// manifest names -- including between two processes of the SAME package, which is
// what lets Finsky's InstallerV2 jobs run in com.android.vending:background.
// See docs/process-model-per-android-process_2026-11-01.md.
//
// BOOT. The broker coming up is this device booting: the headless SYSTEM apps
// that listen for BOOT_COMPLETED are started right away (bootSystemServices),
// and the first time each app attaches, it is sent LOCKED_BOOT_COMPLETED then
// BOOT_COMPLETED when its manifest listens for them (what starts e.g. Google
// Play services' checkin).
//
// RENDEZVOUS. The broker claims a launchd-declared Mach name with
// bootstrap_check_in (MACAPK_SYSTEM_NAME, default org.macapk.system); launchd
// starts it on the first message. In the product the name is declared by a
// LaunchAgent registered with SMAppService from inside Mac.apk.app (see
// docs/cross-process-binder-over-mach_2026-10-30.md); in development and in
// the gate, a transient agent loaded with `launchctl bootstrap`.
//
// CONFIG. Product (no variables): the installed apps in the Mac.apk registry,
// each started through its own installed stub; launchd starts the broker from
// Mac.apk.app (Contents/Library/LaunchAgents/org.macapk.system.plist, an
// SMAppService agent the "Use Google GMS" switch registers). Development:
//   MACAPK_BROKER_APKS     ':'-separated APK paths this broker knows (each
//                          package's base.apk; splits beside it are found the
//                          usual way)
//   MACAPK_BROKER_RUNTIME  the macapk-runtime binary used to start an app
//   MACAPK_BROKER_ROOT     the --android-root for started apps
//   MACAPK_BROKER_LOGDIR   where each started app's stderr goes
// The product reads the installed-apps registry and starts each app through its
// own installed stub instead; that lands with the SMAppService/auto-install
// change.
//
// LIMITS (phase 3, stated): no unbind accounting (a service is not onUnbind'd
// when its last remote client leaves); the caller's Android uid is the one its
// header claims (per-package uid assignment checked against the launched pid is
// the follow-up); content providers and broadcasts across packages are phase 4.
//
// WIN.APK (Windows). The broker is `winapk-runtime --system`, started on demand
// by the first app that looks it up (SystemClient) and alone per user session
// (its pipe name is claimed with FILE_FLAG_FIRST_PIPE_INSTANCE). It starts an
// app as the launcher does, winapk-runtime --apk <library file>. And it does not
// trust what a caller says it is: the transport gives the caller's pid from the
// OS, a process is a package only by the broker's own record of it (a process
// it started, or one whose command line Windows recorded names an installed
// APK of that package: WinIdentity), every caller-package argument must match
// that record or the call is refused with a SecurityException and logged, and
// the calling uid every app sees is derived from the same record. Its package
// table is the registry (installed_apps.json) read once as it starts, then
// every install, update and removal the launcher commits while it runs, which
// the launcher tells it (PACKAGE_CHANGED) as PackageManagerService learns a
// package at its commit; it then broadcasts PACKAGE_* to the running apps.
// What that caller may reach is decided as system_server decides it
// (BrokerPermissions): a component's android:permission, a provider's read and
// write permissions, path-permissions and URI grants, a broadcast's required
// permissions both ways, each against the permissions the caller's package
// holds (signature ones by its verified signer).
package com.macapk.loader;

import android.content.ComponentName;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.MacApkBinderTransport;
import android.os.Parcel;
import android.os.RemoteException;

import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

public final class SystemServer {
    public static final String DEFAULT_NAME = "org.macapk.system";
    static final String DESCRIPTOR = "android.app.IMacApkSystem";
    static final String THREAD_DESCRIPTOR = "android.app.IMacApkApplicationThread";
    static final int TX_REGISTER = IBinder.FIRST_CALL_TRANSACTION;
    static final int TX_BIND = IBinder.FIRST_CALL_TRANSACTION + 1;
    static final int TX_HAS_PACKAGE = IBinder.FIRST_CALL_TRANSACTION + 2;
    static final int TX_RESOLVE = IBinder.FIRST_CALL_TRANSACTION + 3;
    /** RESOLVE answers: AMS bindService returns false / throws SecurityException
     *  synchronously, before any process starts, and so must we. */
    static final int RESOLVE_NONE = 0, RESOLVE_OK = 1, RESOLVE_NOT_EXPORTED = 2;
    /** The service requires a permission the caller does not hold; the permission follows. */
    static final int RESOLVE_NO_PERMISSION = 3;
    static final int TX_GET_PROVIDER = IBinder.FIRST_CALL_TRANSACTION + 4;
    static final int TX_START = IBinder.FIRST_CALL_TRANSACTION + 5;
    static final int TX_BROADCAST = IBinder.FIRST_CALL_TRANSACTION + 6;
    /** AMS stopService for a service in another process (answer: was it running). */
    static final int TX_STOP = IBinder.FIRST_CALL_TRANSACTION + 7;
    /** JobSchedulerService, routed to the process that hosts the job's service:
     *  (callerPkg, callerProc, op, args...). A job runs in the service's
     *  android:process, so schedule/enqueue go THERE; cancel/cancelAll/pending
     *  fan out to the app's other processes. */
    static final int TX_JOB = IBinder.FIRST_CALL_TRANSACTION + 8;
    static final int JOB_SCHEDULE = 0, JOB_ENQUEUE = 1, JOB_CANCEL = 2, JOB_CANCEL_ALL = 3, JOB_PENDING = 4;
    /** TX_SCHEDULE_JOB only: run this job now, the broker held it while no process of
     *  the app was alive and it is due (BrokerJobs). */
    static final int JOB_RUN = 5;
    /** JobSchedulerService's store (BrokerJobs), oneway, from the process running the
     *  jobs: (pkg, proc, op, jobId, value, flags, JobInfo or null); SystemClient.reportJob. */
    static final int TX_JOB_REPORT = IBinder.FIRST_CALL_TRANSACTION + 27;
    /** (pkg) -> APP_*: what ActivityManager knows of a package's processes, for
     *  InstallConstraints (SystemClient.appState). */
    static final int TX_APP_STATE = IBinder.FIRST_CALL_TRANSACTION + 28;
    public static final int APP_UNKNOWN = -1, APP_NOT_RUNNING = 0, APP_BACKGROUND = 1, APP_OPEN = 2;
    static final int TX_SCHEDULE_STOP = IBinder.FIRST_CALL_TRANSACTION + 14;
    static final int TX_SCHEDULE_JOB = IBinder.FIRST_CALL_TRANSACTION + 15;
    static final int TX_SCHEDULE_BROADCAST = IBinder.FIRST_CALL_TRANSACTION + 13;
    /** The account service (AccountsService), as ServiceManager hands out "account". */
    static final int TX_GET_ACCOUNT_SERVICE = IBinder.FIRST_CALL_TRANSACTION + 20;
    /** Win.apk: the calling uid of a pid (pid) -> uid, -1 when that process is
     *  no registered app; asked by an app the pid called (SystemClient.uidOfPid). */
    static final int TX_UID_OF_PID = IBinder.FIRST_CALL_TRANSACTION + 21;
    /** ContentService.registerContentObserver, by authority: (pkg, proc,
     *  authority), oneway. */
    static final int TX_OBSERVE = IBinder.FIRST_CALL_TRANSACTION + 22;
    /** ContentService.notifyChange: (pkg, proc, uri), oneway; the broker calls
     *  TX_SCHEDULE_NOTIFY on every other process that observes the authority. */
    static final int TX_NOTIFY_CHANGE = IBinder.FIRST_CALL_TRANSACTION + 23;
    /** PMS resolveContentProvider for another package: (authority) -> the
     *  declaring package or null. Only the lookup; GET_PROVIDER is the acquire. */
    static final int TX_PROVIDER_PACKAGE = IBinder.FIRST_CALL_TRANSACTION + 24;
    /** Win.apk: PackageManagerService's commit, (op, pkg, apk, dataRemoved) -> nothing; only the
     *  launcher (and its install helper, the same program) may call it, by what Windows says the
     *  calling process runs (WinProcesses.isLauncher). windows/include/winapk/package_events.h is
     *  the caller and must agree with these numbers. */
    static final int TX_PACKAGE_CHANGED = IBinder.FIRST_CALL_TRANSACTION + 25;
    static final int PACKAGE_ADDED = 0, PACKAGE_REPLACED = 1, PACKAGE_REMOVED = 2;
    /** Win.apk: ContentProvider.Transport's per-operation check, asked by the process serving the
     *  provider: (callingPid, uri, write) -> BrokerPermissions.OP_*, the denial's tail. */
    static final int TX_CHECK_PROVIDER = IBinder.FIRST_CALL_TRANSACTION + 29;
    /** PackageManager.checkPermission(perm, pkg) -> 1 granted / 0 not. */
    static final int TX_CHECK_PERMISSION = IBinder.FIRST_CALL_TRANSACTION + 30;
    /** ActivityManagerService.checkPermission(perm, pid, uid) for another process: (perm, pid) -> 1
     *  granted / 0 not, for the package the broker knows that process as. */
    static final int TX_CHECK_PERMISSION_PID = IBinder.FIRST_CALL_TRANSACTION + 34;
    /** Context.grantUriPermission(target, uri, mode), by the calling package. */
    static final int TX_GRANT_URI = IBinder.FIRST_CALL_TRANSACTION + 31;
    /** Context.revokeUriPermission([target,] uri, mode), by the provider's owner: (target or null, uri, mode). */
    static final int TX_REVOKE_URI = IBinder.FIRST_CALL_TRANSACTION + 32;
    /** Context.checkUriPermission(uri, pid, uid, mode): (pid, uri, mode) -> URI_GRANTED / URI_DENIED /
     *  URI_UNKNOWN (no installed app's provider: the asking process answers for its own). */
    static final int TX_CHECK_URI = IBinder.FIRST_CALL_TRANSACTION + 33;
    static final int TX_SCHEDULE_NOTIFY = IBinder.FIRST_CALL_TRANSACTION + 16;
    static final int TX_SCHEDULE_BIND = IBinder.FIRST_CALL_TRANSACTION + 10;
    static final int TX_ACQUIRE_PROVIDER = IBinder.FIRST_CALL_TRANSACTION + 11;
    static final int TX_SCHEDULE_START = IBinder.FIRST_CALL_TRANSACTION + 12;

    private static final long START_TIMEOUT_MS =
        Long.getLong("macapk.broker.startTimeoutMs", 180_000L);

    /** A package this broker can start. */
    static final class PackageRecord {
        final String pkg, apkPath;
        final Manifest manifest;
        /** Its verified signers (ApkVerification), read when a permission check first needs them. */
        volatile ApkVerification.Result signers;
        PackageRecord(String pkg, String apkPath, Manifest manifest) {
            this.pkg = pkg; this.apkPath = apkPath; this.manifest = manifest;
        }
    }

    /** A running, attached app process: ONE android:process of a package. */
    static final class AppRecord {
        final String pkg;
        /** The process name it reports from ActivityThread.currentProcessName(). */
        final String proc;
        final IBinder thread;
        final int pid;
        AppRecord(String pkg, String proc, IBinder thread, int pid) {
            this.pkg = pkg; this.proc = proc; this.thread = thread; this.pid = pid;
        }
    }

    private final Map<String, PackageRecord> packages = new java.util.concurrent.ConcurrentHashMap<>();

    /** Tests: a package's signers, instead of verifying its APK. */
    java.util.function.Function<String, ApkVerification.Result> signersOf;

    /** system_server's permission checks over this broker's packages. */
    final BrokerPermissions perms = new BrokerPermissions(new BrokerPermissions.Packages() {
        @Override public Manifest manifest(String pkg) {
            PackageRecord pr = pkg == null ? null : packages.get(pkg);
            return pr == null ? null : pr.manifest;
        }
        @Override public ApkVerification.Result signers(String pkg) {
            if (signersOf != null) return signersOf.apply(pkg);
            PackageRecord pr = pkg == null ? null : packages.get(pkg);
            if (pr == null) return ApkVerification.of(null);
            ApkVerification.Result r = pr.signers;
            if (r == null) pr.signers = r = ApkVerification.of(pr.apkPath);
            return r;
        }
        @Override public boolean privileged(String pkg) {
            PackageRecord pr = pkg == null ? null : packages.get(pkg);
            return pr != null && MacApkPackageManager.platformSystemApk(pr.pkg, pr.apkPath);
        }
        @Override public boolean runtimeGranted(String pkg, Manifest m, String perm) {
            return SpecialAccessStore.runtimeGrantedFor(pkg, m, perm);
        }
        @Override public int uid(String pkg) {
            PackageRecord pr = pkg == null ? null : packages.get(pkg);
            return AppUids.uidFor(pkg, pr != null ? pr.manifest.sharedUserId : null);
        }
        @Override public Iterable<String> all() { return new ArrayList<>(packages.keySet()); }
    });

    /** findProvider as BrokerPermissions asks it: {owning package, entry}. */
    private final BrokerPermissions.ProviderLookup providerLookup = authority -> {
        Object[] hit = findProvider(authority);
        return hit == null ? null : new Object[] { ((PackageRecord) hit[0]).pkg, hit[1] };
    };

    /** How AOSP names a caller in a denial: "(pid=<pid>, uid=<uid>)" for the calling process. */
    String who(int pid) { return "(pid=" + pid + ", uid=" + uidOfPid(pid) + ")"; }

    /** The URI grants an intent the broker hands from `caller` to `target` carries (AMS grants them as
     *  it delivers); a refused one is logged and not made, the delivery goes on. */
    void grantFromIntent(String caller, String target, Intent intent) {
        if (caller == null || target == null || caller.equals(target)) return;
        for (String why : perms.grantFromIntent(caller, target, intent, providerLookup)) {
            log("uri grant " + caller + " -> " + target + " REFUSED: " + why);
        }
    }
    /** Running processes by PROCESS NAME (not package): a package has several. */
    private final Map<String, AppRecord> running = new LinkedHashMap<>();
    private final Map<String, CompletableFuture<AppRecord>> starting = new LinkedHashMap<>();
    /** Processes this broker spawned (service hosts), by pid. Everything else
     *  that attaches is an app the user opened: those keep the broker alive. */
    private final Map<Long, Process> spawned = new java.util.concurrent.ConcurrentHashMap<>();
    /** Win.apk: what each process this broker started IS: {package, process}. */
    private final Map<Long, String[]> spawnedAs = new java.util.concurrent.ConcurrentHashMap<>();
    /** Last time an app the user opened was running (see BrokerIdle). */
    private volatile long lastClientMs = System.currentTimeMillis();
    /** An app the user opened has attached at least once. Until then the broker
     *  may only have been kicked by a stub whose app is still starting (a cold
     *  Play launch takes minutes), so it waits BrokerIdle.firstAppGraceMs. */
    private volatile boolean sawOpenApp;

    /** The process a package's components live in when they declare none: a
     *  package's android:process components run in their own processes. */
    static String mainProcessOf(PackageRecord pr) {
        return pr.manifest.defaultProcessName();
    }

    /** ComponentInfo.processName for `cls` of `pr`: the process a bind, start,
     *  provider or receiver of that component must be served from. */
    static String processOf(PackageRecord pr, String cls) {
        return pr.manifest.processNameOf(cls);
    }

    static void log(String m) { System.err.println("[system] " + m); }

    // ---- packages ------------------------------------------------------------------

    void addPackage(String apkPath) {
        PackageRecord pr = readPackage(apkPath);
        if (pr == null) return;
        packages.put(pr.pkg, pr);
        log("package " + pr.pkg + " (" + pr.manifest.services.size() + " services) <- " + apkPath);
        accounts().discover(pr);
    }

    /** The package an APK holds, from its manifest; null (logged) when it cannot be read. */
    static PackageRecord readPackage(String apkPath) {
        try {
            Manifest m = Manifest.parse(new File(""), apkPath);
            if (m.packageName == null || m.packageName.isEmpty()) {
                log("no package name in " + apkPath + "; skipped");
                return null;
            }
            return new PackageRecord(m.packageName, apkPath, m);
        } catch (Throwable t) {
            log("cannot read " + apkPath + ": " + t);
            return null;
        }
    }

    /** Resolve a service Intent the way AMS does (through PMS): an explicit
     *  component must be a declared service; an implicit one must name a
     *  package (Android 5+ rejects implicit service intents without one) and
     *  matches the first service whose intent filters list the action, by
     *  android:priority. Returns {package, class} or null. */
    String[] resolve(Intent intent) {
        ComponentName cn = intent.getComponent();
        if (cn != null) {
            PackageRecord pr = packages.get(cn.getPackageName());
            if (pr == null || !pr.manifest.services.containsKey(cn.getClassName())) return null;
            return new String[] { pr.pkg, cn.getClassName() };
        }
        String pkg = intent.getPackage();
        String action = intent.getAction();
        if (pkg == null || action == null) return null;
        PackageRecord pr = packages.get(pkg);
        if (pr == null) return null;
        String best = serviceForAction(pr.manifest, action);
        return best == null ? null : new String[] { pkg, best };
    }

    /** The service of `m` whose intent filters list `action`, highest
     *  android:priority first (AMS through PMS). Static so an app process can
     *  ask the same question of its OWN manifest (SystemClient: is this implicit
     *  intent for a service in one of my sibling processes?) without a second
     *  matcher to drift. */
    static String serviceForAction(Manifest m, String action) {
        if (action == null) return null;
        String best = null;
        int bestPrio = Integer.MIN_VALUE;
        for (Manifest.ServiceEntry e : m.services.values()) {
            if (!e.enabled) continue;
            for (Manifest.IntentFilterEntry f : e.filters) {
                if (f.actions.contains(action) && f.priority > bestPrio) {
                    best = e.name;
                    bestPrio = f.priority;
                }
            }
        }
        return best;
    }

    // ---- processes -------------------------------------------------------------------

    /** The attached DEFAULT process for `pkg` (the one its components that name
     *  no android:process live in), starting it if needed. */
    AppRecord ensureRunning(String pkg) throws Exception {
        PackageRecord pr = packages.get(pkg);
        if (pr == null) throw new IllegalStateException("unknown package " + pkg);
        return ensureRunning(pr, mainProcessOf(pr));
    }

    /** The attached process `proc` of `pkg`, starting it if needed (AMS
     *  startProcessLocked + waiting for attachApplication). */
    AppRecord ensureRunning(String pkg, String proc) throws Exception {
        PackageRecord pr = packages.get(pkg);
        if (pr == null) throw new IllegalStateException("unknown package " + pkg);
        return ensureRunning(pr, proc);
    }

    AppRecord ensureRunning(PackageRecord pr, String proc) throws Exception {
        synchronized (this) {
            AppRecord r = running.get(proc);
            if (r != null && r.thread.isBinderAlive()) return r;
        }
        // A package's processes share its on-disk state, and the first start of
        // an app converts its DEX into a code cache the runtime does not lock. So
        // cold starts of one package are SERIALISED: a second process of it is
        // launched only once the first has attached (which is after that
        // conversion), the order a device gives for free. Different packages
        // start in parallel. The lock is held to attach, not for the process's
        // lifetime.
        synchronized (startLock(pr.pkg)) {
            CompletableFuture<AppRecord> f;
            synchronized (this) {
                AppRecord r = running.get(proc);
                if (r != null && r.thread.isBinderAlive()) return r;
                f = starting.get(proc);
                if (f == null) {
                    f = new CompletableFuture<>();
                    starting.put(proc, f);
                    launch(pr, proc);
                }
            }
            return f.get(START_TIMEOUT_MS, TimeUnit.MILLISECONDS);
        }
    }

    private final Map<String, Object> startLocks = new java.util.concurrent.ConcurrentHashMap<>();
    private Object startLock(String pkg) { return startLocks.computeIfAbsent(pkg, k -> new Object()); }

    /** A file-name-safe form of a process name ("com.android.vending:background"
     *  -> "com.android.vending_background"), for per-process logs. */
    static String fileSafe(String proc) {
        return proc.replaceAll("[^A-Za-z0-9._-]", "_");
    }

    /** Installed bundles by package (product mode, from the registry). */
    private final Map<String, String> bundles = new java.util.concurrent.ConcurrentHashMap<>();

    /** Product mode: every installed app in the Mac.apk registry, by its own
     *  installed APK, each started through its own installed stub. The
     *  registry's reserved-Google filter is about what other apps'
     *  PackageManagers may SEE (MacApkPackageManager.InstalledRegistry.denied);
     *  the broker serving a package the user really installed is a different
     *  question, so it reads the raw rows. */
    void addInstalledPackages() {
        Map<String, MacApkPackageManager.InstalledRegistry.Row> best = new LinkedHashMap<>();
        for (MacApkPackageManager.InstalledRegistry.Row r : MacApkPackageManager.InstalledRegistry.rows()) {
            if (r.pkg == null || r.apkPath == null || !new File(r.apkPath).isFile()) continue;
            MacApkPackageManager.InstalledRegistry.Row cur = best.get(r.pkg);
            if (cur == null || r.versionCode > cur.versionCode) best.put(r.pkg, r);
        }
        for (MacApkPackageManager.InstalledRegistry.Row r : best.values()) {
            addPackage(r.apkPath);
            if (r.bundlePath != null) bundles.put(r.pkg, r.bundlePath);
        }
    }

    /** `<bundle>/Contents/MacOS/<CFBundleExecutable>`, or null. */
    private static String stubOf(String bundlePath) {
        try {
            Process p = new ProcessBuilder("/usr/bin/plutil", "-extract", "CFBundleExecutable", "raw", "-o", "-",
                    bundlePath + "/Contents/Info.plist").redirectErrorStream(true).start();
            String exe = new String(p.getInputStream().readAllBytes(), java.nio.charset.StandardCharsets.UTF_8).trim();
            if (p.waitFor() != 0 || exe.isEmpty()) return null;
            File f = new File(bundlePath, "Contents/MacOS/" + exe);
            return f.canExecute() ? f.getAbsolutePath() : null;
        } catch (Throwable t) {
            return null;
        }
    }

    /** Tests: the command that starts `proc` of a package, instead of the runtime's. */
    java.util.function.BiFunction<PackageRecord, String, List<String>> launchCommand;

    private void launch(PackageRecord pr, String proc) throws Exception {
        String runtime = System.getenv("MACAPK_BROKER_RUNTIME");
        String root = System.getenv("MACAPK_BROKER_ROOT");
        List<String> cmd = new ArrayList<>();
        if (launchCommand != null) {
            cmd.addAll(launchCommand.apply(pr, proc));   // a test's stand-in for the runtime
        } else if (HostAppSupport.windowsHost()) {
            // Win.apk: the app as the launcher starts it, with this broker's own
            // runtime, data folder, JDK and jars (runtime_main sets them).
            cmd.addAll(WindowsBroker.runtimeCommand());
            cmd.add("--apk"); cmd.add(pr.apkPath);
        } else if (runtime != null && !runtime.isEmpty()) {
            // Development: the given runtime binary on the APK.
            cmd.add(runtime);
            cmd.add("--apk"); cmd.add(pr.apkPath);
            if (root != null && !root.isEmpty()) { cmd.add("--android-root"); cmd.add(root); }
        } else {
            // Product: the app's own installed stub, exactly what the Dock runs
            // (it finds the shared runtime and its own android root itself).
            String bundle = bundles.get(pr.pkg);
            String stub = bundle != null ? stubOf(bundle) : null;
            if (stub == null) throw new IllegalStateException("no installed app bundle for " + pr.pkg);
            cmd.add(stub);
        }
        ProcessBuilder pb = new ProcessBuilder(cmd);
        Map<String, String> env = pb.environment();
        // An app gets an APP's environment, not the broker's: the broker's
        // bare JVM runs with logging closed (MACAPK_LOG_LEVEL=7) and its own
        // package list, none of which applies to the process it starts.
        for (String k : new String[] { "MACAPK_LOG_LEVEL", "MACAPK_BROKER_APKS",
                "MACAPK_BROKER_RUNTIME", "MACAPK_BROKER_ROOT", "MACAPK_BROKER_LOGDIR", "MACAPK_APK_PATH" }) {
            env.remove(k);
        }
        env.put("MACAPK_SERVICE_HOST", "1");
        env.put("MACAPK_BROKER", "1");
        env.put("MACAPK_SYSTEM_NAME", sName);
        // The process this runtime is to BE (AMS startProcessLocked(processName)):
        // the legitimate source of ActivityThread.currentProcessName() in a spawned
        // process.
        env.put("MACAPK_PROCESS_NAME", proc);
        // A secondary process shares the package's data and its installed-app
        // identity: the stub must not treat it as a second COPY of the app.
        if (!proc.equals(mainProcessOf(pr))) env.put("MACAPK_SECONDARY_PROCESS", "1");
        // Win.apk: a service host has no window (and does not hand a launcher
        // a ready signal it never asked for).
        if (HostAppSupport.windowsHost()) {
            env.put("WINAPK_HEADLESS", "1");
            env.remove("WINAPK_HEADLESS_EXIT_AFTER_FRAMES");
            env.remove("WINAPK_HEADLESS_FRAME");
            env.remove("WINAPK_READY_EVENT");
        }
        String logDir = System.getenv("MACAPK_BROKER_LOGDIR");
        if (logDir != null && !logDir.isEmpty()) {
            File lf = new File(logDir, (proc.equals(mainProcessOf(pr)) ? pr.pkg : fileSafe(proc)) + ".log");
            pb.redirectErrorStream(true);
            pb.redirectOutput(ProcessBuilder.Redirect.appendTo(lf));
        } else {
            pb.redirectErrorStream(true);
            pb.redirectOutput(ProcessBuilder.Redirect.DISCARD);
        }
        Process p = pb.start();
        log("starting " + proc + " (pid " + p.pid() + ") as a service host");
        spawned.put(p.pid(), p);
        spawnedAs.put(p.pid(), new String[] { pr.pkg, proc });
        p.onExit().thenAccept(x -> {
            spawned.remove(p.pid());
            spawnedAs.remove(p.pid());
            synchronized (SystemServer.this) {
                CompletableFuture<AppRecord> f = starting.remove(proc);
                if (f != null) f.completeExceptionally(
                        new IllegalStateException(proc + " exited (" + x.exitValue() + ") before attaching"));
            }
        });
    }

    // ---- caller identity (Win.apk) -------------------------------------------------

    /** Whether callers are identified by the OS here (Windows), not by what they
     *  write into the parcel (upstream's phase-3 behaviour on the Mac). */
    static boolean osIdentity() { return WinProcesses.available(); }

    /** The package a caller claims, checked: on Windows it must be the package
     *  the broker has on record for the calling process (Binder.getCallingPid,
     *  which the transport takes from the OS). A mismatch is refused and logged;
     *  a process that never registered is refused. On the Mac the claim stands. */
    String callerPackage(String claimed) {
        if (!osIdentity()) return claimed;
        int pid = Binder.getCallingPid();
        String actual = packageOfPid(pid);
        if (actual == null) {
            // Not attached yet (a content provider installs before the app
            // attaches): what the OS says the process is.
            String[] id = identityOfPid(pid);
            if (id != null) actual = id[0];
        }
        if (actual == null) {
            log("REFUSED: pid " + pid + " (claiming " + claimed + ") is no installed app's process");
            throw new SecurityException("pid " + pid + " is not an installed app's process");
        }
        if (claimed != null && !claimed.equals(actual)) {
            log("REFUSED: pid " + pid + " is " + actual + " but claimed to be " + claimed);
            throw new SecurityException("calling package " + claimed + " does not match the calling process ("
                    + actual + ")");
        }
        return actual;
    }

    /** What the OS says `pid` is: the package this broker started it as, or the
     *  package whose installed APK its command line names (the launcher starts
     *  an app as winapk-runtime --apk <library file>, the file installed_apps.json
     *  records for the package). {package, process}, or null for an unknown or
     *  untrusted process. */
    String[] identityOfPid(int pid) {
        String[] mine = spawnedAs.get((long) pid);
        if (mine != null) return mine;
        String apk = WinProcesses.apkArgument(pid);
        if (apk == null) return null;
        String pkg = packageOfApk(apk);
        return pkg == null ? null : new String[] { pkg, null };
    }

    /** The package whose installed APK is `apk` (same file, compared as paths). */
    String packageOfApk(String apk) {
        java.nio.file.Path want;
        try { want = new File(apk).getCanonicalFile().toPath(); } catch (Throwable t) { return null; }
        for (PackageRecord pr : packages.values()) {
            try {
                if (new File(pr.apkPath).getCanonicalFile().toPath().equals(want)) return pr.pkg;
            } catch (Throwable ignored) {}
        }
        return null;
    }

    /** The calling uid of `pid`: its package's uid (AppUids), or -1. */
    int uidOfPid(int pid) {
        if (pid == ProcessHandle.current().pid()) return android.os.Process.SYSTEM_UID;
        String pkg = packageOfPid(pid);
        if (pkg == null) {
            String[] id = identityOfPid(pid);
            if (id != null) pkg = id[0];
        }
        if (pkg == null) return -1;
        PackageRecord pr = packages.get(pkg);
        return AppUids.uidFor(pkg, pr != null ? pr.manifest.sharedUserId : null);
    }

    synchronized void register(String pkg, String proc, IBinder thread, int pid) {
        AppRecord r = new AppRecord(pkg, proc, thread, pid);
        running.put(proc, r);
        try {
            thread.linkToDeath(() -> {
                synchronized (SystemServer.this) {
                    if (running.get(proc) == r) running.remove(proc);
                    lastClientMs = System.currentTimeMillis();   // the grace runs from the last close
                }
                log(proc + " (pid " + pid + ") died");
                jobsProcessDied(pkg, proc);
                reviveSystemApp(proc);
            }, 0);
        } catch (RemoteException e) {
            running.remove(proc);
            return;
        }
        CompletableFuture<AppRecord> f = starting.remove(proc);
        if (f != null) f.complete(r);
        if (!spawned.containsKey((long) pid)) { lastClientMs = System.currentTimeMillis(); sawOpenApp = true; }
        log(proc + " attached (pid " + pid + ")" + (spawned.containsKey((long) pid) ? "" : " [open app]"));
        final AppRecord fr = r;
        new Thread(() -> sendBootBroadcasts(fr), "broker-boot").start();
    }

    /** The attached app whose process is `pid` (the caller identity the kernel's
     *  audit trailer gives a transaction), or null. */
    synchronized String packageOfPid(int pid) {
        for (AppRecord r : running.values()) if (r.pid == pid && r.thread.isBinderAlive()) return r.pkg;
        return null;
    }

    PackageRecord packageRecord(String pkg) { return pkg == null ? null : packages.get(pkg); }

    /** The process serving component `cls` of `pkg`, started on demand. */
    AppRecord ensureServiceProcess(String pkg, String cls) throws Exception {
        PackageRecord pr = packages.get(pkg);
        if (pr == null) throw new IllegalStateException("unknown package " + pkg);
        return ensureRunning(pr, processOf(pr, cls));
    }

    /** Every live process of `pkg`. */
    synchronized List<AppRecord> processesOf(String pkg) {
        List<AppRecord> out = new ArrayList<>();
        for (AppRecord r : running.values()) {
            if (r.pkg.equals(pkg) && r.thread.isBinderAlive()) out.add(r);
        }
        return out;
    }

    /** Bind a service of an installed package on the system's behalf (AMS binding
     *  an authenticator for AccountManagerService): start its process if needed
     *  and return what its onBind answered, or null. */
    IBinder bindService(Intent explicit) throws Exception {
        ComponentName cn = explicit.getComponent();
        AppRecord app = ensureServiceProcess(cn.getPackageName(), cn.getClassName());
        Parcel d = Parcel.obtain(), r = Parcel.obtain();
        try {
            d.writeInterfaceToken(THREAD_DESCRIPTOR);
            d.writeTypedObject(explicit, 0);
            app.thread.transact(TX_SCHEDULE_BIND, d, r, 0);
            r.readException();
            return r.readStrongBinder();
        } finally { d.recycle(); r.recycle(); }
    }

    /** Where system-wide state lives: the broker's android root when it has one
     *  (the test harness), else Mac.apk's support folder. */
    static File systemDir() {
        String root = System.getenv("MACAPK_BROKER_ROOT");
        if (root != null && !root.isEmpty()) return new File(root, "data/system_ce/0");
        if (HostAppSupport.windowsHost()) return HostAppSupport.file("system_ce/0");
        return new File(SharedStorage.hostHome(), "Library/Application Support/Mac.apk/system_ce/0");
    }

    private AccountsService accounts;
    synchronized AccountsService accounts() {
        if (accounts == null) accounts = new AccountsService(this, new File(systemDir(), "accounts_ce.properties"));
        return accounts;
    }

    // ---- the binder --------------------------------------------------------------------

    private static String sName = DEFAULT_NAME;

    final class Stub extends Binder {
        Stub() { super(DESCRIPTOR); }
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            awaitInstalledPackages();
            switch (code) {
                case TX_PACKAGE_CHANGED: {
                    data.enforceInterface(DESCRIPTOR);
                    int pid = Binder.getCallingPid();
                    if (!osIdentity() || !WinProcesses.launcher(pid)) {
                        log("REFUSED package change from pid " + pid + ": not the launcher");
                        throw new SecurityException("only the launcher commits package changes");
                    }
                    int op = data.readInt();
                    String pkg = data.readString();
                    String apk = data.readString();
                    boolean dataRemoved = data.readInt() != 0;
                    packageChanged(op, pkg, apk, dataRemoved);
                    reply.writeNoException();
                    return true;
                }
                case TX_REGISTER: {
                    data.enforceInterface(DESCRIPTOR);
                    String pkg = data.readString();
                    IBinder thread = data.readStrongBinder();
                    String apk = data.readString();
                    // The process it IS (Boot.processName); older clients send
                    // none, and a process is then its package's default one.
                    String proc = data.dataAvail() > 0 ? data.readString() : null;
                    // An app that attached is an installed package too (it may
                    // not be one this broker starts): learn its APK so others'
                    // PackageManagers can answer for it.
                    int pid = Binder.getCallingPid();
                    if (osIdentity()) {
                        // Win.apk: the package is what the OS says this process
                        // is, never what it claims; the APK is the installed one.
                        String[] id = identityOfPid(pid);
                        if (id == null || !id[0].equals(pkg)) {
                            log("REFUSED registration: pid " + pid + " claimed " + pkg + ", the OS says "
                                    + (id == null ? "it is no installed app" : id[0]));
                            throw new SecurityException("process " + pid + " is not " + pkg);
                        }
                        if (proc == null || proc.isEmpty()) proc = pkg;
                        // A process this broker started is exactly the one it
                        // started; one the launcher started is the main process.
                        boolean procOk = id[1] != null ? id[1].equals(proc)
                                : proc.equals(pkg) || proc.equals(mainProcessOf(packages.get(pkg)));
                        if (!procOk) {
                            log("REFUSED registration: pid " + pid + " (" + pkg + ") claimed process " + proc);
                            throw new SecurityException("process " + pid + " is not " + proc);
                        }
                    } else if (apk != null && !apk.isEmpty() && !packages.containsKey(pkg)) {
                        addPackage(apk);
                    }
                    if (proc == null || proc.isEmpty()) proc = pkg;
                    register(pkg, proc, thread, pid);
                    reply.writeNoException();
                    // The broker's pid: an app accepts calls on its application
                    // thread from this process only (SystemClient.AppThread).
                    reply.writeInt((int) ProcessHandle.current().pid());
                    return true;
                }
                case TX_GET_ACCOUNT_SERVICE: {
                    data.enforceInterface(DESCRIPTOR);
                    reply.writeNoException();
                    reply.writeStrongBinder(accounts());
                    return true;
                }
                case TX_UID_OF_PID: {
                    data.enforceInterface(DESCRIPTOR);
                    callerPackage(null);   // a registered app asks, about its own caller
                    int pid = data.readInt();
                    reply.writeNoException();
                    reply.writeInt(uidOfPid(pid));
                    return true;
                }
                case TX_HAS_PACKAGE: {
                    // Known here, and where its installed APK is: the caller's
                    // PackageManager answers getPackageInfo for it from that
                    // APK itself (its own manifest, its own real signature), as
                    // PMS answers from the installed package on a device.
                    data.enforceInterface(DESCRIPTOR);
                    String pkg = data.readString();
                    PackageRecord pr = packages.get(pkg);
                    reply.writeNoException();
                    reply.writeInt(pr != null ? 1 : 0);
                    reply.writeString(pr != null ? pr.apkPath : null);
                    return true;
                }
                case TX_PROVIDER_PACKAGE: {
                    // The caller's PackageManager then answers from that
                    // package's own manifest (SystemClient.pmFor), as
                    // HAS_PACKAGE does for getPackageInfo.
                    data.enforceInterface(DESCRIPTOR);
                    String authority = data.readString();
                    Object[] hit = authority == null ? null : findProvider(authority);
                    reply.writeNoException();
                    reply.writeString(hit != null ? ((PackageRecord) hit[0]).pkg : null);
                    return true;
                }
                case TX_RESOLVE: {
                    data.enforceInterface(DESCRIPTOR);
                    String caller = callerPackage(data.readString());
                    Intent intent = data.readTypedObject(Intent.CREATOR);
                    String[] target = intent == null ? null : resolve(intent);
                    int answer = RESOLVE_NONE;
                    String missing = null;
                    if (target != null) {
                        Manifest.ServiceEntry se = packages.get(target[0]).manifest.services.get(target[1]);
                        missing = serviceDenial(caller, target, se);
                        answer = missing == null ? RESOLVE_OK
                                : missing.isEmpty() ? RESOLVE_NOT_EXPORTED : RESOLVE_NO_PERMISSION;
                    } else {
                        log("resolve from " + caller + ": nothing serves " + intent);
                    }
                    reply.writeNoException();
                    reply.writeInt(answer);
                    reply.writeTypedObject(target == null ? null : new ComponentName(target[0], target[1]), 0);
                    if (answer == RESOLVE_NO_PERMISSION) reply.writeString(missing);
                    return true;
                }
                case TX_GET_PROVIDER: {
                    data.enforceInterface(DESCRIPTOR);
                    String caller = callerPackage(data.readString());
                    String authority = data.readString();
                    IBinder b;
                    try {
                        b = getProvider(caller, authority);
                    } catch (SecurityException e) {
                        reply.writeException(e);
                        return true;
                    }
                    reply.writeNoException();
                    reply.writeStrongBinder(b);
                    return true;
                }
                case TX_START: {
                    data.enforceInterface(DESCRIPTOR);
                    String caller = callerPackage(data.readString());
                    Intent intent = data.readTypedObject(Intent.CREATOR);
                    start(caller, intent);
                    reply.writeNoException();
                    return true;
                }
                case TX_BROADCAST: {
                    data.enforceInterface(DESCRIPTOR);
                    String caller = callerPackage(data.readString());
                    Intent intent = data.readTypedObject(Intent.CREATOR);
                    // The sender's own process, which delivered to itself.
                    String callerProc = data.dataAvail() > 0 ? data.readString() : null;
                    // sendBroadcast(intent, receiverPermission): what a receiving app must hold.
                    String[] required = data.dataAvail() > 0 ? data.createStringArray() : null;
                    if (intent != null && intent.getAction() != null && osIdentity()
                            && PACKAGE_ACTIONS.contains(intent.getAction())) {
                        // AMS: a protected broadcast only the system sends ("Permission Denial: not
                        // allowed to send broadcast"). The broker sends these from the launcher's
                        // commit (packageChanged); an app's own announcement stays in its process.
                        log("REFUSED broadcast " + intent.getAction() + " from " + caller + ": only the system sends it");
                        intent = null;
                    }
                    if (intent != null) {
                        final Intent fi = intent; final String fc = caller, fp = callerProc;
                        final String[] fr = required;
                        new Thread(() -> broadcast(fc, fp, fi, fr), "broker-broadcast").start();
                    }
                    if (reply != null) reply.writeNoException();
                    return true;
                }
                case TX_OBSERVE: {
                    data.enforceInterface(DESCRIPTOR);
                    String caller = callerPackage(data.readString());
                    String proc = data.readString();
                    String authority = data.readString();
                    if (proc != null && authority != null && (proc.equals(caller) || proc.startsWith(caller + ":")
                            || ownsProcess(caller, proc))) {
                        observers.computeIfAbsent(proc, k -> java.util.concurrent.ConcurrentHashMap.newKeySet())
                                .add(authority);
                    }
                    if (reply != null) reply.writeNoException();
                    return true;
                }
                case TX_NOTIFY_CHANGE: {
                    data.enforceInterface(DESCRIPTOR);
                    callerPackage(data.readString());
                    String proc = data.readString();
                    String uri = data.readString();
                    if (uri != null) notifyObservers(proc, uri);
                    if (reply != null) reply.writeNoException();
                    return true;
                }
                case TX_BIND: {
                    data.enforceInterface(DESCRIPTOR);
                    String caller = callerPackage(data.readString());
                    Intent intent = data.readTypedObject(Intent.CREATOR);
                    bind(caller, intent, reply);
                    return true;
                }
                case TX_STOP: {
                    data.enforceInterface(DESCRIPTOR);
                    String caller = callerPackage(data.readString());
                    Intent intent = data.readTypedObject(Intent.CREATOR);
                    boolean stopped;
                    try {
                        stopped = stopService(caller, intent);
                    } catch (SecurityException e) {
                        reply.writeException(e);
                        return true;
                    }
                    reply.writeNoException();
                    reply.writeInt(stopped ? 1 : 0);
                    return true;
                }
                case TX_JOB: {
                    data.enforceInterface(DESCRIPTOR);
                    job(data, reply);
                    return true;
                }
                case TX_JOB_REPORT: {
                    data.enforceInterface(DESCRIPTOR);
                    jobReport(data);
                    if (reply != null) reply.writeNoException();
                    return true;
                }
                case TX_CHECK_PROVIDER: {
                    data.enforceInterface(DESCRIPTOR);
                    String self = callerPackage(null);   // the provider's own process asks
                    int pid = data.readInt();
                    String uri = data.readString();
                    boolean write = data.readInt() != 0;
                    Object[] answer = checkProvider(self, pid, uri, write);
                    reply.writeNoException();
                    reply.writeInt((Integer) answer[0]);
                    reply.writeString((String) answer[1]);
                    return true;
                }
                case TX_CHECK_PERMISSION: {
                    data.enforceInterface(DESCRIPTOR);
                    callerPackage(null);
                    String perm = data.readString();
                    String pkg = data.readString();
                    boolean granted = pkg != null && packages.containsKey(pkg) && perms.holds(pkg, perm);
                    reply.writeNoException();
                    reply.writeInt(granted ? 1 : 0);
                    return true;
                }
                case TX_CHECK_PERMISSION_PID: {
                    data.enforceInterface(DESCRIPTOR);
                    callerPackage(null);
                    String perm = data.readString();
                    int pid = data.readInt();
                    boolean granted;
                    if (pid == ProcessHandle.current().pid()) {
                        granted = true;   // the system
                    } else {
                        String pkg = packageOfProcess(pid);
                        granted = pkg != null && perms.holds(pkg, perm);
                    }
                    reply.writeNoException();
                    reply.writeInt(granted ? 1 : 0);
                    return true;
                }
                case TX_GRANT_URI: {
                    data.enforceInterface(DESCRIPTOR);
                    String caller = callerPackage(null);
                    String target = data.readString();
                    String uri = data.readString();
                    int mode = data.readInt();
                    String why = target == null || uri == null || !packages.containsKey(target) ? null
                            : perms.grant(caller, target, android.net.Uri.parse(uri), mode,
                                    (mode & Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) != 0, providerLookup);
                    if (why != null) {
                        log("uri grant " + caller + " -> " + target + " REFUSED: " + why);
                        reply.writeException(new SecurityException(why));
                        return true;
                    }
                    reply.writeNoException();
                    return true;
                }
                case TX_REVOKE_URI: {
                    data.enforceInterface(DESCRIPTOR);
                    String caller = callerPackage(null);
                    String target = data.readString();
                    String uri = data.readString();
                    int mode = data.readInt();
                    android.net.Uri u = uri == null ? null : android.net.Uri.parse(uri);
                    Object[] hit = u == null || u.getAuthority() == null ? null : providerLookup.find(u.getAuthority());
                    // UriGrantsManagerService.revokeUriPermission: the provider's own app revokes the grants
                    // of the uri, to one package or to all.
                    if (hit != null && perms.sameApp(caller, (String) hit[0])) {
                        perms.grants.revoke((String) hit[0], target, u, mode);
                    }
                    reply.writeNoException();
                    return true;
                }
                case TX_CHECK_URI: {
                    data.enforceInterface(DESCRIPTOR);
                    callerPackage(null);
                    int pid = data.readInt();
                    String uri = data.readString();
                    int mode = data.readInt();
                    reply.writeNoException();
                    reply.writeInt(checkUri(pid, uri, mode));
                    return true;
                }
                case TX_APP_STATE: {
                    data.enforceInterface(DESCRIPTOR);
                    callerPackage(null);   // a registered app asks (an installer, about the app it updates)
                    String pkg = data.readString();
                    reply.writeNoException();
                    reply.writeInt(appState(pkg));
                    return true;
                }
            }
            return super.onTransact(code, data, reply, flags);
        }
    }

    /** Which authorities each process (by name) has a ContentObserver on. */
    private final Map<String, java.util.Set<String>> observers = new java.util.concurrent.ConcurrentHashMap<>();

    /** A process name of `pkg`'s (its android:process values). */
    private boolean ownsProcess(String pkg, String proc) {
        for (AppRecord r : processesOf(pkg)) if (r.proc.equals(proc)) return true;
        return false;
    }

    /** AOSP ContentService.notifyChange: every observer of the uri, in every
     *  process but the notifier's (which has told its own). Matched by
     *  authority here; the receiving ContentResolver matches the uri. */
    private void notifyObservers(String fromProc, String uri) {
        String authority;
        try { authority = android.net.Uri.parse(uri).getAuthority(); } catch (Throwable t) { return; }
        if (authority == null) return;
        for (Map.Entry<String, java.util.Set<String>> e : observers.entrySet()) {
            if (e.getKey().equals(fromProc) || !e.getValue().contains(authority)) continue;
            AppRecord app;
            synchronized (this) { app = running.get(e.getKey()); }
            if (app == null || !app.thread.isBinderAlive()) continue;
            Parcel d = Parcel.obtain();
            try {
                d.writeInterfaceToken(THREAD_DESCRIPTOR);
                d.writeString(uri);
                app.thread.transact(TX_SCHEDULE_NOTIFY, d, null, IBinder.FLAG_ONEWAY);
            } catch (Throwable t) {
                log("notifyChange " + uri + " -> " + app.proc + " FAILED: " + t);
            } finally { d.recycle(); }
        }
    }

    /** Actions Android still delivers to a STOPPED app's manifest receivers
     *  (the implicit-broadcast exemptions that matter here). */
    private static final java.util.Set<String> STARTING_ACTIONS = java.util.Set.of(
        "android.intent.action.LOCKED_BOOT_COMPLETED",
        "android.intent.action.BOOT_COMPLETED",
        "android.intent.action.MY_PACKAGE_REPLACED",
        "android.intent.action.LOCALE_CHANGED",
        "android.intent.action.TIMEZONE_CHANGED");

    /** Does `pkg`'s manifest have a receiver for this broadcast? */
    private static boolean manifestReceives(PackageRecord pr, Intent intent) {
        return !receiverProcesses(pr, intent).isEmpty();
    }

    /** Does `pkg`'s manifest have a receiver for this broadcast that lives in
     *  process `proc`? */
    private static boolean manifestReceives(PackageRecord pr, Intent intent, String proc) {
        return receiverProcesses(pr, intent).contains(proc);
    }

    /** The processes whose manifest-declared receivers match `intent`: a
     *  receiver runs in ITS android:process, so one broadcast can have receivers
     *  in several processes of the same package. */
    private static java.util.Set<String> receiverProcesses(PackageRecord pr, Intent intent) {
        return receiverProcesses(pr, intent, null);
    }

    /** receiverProcesses, of the receivers `admit` lets the broadcast reach (null: all). */
    private static java.util.Set<String> receiverProcesses(PackageRecord pr, Intent intent,
            java.util.function.Predicate<Manifest.ActivityEntry> admit) {
        java.util.Set<String> out = new java.util.LinkedHashSet<>();
        ComponentName cn = intent.getComponent();
        for (Manifest.ActivityEntry r : pr.manifest.receivers.values()) {
            if (!r.enabled) continue;
            if (admit != null && !admit.test(r)) continue;
            boolean match = false;
            if (cn != null) {
                match = cn.getClassName().equals(r.name);
            } else {
                for (Manifest.IntentFilterEntry f : r.filters) {
                    if (intent.getAction() != null && f.actions.contains(intent.getAction())) {
                        match = true;
                        break;
                    }
                }
            }
            if (match) out.add(processOf(pr, r.name));
        }
        return out;
    }

    /** AMS broadcastIntent, across apps: every RUNNING process of every app
     *  gets it (its own dynamic receivers + the manifest receivers that live in
     *  that process decide); a stopped process is started for it only when it
     *  owns a matching manifest receiver and the broadcast is explicitly
     *  targeted or is one of the boot-time exemptions. The caller's OWN package
     *  is reached too, in its OTHER processes: the caller's process delivered to
     *  itself, and a receiver declared in a sibling android:process is only ever
     *  run there. `callerProc` is the process that sent it (null = unknown). */
    void broadcast(String caller, String callerProc, Intent intent) { broadcast(caller, callerProc, intent, null); }

    /** broadcast, with the sender's required permissions (sendBroadcast's receiverPermission): a
     *  package that does not hold every one of them is left out (BroadcastQueue's skip, logged as AMS
     *  logs it). A receiver's own android:permission is the sender's to hold: the broker leaves such
     *  a manifest receiver out of the processes it starts, and the receiving process checks it, and a
     *  registered receiver's, against the sender it is told (SCHEDULE_BROADCAST). An explicitly
     *  targeted broadcast's URI grants go to its package (AMS grants them as it delivers). */
    void broadcast(String caller, String callerProc, Intent intent, String[] required) {
        // Play services announces a finished device check-in; the boot nudge
        // below has done its job.
        if (CHECKIN_COMPLETE.equals(intent.getAction())
                && ("com.google.android.gms".equals(caller) || "com.google.android.gsf".equals(caller))) {
            if (!checkinCompleted) log("boot: device check-in completed (" + caller + ")");
            checkinCompleted = true;
        }
        String target = intent.getComponent() != null ? intent.getComponent().getPackageName()
                                                      : intent.getPackage();
        java.util.List<String> pkgs = new ArrayList<>(packages.keySet());
        for (String pkg : pkgs) {
            if (target != null && !target.equals(pkg)) continue;
            boolean own = pkg.equals(caller);
            // Another app's stopped process starts only for a targeted or
            // boot-time broadcast; the app's own broadcast starts the sibling
            // process that owns the receiver (AMS starts a receiver's process).
            boolean mayStart = own || target != null || STARTING_ACTIONS.contains(intent.getAction());
            String lacks = null;
            if (required != null) {
                for (String rp : required) {
                    if (rp != null && !perms.holds(pkg, rp)) { lacks = rp; break; }
                }
            }
            if (lacks != null) {
                PackageRecord pr = packages.get(pkg);
                if (target != null || (pr != null && manifestReceives(pr, intent)) || !processesOf(pkg).isEmpty()) {
                    log("Permission Denial: receiving " + intent + " to " + pkg + " requires " + lacks
                            + " due to sender " + caller + " (uid " + perms.uidOf(caller) + ")");
                }
                continue;
            }
            if (!own && target != null) grantFromIntent(caller, pkg, intent);
            deliverToPackage(pkg, intent, own, mayStart, own ? callerProc : null, own ? null : caller);
        }
    }

    /** Deliver one broadcast to ONE package: every running process of it, plus
     *  (when `mayStart`) any stopped process that owns a matching manifest
     *  receiver, started for the purpose. `fromSystem`: the system's own
     *  broadcast, which reaches non-exported receivers too (see deliverTo).
     *  `skipProc`: the sender's own process, which delivered to itself. */
    void deliverToPackage(String pkg, Intent intent, boolean fromSystem, boolean mayStart, String skipProc) {
        deliverToPackage(pkg, intent, fromSystem, mayStart, skipProc, null);
    }

    /** deliverToPackage from `sender`, another app (null: the system or the package itself): a
     *  stopped process is started only for a manifest receiver the sender may reach (exported, and
     *  its android:permission held: AMS checkComponentPermission). */
    void deliverToPackage(String pkg, Intent intent, boolean fromSystem, boolean mayStart, String skipProc,
            String sender) {
        PackageRecord pr = packages.get(pkg);
        if (pr == null) return;
        java.util.Set<String> owners = receiverProcesses(pr, intent, sender == null ? null : r -> {
            if (perms.checkComponent(r.permission, sender, pkg, r.exported)) return true;
            if (r.exported) {
                log("Permission Denial: broadcasting " + intent + " from " + sender + " (uid " + perms.uidOf(sender)
                        + ") requires " + r.permission + " due to receiver " + pkg + "/" + r.name);
            }
            return false;
        });
        java.util.Set<String> procs = new java.util.LinkedHashSet<>();
        for (AppRecord r : processesOf(pkg)) procs.add(r.proc);
        procs.addAll(owners);
        for (String proc : procs) {
            if (proc.equals(skipProc)) continue;
            AppRecord app;
            synchronized (this) { app = running.get(proc); }
            if (app == null || !app.thread.isBinderAlive()) {
                if (!mayStart || !owners.contains(proc)) continue;
                try { app = ensureRunning(pr, proc); } catch (Throwable t) {
                    log("broadcast " + intent.getAction() + " -> " + proc + ": start FAILED " + t);
                    continue;
                }
            }
            deliverTo(app, intent, fromSystem, sender);
        }
    }

    private void deliverTo(AppRecord app, Intent intent) { deliverTo(app, intent, false); }

    /** `fromSystem`: the broker's own broadcast (boot), which AMS delivers to
     *  non-exported receivers too; another app's broadcast reaches only
     *  exported ones. */
    private void deliverTo(AppRecord app, Intent intent, boolean fromSystem) { deliverTo(app, intent, fromSystem, null); }

    /** `sender`: the app that sent it, whose permissions the receivers' required ones are checked
     *  against in the receiving process (null: the system, or the package's own process). */
    private void deliverTo(AppRecord app, Intent intent, boolean fromSystem, String sender) {
        Parcel d = Parcel.obtain();
        try {
            d.writeInterfaceToken(THREAD_DESCRIPTOR);
            d.writeTypedObject(intent, 0);
            d.writeInt(fromSystem ? 1 : 0);
            d.writeString(fromSystem ? null : sender);
            app.thread.transact(TX_SCHEDULE_BROADCAST, d, null, IBinder.FLAG_ONEWAY);
            log("broadcast " + intent.getAction() + " -> " + app.proc);
        } catch (Throwable t) {
            log("broadcast " + intent.getAction() + " -> " + app.proc + " FAILED: " + t);
        } finally { d.recycle(); }
    }

    // ---- package changes (Win.apk) ---------------------------------------------------------

    /** The package-lifecycle broadcasts, which only the system sends (protected broadcasts). */
    static final java.util.Set<String> PACKAGE_ACTIONS = java.util.Set.of(
        Intent.ACTION_PACKAGE_ADDED, Intent.ACTION_PACKAGE_REPLACED, Intent.ACTION_MY_PACKAGE_REPLACED,
        Intent.ACTION_PACKAGE_REMOVED, Intent.ACTION_PACKAGE_FULLY_REMOVED, Intent.ACTION_PACKAGE_CHANGED,
        Intent.ACTION_PACKAGE_DATA_CLEARED, Intent.ACTION_PACKAGE_RESTARTED);

    /** Implicit broadcasts Android still delivers to the manifest receivers of an app that targets
     *  API 26 or later ("Implicit broadcast exceptions"): of the package ones, these two. */
    static final java.util.Set<String> IMPLICIT_EXEMPT = java.util.Set.of(
        Intent.ACTION_PACKAGE_FULLY_REMOVED, Intent.ACTION_PACKAGE_DATA_CLEARED);

    /** Serialises the commits, and the first read of the registry before them. */
    private final Object packagesLock = new Object();
    /** Set by main while the registry is read for the first time; every call waits for that. */
    private volatile java.util.concurrent.CountDownLatch installedRead;

    private void awaitInstalledPackages() {
        java.util.concurrent.CountDownLatch l = installedRead;
        if (l == null) return;
        try { l.await(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
    }

    /** PackageManagerService's commit and what ActivityManager does after it, told by the launcher
     *  (TX_PACKAGE_CHANGED) once installed_apps.json holds the change. Added or replaced: the package
     *  is read from `apk` and replaces the broker's record of it (its account authenticators too), so
     *  a process of it is known when it attaches; then PACKAGE_ADDED (EXTRA_REPLACING on an update),
     *  PACKAGE_REPLACED and MY_PACKAGE_REPLACED go out, as InstallPackageHelper sends them. Removed:
     *  the record goes, then PACKAGE_REMOVED and, with the data, PACKAGE_FULLY_REMOVED. The processes
     *  of a replaced or removed package are not stopped here: the launcher stops them before it
     *  touches the app's files (the install's freeze), as Android kills them before the commit. */
    void packageChanged(int op, String pkg, String apk, boolean dataRemoved) {
        if (pkg == null || pkg.isEmpty() || apk == null || apk.isEmpty()
                || op < PACKAGE_ADDED || op > PACKAGE_REMOVED) {
            throw new IllegalArgumentException("package change " + op + " " + pkg + " " + apk);
        }
        int uid;
        boolean replacing;
        synchronized (packagesLock) {
            PackageRecord old = packages.get(pkg);
            if (op == PACKAGE_REMOVED) {
                // Another copy of the package (a second library name) going is not the package going.
                if (old == null || !sameFile(old.apkPath, apk)) {
                    log("package " + pkg + ": " + apk + " removed, which is not its installed APK; nothing changes");
                    return;
                }
                uid = AppUids.uidFor(pkg, old.manifest.sharedUserId);
                packages.remove(pkg);
                bundles.remove(pkg);
                accounts().forget(pkg);
                jobs().removeAll(pkg);   // JobSchedulerService drops a removed package's jobs
                perms.grants.forget(pkg);   // and UriGrantsManagerService its grants
                log("package " + pkg + " removed" + (dataRemoved ? " with its data" : ""));
                replacing = false;
            } else {
                PackageRecord pr = readPackage(apk);
                if (pr == null || !pr.pkg.equals(pkg)) {
                    log("REFUSED package change: " + apk + " is " + (pr == null ? "unreadable" : pr.pkg)
                            + ", not " + pkg);
                    throw new IllegalArgumentException(apk + " is not " + pkg);
                }
                replacing = old != null || op == PACKAGE_REPLACED;
                packages.put(pkg, pr);
                bundles.put(pkg, apk);
                accounts().forget(pkg);
                accounts().discover(pr);
                uid = AppUids.uidFor(pkg, pr.manifest.sharedUserId);
                log("package " + pkg + (replacing ? " replaced" : " added") + " (" + pr.manifest.services.size()
                        + " services) <- " + apk);
            }
        }
        android.net.Uri data = android.net.Uri.fromParts("package", pkg, null);
        if (op == PACKAGE_REMOVED) {
            // The removed package does not hear its own removal (Intent.ACTION_PACKAGE_REMOVED).
            Intent removed = packageIntent(Intent.ACTION_PACKAGE_REMOVED, data, uid);
            removed.putExtra(Intent.EXTRA_DATA_REMOVED, dataRemoved);
            broadcastPackageEvent(removed, pkg);
            if (dataRemoved) {
                Intent fully = packageIntent(Intent.ACTION_PACKAGE_FULLY_REMOVED, data, uid);
                fully.putExtra(Intent.EXTRA_DATA_REMOVED, true);
                broadcastPackageEvent(fully, pkg);
            }
            return;
        }
        Intent added = packageIntent(Intent.ACTION_PACKAGE_ADDED, data, uid);
        if (replacing) added.putExtra(Intent.EXTRA_REPLACING, true);
        broadcastPackageEvent(added, null);
        if (!replacing) return;
        Intent replaced = packageIntent(Intent.ACTION_PACKAGE_REPLACED, data, uid);
        replaced.putExtra(Intent.EXTRA_REPLACING, true);
        broadcastPackageEvent(replaced, null);
        // To the updated package alone, with no data: explicit, so its manifest receiver hears it
        // and its process is started for it (STARTING_ACTIONS). Not on the launcher's call, which
        // must not wait for a process to start.
        Intent mine = new Intent(Intent.ACTION_MY_PACKAGE_REPLACED).setPackage(pkg);
        new Thread(() -> deliverToPackage(pkg, mine, true, true, null), "broker-package").start();
    }

    /** BroadcastHelper.doSendBroadcast's shape: package:<pkg> data, the package's uid, user 0. */
    private static Intent packageIntent(String action, android.net.Uri data, int uid) {
        Intent i = new Intent(action, data);
        i.putExtra(Intent.EXTRA_UID, uid);
        i.putExtra(Intent.EXTRA_USER_HANDLE, 0);
        return i;
    }

    /** One implicit package broadcast from the system to every running process (but `skipPkg`'s): its
     *  dynamic receivers always; its manifest receivers only where Android allows an implicit broadcast,
     *  an app that targets API 25 or lower, or an exempt action (else the intent carries
     *  FLAG_RECEIVER_REGISTERED_ONLY, which that process's delivery honours). A process that is not
     *  running is not started for it, as Android leaves a stopped package out of a broadcast. Sent
     *  oneway, before the launcher's call returns: the installing app hears it before its status. */
    private void broadcastPackageEvent(Intent intent, String skipPkg) {
        List<AppRecord> apps;
        synchronized (this) { apps = new ArrayList<>(running.values()); }
        boolean exempt = IMPLICIT_EXEMPT.contains(intent.getAction());
        for (AppRecord app : apps) {
            if (app.pkg.equals(skipPkg) || !app.thread.isBinderAlive()) continue;
            PackageRecord pr = packages.get(app.pkg);
            boolean manifestToo = exempt || (pr != null && pr.manifest.targetSdk < 26);
            Intent i = new Intent(intent);
            if (!manifestToo) i.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY);
            deliverTo(app, i, true);
        }
    }

    private static boolean sameFile(String a, String b) {
        try {
            return new File(a).getCanonicalFile().equals(new File(b).getCanonicalFile());
        } catch (Throwable t) {
            return a.equalsIgnoreCase(b);
        }
    }

    /** Processes that have had their boot broadcasts since this broker started
     *  (by process name: a boot receiver runs in ITS android:process). */
    private final java.util.Set<String> booted = java.util.concurrent.ConcurrentHashMap.newKeySet();
    /** Packages whose one-shot boot-time kicks have been sent. */
    private final java.util.Set<String> bootKicked = java.util.concurrent.ConcurrentHashMap.newKeySet();

    static final String CHECKIN_COMPLETE = "com.google.android.checkin.CHECKIN_COMPLETE";
    /** Play services broadcast CHECKIN_COMPLETE since this broker started. */
    private volatile boolean checkinCompleted;

    private void sendBootBroadcasts(AppRecord app) {
        PackageRecord pr = packages.get(app.pkg);
        if (pr == null) return;
        if (booted.add(app.proc)) {
            for (String a : new String[] { "android.intent.action.LOCKED_BOOT_COMPLETED",
                                           "android.intent.action.BOOT_COMPLETED" }) {
                Intent i = new Intent(a).setPackage(app.pkg);
                if (manifestReceives(pr, i, app.proc)) deliverTo(app, i, true);
            }
        }
        if (!bootKicked.add(app.pkg)) return;
        // System-level device-checkin kick at boot. On a device the framework/GMS
        // causes the first device checkin (registration -> GSF Android ID) at boot;
        // GMS self-schedules a one-off GmsTask for it, which runs via GMS's own
        // NetworkScheduler. Our standalone GcmTaskService scheduler
        // (com.google.android.gms.gcm.nts.*) does not yet execute that scheduled
        // one-off (immediate tasks are refused, the one-off never fires), so without
        // a nudge the device never checks in and no Google-trusted identity exists.
        // The broker IS this device's system_server, and CheckinServiceTriggerReceiver
        // declares an intent filter for exactly android.server.checkin.CHECKIN (its
        // manifest), so delivering that broadcast is the platform-faithful way for the
        // system to ask GMS to check in -- the same trigger `am broadcast -a
        // android.server.checkin.CHECKIN` uses. GMS's own rate-limiting / backoff
        // decides whether to actually hit the network, so re-sending is idempotent.
        // Sent a few times with backoff because on a cold boot the Chimera config
        // (which routes the receiver) commits ~2-3 min in; at least one send lands
        // after it is ready, and none once Play services has broadcast
        // CHECKIN_COMPLETE: every nudge after that started one more check-in
        // (each answered with CHECKIN_COMPLETE again), so a device that had
        // checked in at boot checked in three more times, a minute apart.
        // Follow-up (broader, not on the sign-in path): make the nts scheduler execute
        // scheduled tasks so this nudge is unnecessary and all GMS background tasks run.
        if ("com.google.android.gms".equals(app.pkg)) {
            Thread t = new Thread(() -> {
                try {
                    long base = Long.getLong("macapk.broker.checkinTriggerMs", 75_000L);
                    for (int attempt = 0; attempt < 3; attempt++) {
                        Thread.sleep(attempt == 0 ? base : 75_000L);
                        if (processesOf(app.pkg).isEmpty()) return;
                        if (checkinCompleted) {
                            log("boot: device check-in already done; no nudge");
                            return;
                        }
                        Intent ci = new Intent("android.server.checkin.CHECKIN").setPackage(app.pkg);
                        log("boot: kicking device checkin (android.server.checkin.CHECKIN) -> " + app.pkg
                                + " (attempt " + (attempt + 1) + ")");
                        // The receiver runs in ITS process (the default one in
                        // practice); deliverToPackage routes there and starts it.
                        deliverToPackage(app.pkg, ci, true, true, null);
                    }
                } catch (Throwable e) { log("checkin boot-kick failed: " + e); }
            }, "checkin-boot-kick");
            t.setDaemon(true);
            t.start();
        }
    }

    /** AMS finishBooting -> sendBootCompleted, for the apps this device ships as
     *  SYSTEM apps (the ones our PackageManager reports with FLAG_SYSTEM:
     *  MacApkPackageManager.platformSystemPackage). On a phone every app with a
     *  BOOT_COMPLETED receiver is started at boot; that is how Google Play
     *  services and the Services Framework are already up when the user opens
     *  Play. Here the broker coming up IS the boot (launchd starts it on the
     *  first app's lookup), so it starts those system apps now and each gets its
     *  LOCKED_BOOT_COMPLETED / BOOT_COMPLETED as it attaches (sendBootBroadcasts).
     *
     *  Only headless system apps (no launcher activity): a process started here
     *  is a service host without a window, and a user-facing app (Play itself)
     *  must come up from its own launch instead -- it gets its boot broadcasts
     *  when it attaches, exactly as before. Restricted to system apps rather than
     *  every installed app because each Mac.apk process is a JVM, and starting
     *  all of a user's apps at boot is a cost a phone pays and a Mac should not. */
    /** The file the broker writes once its boot has finished (every system app
     *  it started has attached or failed): the app stub for Google Play waits
     *  for it before opening Play, so the services Play needs are up first
     *  (owner, 2026-11-02). Removed when a broker starts, so a file left by a
     *  dead broker never reads as readiness. */
    static File readyFile() {
        String f = System.getenv("MACAPK_BROKER_READY_FILE");
        if (f != null && !f.isEmpty()) return new File(f);
        if (HostAppSupport.windowsHost()) return HostAppSupport.file("run/system-ready");
        return new File(System.getProperty("user.home"), "Library/Application Support/Mac.apk/run/system-ready");
    }

    private static void writeReady(String started) { writeReady(started, ""); }

    /** Line 1 the broker's pid, line 2 the system processes that attached, line 3
     *  those that never did (the stub logs it). */
    private static void writeReady(String started, String failed) {
        try {
            File f = readyFile();
            f.getParentFile().mkdirs();
            java.nio.file.Files.writeString(f.toPath(),
                    ProcessHandle.current().pid() + "\n" + started + "\n" + failed + "\n");
        } catch (Throwable t) { log("could not write " + readyFile() + ": " + t); }
    }

    /** The system apps bootSystemServices started, by process name: the ones the
     *  broker keeps alive (AMS restarts a persistent system app that dies). */
    private final java.util.Map<String, PackageRecord> bootSupervised = new java.util.concurrent.ConcurrentHashMap<>();
    /** Set while the broker is stopping its services (idle exit): a death then is the stop. */
    private volatile boolean stopping;
    private final java.util.Map<String, java.util.ArrayDeque<Long>> reviveTimes = new java.util.concurrent.ConcurrentHashMap<>();

    /** A system app the broker booted died (killed, crashed, quit by the user).
     *  Readiness is withdrawn at once -- the readiness file otherwise still says
     *  "up" and the next Play launch passes its wait, then binds a service that has
     *  a whole JVM (and, after an update, a DEX conversion) to boot, outlasting
     *  Play's own bind timeout (owner, 2026-11-03: after a new build Play only
     *  appeared on the second open). The app is started again, as AMS does for a
     *  persistent system app, and readiness is written back when all are up.
     *  At most 3 revivals per process per 5 minutes, so a crash loop is not a
     *  respawn loop. */
    private void reviveSystemApp(String proc) {
        final PackageRecord pr = bootSupervised.get(proc);
        if (pr == null || stopping) return;
        java.util.ArrayDeque<Long> times = reviveTimes.computeIfAbsent(proc, k -> new java.util.ArrayDeque<>());
        long now = System.currentTimeMillis();
        synchronized (times) {
            while (!times.isEmpty() && now - times.peekFirst() > 300_000L) times.pollFirst();
            if (times.size() >= 3) {
                log("boot: system app " + proc + " died again (3 revivals in 5 min); leaving it down");
                return;
            }
            times.addLast(now);
        }
        readyFile().delete();
        log("boot: system app " + proc + " died; reviving it");
        Thread t = new Thread(() -> {
            for (int attempt = 1; attempt <= 3 && !stopping; attempt++) {
                try { ensureRunning(pr, proc); break; }
                catch (Throwable e) {
                    log("boot: reviving " + proc + " failed (attempt " + attempt + "): " + e);
                    try { Thread.sleep(3_000); } catch (InterruptedException ie) { return; }
                }
            }
            if (stopping) return;
            java.util.List<String> up = new ArrayList<>(), down = new ArrayList<>();
            synchronized (this) {
                for (String p : bootSupervised.keySet()) {
                    AppRecord r = running.get(p);
                    if (r != null && r.thread.isBinderAlive()) up.add(p); else down.add(p);
                }
            }
            if (down.isEmpty()) {
                writeReady(String.join(" ", up), "");
                log("boot: system apps ready again (" + up.size() + ")");
            }
        }, "revive-" + proc);
        t.setDaemon(true);
        t.start();
    }

    void bootSystemServices() {
        java.util.List<String> bootNames = new ArrayList<>();
        java.util.List<Runnable> bootJobs = new ArrayList<>();
        final java.util.concurrent.CopyOnWriteArrayList<String> okNames = new java.util.concurrent.CopyOnWriteArrayList<>();
        final java.util.concurrent.CopyOnWriteArrayList<String> failedNames = new java.util.concurrent.CopyOnWriteArrayList<>();
        for (PackageRecord pr : new ArrayList<>(packages.values())) {
            if (!MacApkPackageManager.platformSystemApk(pr.pkg, pr.apkPath)) continue;
            // A maintenance window boots the Play Store too: it has a window of its own,
            // so an ordinary boot leaves it to its own launch, but with no app open its
            // BOOT_COMPLETED is how it schedules its update jobs again after Windows
            // restarted (on a phone every app with a boot receiver gets it at boot).
            boolean launchable = pr.manifest.launchableActivity != null;
            if (launchable && !MAINTENANCE) continue;
            // Every process that owns a boot receiver (AMS starts a receiver's
            // process for BOOT_COMPLETED). One process for a collapsed package.
            java.util.Set<String> bootProcs = new java.util.LinkedHashSet<>();
            for (String a : new String[] { "android.intent.action.LOCKED_BOOT_COMPLETED",
                                           "android.intent.action.BOOT_COMPLETED" }) {
                bootProcs.addAll(receiverProcesses(pr, new Intent(a).setPackage(pr.pkg)));
            }
            // AMS also starts a persistent system app's DEFAULT process at boot,
            // whether or not a boot receiver lives there. Play binds services in
            // Play services' default process; if only the receivers' process were
            // up, "ready" would be reported while the first bind still had a whole
            // JVM to boot, and Play's own bind timeout fired first (owner,
            // 2026-11-02: Play failed to open on a cold broker, the services
            // appeared only after it had tried; the second launch worked).
            if (!launchable) bootProcs.add(mainProcessOf(pr));
            for (String proc : bootProcs) {
                bootNames.add(proc);
                bootJobs.add(() -> {
                    // A start that fails once (a previous cycle's process still
                    // exiting, a slow attach) is retried before "ready" is
                    // written, so readiness means the services really are up.
                    for (int attempt = 1; attempt <= 3; attempt++) {
                        try {
                            ensureRunning(pr, proc);
                            // Revived if it dies; not the Play Store's, which the user may
                            // open (its window replaces this headless process).
                            if (!launchable) bootSupervised.put(proc, pr);
                            okNames.add(proc);
                            log("boot: started system app " + proc);
                            return;
                        } catch (Throwable e) {
                            log("boot: system app " + proc + " did not start (attempt " + attempt + "): " + e);
                            try { Thread.sleep(3_000); } catch (InterruptedException ie) { return; }
                        }
                    }
                    failedNames.add(proc);
                });
            }
        }
        final java.util.concurrent.CountDownLatch done = new java.util.concurrent.CountDownLatch(bootJobs.size());
        for (int i = 0; i < bootJobs.size(); i++) {
            final Runnable job = bootJobs.get(i);
            Thread t = new Thread(() -> { try { job.run(); } finally { done.countDown(); } }, "boot-" + bootNames.get(i));
            t.setDaemon(true);
            t.start();
        }
        Thread ready = new Thread(() -> {
            try { done.await(); } catch (InterruptedException e) { return; }
            writeReady(String.join(" ", okNames), String.join(" ", failedNames));
            bootDone = true;
            log("boot: system apps ready (" + okNames.size() + " of " + bootNames.size() + ")");
        }, "boot-ready");
        ready.setDaemon(true);
        ready.start();
    }

    /** The package declaring `authority` and its provider entry, or null. */
    private Object[] findProvider(String authority) {
        for (PackageRecord pr : packages.values()) {
            for (Manifest.ProviderEntry e : pr.manifest.providers) {
                if (e.authorities == null) continue;
                for (String a : e.authorities.split(";")) {
                    if (a.trim().equals(authority)) return new Object[] { pr, e };
                }
            }
        }
        return null;
    }

    /** AMS getContentProvider: resolve, enforce android:exported and the
     *  provider's permissions for another app (SecurityException, as
     *  ContentProviderHelper throws it), start the process if needed, and fetch
     *  the provider's binder. */
    private IBinder getProvider(String caller, String authority) {
        Object[] hit = authority == null ? null : findProvider(authority);
        if (hit == null) return null;
        PackageRecord pr = (PackageRecord) hit[0];
        Manifest.ProviderEntry e = (Manifest.ProviderEntry) hit[1];
        // ContentProviderHelper.checkContentProviderPermission: exported, and the caller holds the
        // read or write permission (or a path-permission's), or has a URI grant under the authority.
        String denial = perms.providerOpenDenial(caller, pr.pkg, e, who(Binder.getCallingPid()));
        if (denial != null) {
            log(denial);
            throw new SecurityException(denial);
        }
        try {
            AppRecord app = ensureRunning(pr, processOf(pr, e.name));
            Parcel d = Parcel.obtain(), r = Parcel.obtain();
            try {
                d.writeInterfaceToken(THREAD_DESCRIPTOR);
                d.writeString(authority);
                app.thread.transact(TX_ACQUIRE_PROVIDER, d, r, 0);
                r.readException();
                IBinder b = r.readStrongBinder();
                log("provider " + caller + " -> " + authority + " in " + pr.pkg + (b != null ? " OK" : " (none)"));
                return b;
            } finally { d.recycle(); r.recycle(); }
        } catch (Throwable t) {
            log("provider " + caller + " -> " + authority + " FAILED: " + t);
            return null;
        }
    }

    /** AMS startService for another app's service (the caller already got its
     *  synchronous answer from RESOLVE). */
    private void start(String caller, Intent intent) {
        String[] target = intent == null ? null : resolve(intent);
        if (target == null) return;
        Manifest.ServiceEntry se = packages.get(target[0]).manifest.services.get(target[1]);
        if (serviceDenial(caller, target, se) != null) return;
        grantFromIntent(caller, target[0], intent);
        try {
            AppRecord app = ensureServiceProcess(target[0], target[1]);
            Intent explicit = new Intent(intent);
            explicit.setComponent(new ComponentName(target[0], target[1]));
            Parcel d = Parcel.obtain(), r = Parcel.obtain();
            try {
                d.writeInterfaceToken(THREAD_DESCRIPTOR);
                d.writeTypedObject(explicit, 0);
                app.thread.transact(TX_SCHEDULE_START, d, r, 0);
                r.readException();
            } finally { d.recycle(); r.recycle(); }
            log("start " + caller + " -> " + target[0] + "/" + target[1] + " OK");
        } catch (Throwable t) {
            log("start " + caller + " -> " + target[0] + "/" + target[1] + " FAILED: " + t);
        }
    }

    /** AMS stopService for a service another process hosts. Only a RUNNING
     *  process can have a running service, so a stopped one is not started for
     *  it. */
    private boolean stopService(String caller, Intent intent) {
        String[] target = intent == null ? null : resolve(intent);
        if (target == null) return false;
        PackageRecord pr = packages.get(target[0]);
        Manifest.ServiceEntry se = pr.manifest.services.get(target[1]);
        // ContextImpl.stopServiceCommon: a service the caller may not use throws.
        if (serviceDenial(caller, target, se) != null) throw new SecurityException("Not allowed to stop service " + intent);
        AppRecord app;
        synchronized (this) { app = running.get(processOf(pr, target[1])); }
        if (app == null || !app.thread.isBinderAlive()) return false;
        Intent explicit = new Intent(intent);
        explicit.setComponent(new ComponentName(target[0], target[1]));
        Parcel d = Parcel.obtain(), r = Parcel.obtain();
        try {
            d.writeInterfaceToken(THREAD_DESCRIPTOR);
            d.writeTypedObject(explicit, 0);
            app.thread.transact(TX_SCHEDULE_STOP, d, r, 0);
            r.readException();
            boolean stopped = r.readInt() != 0;
            log("stop " + caller + " -> " + target[0] + "/" + target[1] + (stopped ? " OK" : " (was not running)"));
            return stopped;
        } catch (Throwable t) {
            log("stop " + caller + " -> " + target[0] + "/" + target[1] + " FAILED: " + t);
            return false;
        } finally { d.recycle(); r.recycle(); }
    }

    /** JobScheduler operations of an app that has several processes. The
     *  scheduler lives in each process (jobs are not persisted), and a job runs
     *  where its service is declared, so: schedule/enqueue go to that process
     *  (started if needed); cancel and the pending queries cover the app's OTHER
     *  processes (the caller handles its own). */
    private void job(Parcel data, Parcel reply) {
        String caller = callerPackage(data.readString());
        String callerProc = data.readString();
        int op = data.readInt();
        try {
            switch (op) {
                case JOB_SCHEDULE:
                case JOB_ENQUEUE: {
                    android.app.job.JobInfo info = data.readTypedObject(android.app.job.JobInfo.CREATOR);
                    android.app.job.JobWorkItem work = op == JOB_ENQUEUE
                        ? data.readTypedObject(android.app.job.JobWorkItem.CREATOR) : null;
                    ComponentName svc = info == null ? null : info.getService();
                    PackageRecord pr = svc == null ? null : packages.get(svc.getPackageName());
                    int rc = 0;
                    // A job is the app's own (JobScheduler only schedules for the
                    // caller's uid): another package's service is refused.
                    if (pr != null && pr.pkg.equals(caller)) {
                        AppRecord app = ensureServiceProcess(pr.pkg, svc.getClassName());
                        Parcel d = Parcel.obtain(), r = Parcel.obtain();
                        try {
                            d.writeInterfaceToken(THREAD_DESCRIPTOR);
                            d.writeInt(op);
                            d.writeTypedObject(info, 0);
                            if (op == JOB_ENQUEUE) d.writeTypedObject(work, 0);
                            app.thread.transact(TX_SCHEDULE_JOB, d, r, 0);
                            r.readException();
                            rc = r.readInt();
                        } finally { d.recycle(); r.recycle(); }
                        log("job " + (op == JOB_ENQUEUE ? "enqueue" : "schedule") + " id=" + info.getId()
                            + " " + caller + "(" + callerProc + ") -> " + app.proc + " rc=" + rc);
                    } else {
                        log("job from " + caller + " names a service it does not own: " + svc);
                    }
                    reply.writeNoException();
                    reply.writeInt(rc);
                    return;
                }
                case JOB_CANCEL:
                case JOB_CANCEL_ALL:
                case JOB_PENDING: {
                    int id = op == JOB_CANCEL || op == JOB_PENDING ? data.readInt() : -1;
                    List<android.app.job.JobInfo> pending = new ArrayList<>();
                    for (AppRecord app : processesOf(caller)) {
                        if (app.proc.equals(callerProc)) continue;
                        Parcel d = Parcel.obtain(), r = Parcel.obtain();
                        try {
                            d.writeInterfaceToken(THREAD_DESCRIPTOR);
                            d.writeInt(op);
                            if (op != JOB_CANCEL_ALL) d.writeInt(id);
                            app.thread.transact(TX_SCHEDULE_JOB, d, r, 0);
                            r.readException();
                            if (op == JOB_PENDING) {
                                List<android.app.job.JobInfo> l = r.createTypedArrayList(android.app.job.JobInfo.CREATOR);
                                if (l != null) pending.addAll(l);
                            }
                        } catch (Throwable t) {
                            log("job op " + op + " -> " + app.proc + " FAILED: " + t);
                        } finally { d.recycle(); r.recycle(); }
                    }
                    if (op == JOB_PENDING) {
                        // Jobs the broker holds for a process of the caller's that died are
                        // the app's pending jobs too (JobSchedulerService lists them).
                        for (android.app.job.JobInfo held : jobs().heldOf(caller)) {
                            if (id < 0 || held.getId() == id) pending.add(held);
                        }
                    }
                    reply.writeNoException();
                    if (op == JOB_PENDING) reply.writeTypedList(pending);
                    return;
                }
                default:
                    reply.writeException(new IllegalArgumentException("job op " + op));
            }
        } catch (Throwable t) {
            log("job op " + op + " from " + caller + " FAILED: " + t);
            reply.writeException(new IllegalStateException(String.valueOf(t)));
        }
    }

    // ---- JobSchedulerService (BrokerJobs) ---------------------------------------------

    /** This broker is a maintenance window (`winapk-runtime --system --maintenance`, which
     *  the launcher opens every few hours while it runs): it boots the system
     *  apps, runs the jobs that are due, and stops BrokerIdle.maintenanceGraceMs after
     *  the last one instead of waiting for an app that is not coming. */
    static final boolean MAINTENANCE = "1".equals(System.getenv("MACAPK_BROKER_MAINTENANCE"));
    /** How often held jobs are looked at (AOSP is push-based; the host's network and
     *  power state are polled here, as the app-side scheduler polls them). */
    static final long JOBS_POLL_MS = Long.getLong("macapk.broker.jobsPollMs", 15_000L);

    private BrokerJobs jobStore;
    /** Tests: where the store is written instead of systemDir()/jobs.bin. */
    File jobsFile;
    /** Last time a job was handed out or reported running or finished. */
    private volatile long lastJobActivityMs = System.currentTimeMillis();
    private final Object jobsWake = new Object();
    private volatile boolean jobsStarted;
    /** Set once bootSystemServices has written readiness: held jobs wait for it, because
     *  the Play Store's jobs talk to Play services, which the boot is starting. */
    volatile boolean bootDone;

    /** The store, read on first use. Its own lock, not the broker's: it is reached with the
     *  broker's and the package table's locks held. */
    BrokerJobs jobs() {
        synchronized (jobsLoadLock) {
            if (jobStore == null) {
                File f = jobsFile != null ? jobsFile : new File(systemDir(), "jobs.bin");
                jobStore = BrokerJobs.load(f, BrokerJobs.bootWallMs());
                if (jobStore.size() > 0) log("jobs: " + jobStore.size() + " held from before (" + f + ")");
            }
            return jobStore;
        }
    }
    private final Object jobsLoadLock = new Object();

    void saveJobs() {
        try { jobs().saveIfDirty(); } catch (Throwable t) { log("jobs: cannot write the store: " + t); }
    }

    private void wakeJobs() {
        synchronized (jobsWake) { jobsWake.notifyAll(); }
    }

    /** Win.apk's app standby: the broker starts a process for a held job only for the
     *  system apps (Google's Play Store, Play services, Services Framework: what keeps
     *  apps updated and push messages flowing). Another app's held job runs when that
     *  app runs, as a phone's rarely used apps get their jobs in the restricted bucket:
     *  each started process is a JVM. (Starting any app's, as Android does within its standby
     *  buckets, is for the owner to decide: docs/research/open-issues.md, "Decisions for the
     *  owner"; its opt-in switch is gone.) A field so the broker's test can replace it. */
    java.util.function.Predicate<String> mayStartForJob = pkg -> {
        PackageRecord pr = packages.get(pkg);
        return pr != null && MacApkPackageManager.platformSystemApk(pr.pkg, pr.apkPath);
    };

    /** May held job `j` run now: its package may be started for it, or the process it
     *  runs in is running anyway. */
    boolean jobRunnable(BrokerJobs.Job j) {
        if (mayStartForJob.test(j.pkg)) return true;
        synchronized (this) {
            AppRecord r = running.get(j.proc);
            return r != null && r.thread.isBinderAlive();
        }
    }

    /** A process died: the jobs it held are the broker's now. */
    private void jobsProcessDied(String pkg, String proc) {
        List<BrokerJobs.Job> held = jobs().processDied(pkg, proc, System.currentTimeMillis(), stopping);
        if (!held.isEmpty()) {
            log("jobs: " + proc + " died holding " + held.size() + " job(s); the system keeps them");
            wakeJobs();
        }
    }

    /** What a process says about the jobs it holds (SystemClient.reportJob). The package
     *  is the OS's answer (callerPackage); a job is only ever its own package's, and its
     *  process is the one the manifest puts its service in, never the caller's word. */
    private void jobReport(Parcel data) {
        String caller = callerPackage(data.readString());
        data.readString();   // the reporting process: informative only
        int op = data.readInt(), id = data.readInt();
        long value = data.readLong();
        int flags = data.readInt();
        android.app.job.JobInfo info = data.readTypedObject(android.app.job.JobInfo.CREATOR);
        long now = System.currentTimeMillis();
        switch (op) {
            case SystemClient.JOB_REPORT_PUT: {
                ComponentName svc = info == null ? null : info.getService();
                PackageRecord pr = packages.get(caller);
                if (svc == null || pr == null || !caller.equals(svc.getPackageName())
                        || !pr.manifest.services.containsKey(svc.getClassName())) {
                    log("jobs: " + caller + " reported job " + id + " for a service it does not declare: " + svc);
                    return;
                }
                jobs().put(caller, processOf(pr, svc.getClassName()), info, value, flags, now);
                return;
            }
            case SystemClient.JOB_REPORT_REMOVE:
                jobs().remove(caller, id);
                return;
            case SystemClient.JOB_REPORT_REMOVE_ALL:
                jobs().removeAll(caller);
                return;
            case SystemClient.JOB_REPORT_RUNNING:
                jobs().running(caller, id, value != 0, now);
                lastJobActivityMs = now;
                return;
            default:
                log("jobs: unknown report " + op + " from " + caller);
        }
    }

    /** APP_*: a package's processes as ActivityManager sees them. The empty package
     *  asks about every app: APP_OPEN while any app the user opened is (the PC in use,
     *  InstallConstraints' "device idle"). */
    synchronized int appState(String pkg) {
        if (pkg == null || pkg.isEmpty()) return openApps() > 0 ? APP_OPEN : APP_NOT_RUNNING;
        int state = APP_NOT_RUNNING;
        for (AppRecord r : running.values()) {
            if (!r.pkg.equals(pkg) || !r.thread.isBinderAlive()) continue;
            if (!spawned.containsKey((long) r.pid)) return APP_OPEN;
            state = APP_BACKGROUND;
        }
        return state;
    }

    /** The host's state, for AOSP's job constraint controllers. */
    private final BrokerJobs.Conditions conditions = new BrokerJobs.Conditions() {
        @Override public boolean networkUp() {
            try { return new android.net.ConnectivityManager().getActiveNetworkInfo() != null; }
            catch (Throwable t) { return true; }
        }
        @Override public boolean networkMetered() {
            try { return new android.net.ConnectivityManager().isActiveNetworkMetered(); }
            catch (Throwable t) { return false; }
        }
        @Override public boolean charging() { return HostPower.charging(); }
        @Override public int batteryPercent() { return HostPower.batteryPercent(); }
        @Override public long freeStorageBytes() {
            File d = systemDir();
            while (d != null && !d.exists()) d = d.getParentFile();
            return d == null ? -1 : d.getUsableSpace();
        }
        @Override public boolean idle() {
            synchronized (SystemServer.this) { return openApps() == 0; }
        }
    };

    /** Tests: the conditions to use instead of the host's. */
    BrokerJobs.Conditions conditionsOverride;

    BrokerJobs.Conditions hostConditions() { return conditionsOverride != null ? conditionsOverride : conditions; }

    /** The held-jobs loop: every JOBS_POLL_MS (or when a process dies), the held jobs
     *  that are ready are handed to their process, started for them. */
    void startJobs() {
        if (jobsStarted) return;
        jobsStarted = true;
        jobs();
        Thread t = new Thread(() -> {
            long waitUntil = System.currentTimeMillis() + 5 * 60_000L;
            while (!bootDone && System.currentTimeMillis() < waitUntil) {
                try { Thread.sleep(1_000); } catch (InterruptedException e) { return; }
            }
            while (!stopping) {
                for (BrokerJobs.Job j : jobs().takeReady(System.currentTimeMillis(), hostConditions(),
                        this::jobRunnable)) {
                    Thread d = new Thread(() -> dispatchJob(j), "job-" + j.pkg + "-" + j.id);
                    d.setDaemon(true);
                    d.start();
                }
                saveJobs();
                synchronized (jobsWake) {
                    try { jobsWake.wait(JOBS_POLL_MS); } catch (InterruptedException e) { return; }
                }
            }
        }, "broker-jobs");
        t.setDaemon(true);
        t.start();
    }

    /** Start the process a held job's service lives in and hand it the job, as
     *  JobSchedulerService binds a ready job's service, starting its process. */
    void dispatchJob(BrokerJobs.Job j) {
        lastJobActivityMs = System.currentTimeMillis();
        PackageRecord pr = packages.get(j.pkg);
        ComponentName svc = j.info.getService();
        if (pr == null || svc == null || !pr.manifest.services.containsKey(svc.getClassName())) {
            log("jobs: " + j.pkg + "#" + j.id + " names no installed service (" + svc + "); dropped");
            jobs().remove(j.pkg, j.id);
            return;
        }
        String proc = processOf(pr, svc.getClassName());
        boolean wasRunning;
        synchronized (this) {
            AppRecord r = running.get(proc);
            wasRunning = r != null && r.thread.isBinderAlive();
        }
        try {
            AppRecord app = ensureRunning(pr, proc);
            int rc;
            Parcel d = Parcel.obtain(), r = Parcel.obtain();
            try {
                d.writeInterfaceToken(THREAD_DESCRIPTOR);
                d.writeInt(JOB_RUN);
                d.writeTypedObject(j.info, 0);
                app.thread.transact(TX_SCHEDULE_JOB, d, r, 0);
                r.readException();
                rc = r.readInt();
            } finally { d.recycle(); r.recycle(); }
            if (rc == android.app.job.JobScheduler.RESULT_SUCCESS) jobs().dispatched(j);
            else jobs().dispatchFailed(j, System.currentTimeMillis());
            log("jobs: " + j.pkg + "#" + j.id + " (" + svc.getClassName() + ") is due -> " + proc
                    + (wasRunning ? "" : " (started for it)") + " rc=" + rc);
        } catch (Throwable t) {
            jobs().dispatchFailed(j, System.currentTimeMillis());
            log("jobs: " + j.pkg + "#" + j.id + " is due but " + proc + " did not take it: " + t);
        }
        lastJobActivityMs = System.currentTimeMillis();
        saveJobs();
    }

    /** ActiveServices.retrieveServiceLocked's check (checkComponentPermission): null when `caller`
     *  may use the service, "" when it is not exported to it, else the permission it lacks; a denial
     *  is logged as AMS logs it. */
    String serviceDenial(String caller, String[] target, Manifest.ServiceEntry se) {
        if (perms.checkComponent(se.permission, caller, target[0], se.exported)) return null;
        int pid = Binder.getCallingPid();
        String head = "Permission Denial: Accessing service " + target[0] + "/" + target[1] + " from pid=" + pid
                + ", uid=" + uidOfPid(pid);
        if (!se.exported) {
            log(head + " that is not exported from uid " + perms.uidOf(target[0]));
            return "";
        }
        log(head + " requires " + se.permission);
        return se.permission;
    }

    /** The package process `pid` is (attached, else what the OS says it is), or null. */
    String packageOfProcess(int pid) {
        String p = packageOfPid(pid);
        if (p == null) {
            String[] id = identityOfPid(pid);
            if (id != null) p = id[0];
        }
        return p;
    }

    /** TX_CHECK_PROVIDER: may process `pid` read (or write) `uri` of a provider of `self`'s?
     *  {BrokerPermissions.OP_*, the denial's tail}. The broker itself is the system. */
    Object[] checkProvider(String self, int pid, String uri, boolean write) {
        android.net.Uri u = uri == null ? null : android.net.Uri.parse(uri);
        Object[] hit = u == null || u.getAuthority() == null ? null : providerLookup.find(u.getAuthority());
        if (hit == null || !perms.sameApp(self, (String) hit[0])) {
            return new Object[] { BrokerPermissions.OP_DENIED, " (the uri's authority is not this provider's)" };
        }
        if (pid == ProcessHandle.current().pid()) return new Object[] { BrokerPermissions.OP_ALLOWED, null };
        String caller = packageOfProcess(pid);
        if (caller == null) {
            return new Object[] { BrokerPermissions.OP_DENIED, " (the calling process is no installed app's)" };
        }
        Object[] answer = perms.providerOp(caller, (String) hit[0], (Manifest.ProviderEntry) hit[1], u, write);
        if ((Integer) answer[0] == BrokerPermissions.OP_DENIED) {
            log("Permission Denial: " + (write ? "writing " : "reading ") + uri + " from " + caller + " " + who(pid)
                    + answer[1]);
        }
        return answer;
    }

    /** TX_CHECK_URI's answers: granted, denied, and no installed app's provider serves the uri. */
    static final int URI_GRANTED = 1, URI_DENIED = 0, URI_UNKNOWN = 2;

    /** TX_CHECK_URI: Context.checkUriPermission(uri, pid, uid, mode) for process `pid`. */
    int checkUri(int pid, String uri, int mode) {
        if (pid == ProcessHandle.current().pid()) return URI_GRANTED;
        android.net.Uri u = uri == null ? null : android.net.Uri.parse(uri);
        Object[] hit = u == null || u.getAuthority() == null ? null : providerLookup.find(u.getAuthority());
        if (hit == null) return URI_UNKNOWN;
        String caller = packageOfProcess(pid);
        if (caller == null) return URI_DENIED;
        return perms.uriAccess(caller, (String) hit[0], (Manifest.ProviderEntry) hit[1], u, mode)
                ? URI_GRANTED : URI_DENIED;
    }

    private void bind(String caller, Intent intent, Parcel reply) {
        String[] target = intent == null ? null : resolve(intent);
        if (target == null) {
            log("bind from " + caller + ": nothing resolves " + intent);
            reply.writeNoException(); reply.writeInt(0);
            return;
        }
        Manifest.ServiceEntry se = packages.get(target[0]).manifest.services.get(target[1]);
        // AMS: a service another app binds must be exported and its android:permission held
        // (same-app binds are always allowed); ContextImpl.bindServiceCommon throws.
        if (serviceDenial(caller, target, se) != null) {
            reply.writeException(new SecurityException("Not allowed to bind to service " + intent));
            return;
        }
        grantFromIntent(caller, target[0], intent);
        try {
            AppRecord app = ensureServiceProcess(target[0], target[1]);
            Intent explicit = new Intent(intent);
            explicit.setComponent(new ComponentName(target[0], target[1]));
            Parcel d = Parcel.obtain(), r = Parcel.obtain();
            IBinder service;
            try {
                d.writeInterfaceToken(THREAD_DESCRIPTOR);
                d.writeTypedObject(explicit, 0);
                app.thread.transact(TX_SCHEDULE_BIND, d, r, 0);
                r.readException();
                service = r.readStrongBinder();
            } finally { d.recycle(); r.recycle(); }
            log("bind " + caller + " -> " + target[0] + "/" + target[1]
                    + (service != null ? " OK" : " (onBind returned null)"));
            reply.writeNoException();
            reply.writeInt(1);
            reply.writeTypedObject(new ComponentName(target[0], target[1]), 0);
            reply.writeStrongBinder(service);
        } catch (Throwable t) {
            log("bind " + caller + " -> " + target[0] + "/" + target[1] + " FAILED: " + t);
            reply.writeNoException();
            reply.writeInt(0);
        }
    }

    /** Apps the user opened that are attached now: running processes this broker
     *  did not spawn. Caller holds the monitor. */
    private int openApps() {
        int n = 0;
        for (AppRecord r : running.values()) {
            if (r.thread.isBinderAlive() && !spawned.containsKey((long) r.pid)) n++;
        }
        return n;
    }

    /** Google services only while an Android app is open (owner, 2026-11-02:
     *  "I dont want google play services open 24/7 on these users macs"). Polls
     *  the open-app count; after BrokerIdle.graceMs() with none, stops every
     *  process this broker spawned (Play services, the Services Framework, their
     *  helpers) and exits. launchd keeps the Mach name, so the next app's lookup
     *  starts a fresh broker, which boots the services again. */
    private void watchIdle(long graceMs) {
        Thread t = new Thread(() -> {
            while (true) {
                try { Thread.sleep(5_000); } catch (InterruptedException e) { return; }
                boolean stop;
                long now = System.currentTimeMillis();
                synchronized (this) {
                    int n = openApps();
                    if (n > 0) lastClientMs = now;
                    long grace = sawOpenApp ? graceMs
                            : MAINTENANCE ? BrokerIdle.maintenanceGraceMs() : BrokerIdle.firstAppGraceMs(graceMs);
                    // A maintenance window (no app open) counts from the last job it ran.
                    long last = !sawOpenApp && MAINTENANCE ? Math.max(lastClientMs, lastJobActivityMs) : lastClientMs;
                    stop = BrokerIdle.shouldStop(n, last, now, grace);
                }
                // A job the broker handed out, or one running, keeps it up (an update the
                // Play Store is installing must not lose its process); in a maintenance
                // window so does a held job that may start now.
                if (stop && jobs().keepsBrokerUp(now, hostConditions(), MAINTENANCE, this::jobRunnable)) stop = false;
                if (stop && stopIdle()) return;
            }
        }, "broker-idle");
        t.setDaemon(true);
        t.start();
    }

    /** Stop the spawned processes and exit, unless an app opened meanwhile.
     *  @return true when the broker is exiting. */
    private boolean stopIdle() {
        log("idle: no open Android app for " + (BrokerIdle.graceMs() / 1000) + "s; stopping Google services");
        stopping = true;
        readyFile().delete();   // a stopped broker's readiness is nobody's
        List<Process> victims = new ArrayList<>(spawned.values());
        for (Process p : victims) p.destroy();   // SIGTERM, as the runtime's own shutdown expects
        long deadline = System.currentTimeMillis() + 5_000;
        for (Process p : victims) {
            try { p.waitFor(Math.max(1, deadline - System.currentTimeMillis()), TimeUnit.MILLISECONDS); }
            catch (InterruptedException e) { break; }
            if (p.isAlive()) { log("pid " + p.pid() + " ignored SIGTERM; killing"); p.destroyForcibly(); }
        }
        booted.clear();      // a broker that starts again is a new boot: every
        bootKicked.clear();  // system app gets its boot broadcasts again
        saveJobs();
        synchronized (this) {
            if (openApps() > 0) {
                // An app attached while the services were coming down. Its bind
                // would find nothing, so boot them again and keep serving.
                log("idle: an app opened during shutdown; booting the Google services again");
                lastClientMs = System.currentTimeMillis();
                bootSupervised.clear();
                stopping = false;
                bootSystemServices();
                return false;
            }
            log("idle: stopped " + victims.size() + " process(es); exiting (launchd starts the broker again on demand)");
        }
        Thread hard = new Thread(() -> {
            try { Thread.sleep(3_000); } catch (InterruptedException ignored) {}
            Runtime.getRuntime().halt(0);
        }, "broker-exit");
        hard.setDaemon(true);
        hard.start();
        System.exit(0);
        return true;
    }

    public static void main(String[] args) throws Exception {
        String name = System.getenv("MACAPK_SYSTEM_NAME");
        if (name != null && !name.isEmpty()) sName = name;
        // The broker's account service holds the Google account as AccountManagerService
        // does: it reads the master token (com.macapk.gms.GmsStore), no app does.
        com.macapk.gms.GmsStore.becomeOwner();
        SystemServer s = new SystemServer();
        String apks = System.getenv("MACAPK_BROKER_APKS");
        readyFile().delete();   // a previous broker's readiness is not this one's
        MacApkBinderTransport.setThreadPoolSize(16);
        // Win.apk: Binder.getCallingUid() of a caller is its registered
        // package's uid, from the pid the OS reports (osIdentity).
        if (osIdentity()) MacApkBinderTransport.setUidResolver(s::uidOfPid);
        // The name is served before the registry is read, and every call waits for that read
        // (awaitInstalledPackages): an install the launcher commits while the broker starts either
        // is in the file read below or reaches the broker after it (TX_PACKAGE_CHANGED), never
        // falls between the two.
        s.installedRead = new java.util.concurrent.CountDownLatch(1);
        MacApkBinderTransport.publish(sName, s.new Stub());
        synchronized (s.packagesLock) {
            if (apks != null && !apks.isBlank()) {
                for (String a : apks.split(":")) if (!a.isBlank()) s.addPackage(a.trim());
            } else {
                s.addInstalledPackages();
            }
        }
        s.installedRead.countDown();
        log("serving " + sName + " with " + s.packages.size() + " packages (pid "
                + ProcessHandle.current().pid() + ")");
        s.bootSystemServices();
        s.startJobs();
        boolean product = apks == null || apks.isBlank();
        if (MAINTENANCE) log("maintenance window: running the jobs that are due, then stopping");
        if (BrokerIdle.enabled(product)) {
            log("idle exit on: the broker stops " + (BrokerIdle.graceMs() / 1000) + "s after the last open app closes");
            s.watchIdle(BrokerIdle.graceMs());
        }
        Thread.sleep(Long.MAX_VALUE);
    }
}
