// The app's process started as Android starts it, on AOSP 14's own ActivityThread
// (docs/research/aosp-app.md): what ActivityThread.main does on the main thread, with the system side
// in this process. The activity manager, the activity task manager, the activity client controller
// and the package manager are registered with ServiceManager under their names; then a new
// ActivityThread attaches (ActivityThread.attach(false, seq): IActivityManager.attachApplication),
// the activity manager binds the application (handleBindApplication: LoadedApk, ContextImpl, the
// Application and its onCreate) and the task manager launches the first activity, each step a
// message on the main looper as on a device.
//
// The host supplies what system_server and the zygote know (com.macapk.loader.app.AppHost.Start):
// the app's ApplicationInfo, the process configuration, the app's class loader (Win.apk's, over the
// app's converted DEX), the PackageManager its contexts hand out (MacApkPackageManager, which also
// resolves intents), what to run right before Application.onCreate (the app's providers) and what to
// do when the task is empty. The main looper is the caller's; start() returns once the attach is
// sent, and the caller loops.
package com.winapk.aospapp;

import android.app.ActivityThread;
import android.app.Application;
import android.app.IActivityClientController;
import android.app.IActivityManager;
import android.app.IActivityTaskManager;
import android.app.IApplicationThread;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.res.Configuration;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.function.Consumer;

public final class AospApp {
    /** The one task's id (ActivityTaskManagerService numbers tasks from 1 on a fresh device). */
    static final int TASK_ID = 1;

    /** How an Intent resolves to an activity of the app: PackageManager.resolveActivity's answer. */
    public interface Resolver {
        ActivityInfo resolveActivity(Intent intent, String resolvedType);
    }

    private static volatile AospApp sInstance;

    final ApplicationInfo info;
    final Configuration config;
    final Resolver resolver;
    final ClassLoader classLoader;
    final PackageManager packageManager;
    final Consumer<Application> beforeOnCreate;
    final Tasks tasks;
    final IActivityClientController clientController;
    final Intent launch;
    /** The process's ApplicationThread once attached: what the system sends client transactions to. */
    volatile IApplicationThread appThread;
    final ActiveServices services = new ActiveServices(this);
    private Intent first;
    private String firstSender;
    private final Runnable onTaskEmpty;
    private Runnable onMoveToBack;
    java.util.function.Predicate<Intent> hostStart;
    java.util.function.IntConsumer orientation;
    Runnable userOrientation;
    java.util.function.BiConsumer<String[], com.macapk.loader.SpecialAccessHandler.PermissionSink> hostPermissions;

    private AospApp(ApplicationInfo info, Configuration config, Resolver resolver, ClassLoader cl,
                    PackageManager pm, Intent launch, Consumer<Application> beforeOnCreate, Runnable empty) {
        this.info = info;
        this.packageManager = pm;
        this.classLoader = cl;
        this.config = config;
        this.resolver = resolver != null ? resolver : (intent, type) -> resolve(pm, intent);
        this.launch = launch;
        this.beforeOnCreate = beforeOnCreate;
        this.onTaskEmpty = empty;
        HandlerThread system = new HandlerThread("winapk-system");
        system.start();
        this.tasks = new Tasks(new Handler(system.getLooper()));
        this.clientController = Services.of(IActivityClientController.class, new ActivityClientService(this));
        Services.register(Context.ACTIVITY_SERVICE, IActivityManager.class, new ActivityManagerService(this));
        Services.register("activity_task", IActivityTaskManager.class, new ActivityTaskService(this));
        Services.register("package", android.content.pm.IPackageManager.class, new PackageService(this));
    }

    /** PackageManager.resolveActivity, as ActivityTaskManagerService resolves a start through
     *  PackageManagerService (MATCH_DEFAULT_ONLY for an implicit intent). */
    static ActivityInfo resolve(PackageManager pm, Intent intent) {
        if (pm == null || intent == null) return null;
        int flags = intent.getComponent() != null ? 0 : PackageManager.MATCH_DEFAULT_ONLY;
        ResolveInfo ri = pm.resolveActivity(intent, flags);
        return ri != null ? ri.activityInfo : null;
    }

    public static AospApp get() { return sInstance; }

    /** The plain-JVM test's start: an explicit resolver, nothing before onCreate. */
    public static ActivityThread start(ApplicationInfo info, Configuration config, Resolver resolver,
                                       ClassLoader appClassLoader, PackageManager pm,
                                       Intent launch, Runnable onTaskEmpty) {
        return start(new AospApp(info, config, resolver, appClassLoader, pm, launch, null, onTaskEmpty));
    }

    /** The runtime's start (AospAppHost). */
    static ActivityThread start(com.macapk.loader.app.AppHost.Start s) {
        AospApp app = new AospApp(s.info, s.config, null, s.classLoader, s.packageManager, s.launch,
                s.beforeApplicationOnCreate, s.taskEmpty);
        app.onMoveToBack = s.moveToBack;
        app.orientation = s.orientation;
        app.userOrientation = s.userOrientation;
        app.first = s.first;
        app.firstSender = s.firstSender;
        app.hostStart = s.hostStart;
        app.hostPermissions = s.hostPermissions;
        return start(app);
    }

    /** ActivityThread.main's work, on the calling thread, which must have the main looper: the
     *  services up, a new ActivityThread attached. The caller then runs Looper.loop(). */
    private static ActivityThread start(AospApp app) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            throw new IllegalStateException("AospApp.start runs on the main looper's thread");
        }
        sInstance = app;
        System.err.println("[win.apk] AOSP app layer: AOSP 14's ActivityThread, Instrumentation and Activity,"
                + " over the in-process activity manager");
        try {
            // AospAppRewrite adds ActivityThread.winapkAttach (new ActivityThread(), attach(false, seq));
            // a method handle, as javac sees android-all's class without it.
            MethodHandle attach = MethodHandles.publicLookup().findStatic(ActivityThread.class, "winapkAttach",
                    MethodType.methodType(ActivityThread.class, long.class));
            return (ActivityThread) attach.invokeExact(0L);
        } catch (RuntimeException | Error e) {
            throw e;
        } catch (Throwable e) {
            throw new IllegalStateException("AOSP's ActivityThread did not attach", e);
        }
    }

    /** The app moved its task behind: the host's window goes behind too. */
    void movedToBack() {
        if (onMoveToBack != null) onMoveToBack.run();
    }

    /** The task is empty (its last activity destroyed). */
    void taskEmpty() {
        if (onTaskEmpty != null) onTaskEmpty.run();
    }

    /** The activity manager bound the application: the task manager starts the launch activity, as
     *  ActivityTaskManagerService.attachApplication does with the process's first activity. */
    void attached(IApplicationThread appThread) {
        this.appThread = appThread;
        tasks.handler.post(() -> {
            tasks.attach(this, appThread, clientController, config);
            if (first != null) tasks.arrive(first, firstSender);
            else if (launch != null) tasks.startLaunch(launch);
        });
    }
}
