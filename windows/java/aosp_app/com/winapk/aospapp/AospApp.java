// The app's process started as Android starts it, on AOSP 14's own ActivityThread (WINAPK_AOSP_APP=1;
// docs/research/aosp-app.md, step A1): what ActivityThread.main does on the main thread, with the
// system side in this process. The activity manager, the activity task manager and the activity
// client controller are registered with ServiceManager under their names; then a new
// ActivityThread attaches (ActivityThread.attach(false, seq): IActivityManager.attachApplication),
// the activity manager binds the application (handleBindApplication: LoadedApk, ContextImpl, the
// Application and its onCreate) and the task manager launches the first activity, each step a
// message on the main looper as on a device.
//
// The host supplies what system_server and the zygote know: the app's ApplicationInfo, the process
// configuration, how an Intent resolves to an activity, the app's class loader (Win.apk's, over the
// app's converted DEX), the PackageManager its contexts hand out (MacApkPackageManager) and what to do
// when the task is empty.
// The main looper is the caller's; start() returns once the attach is sent, and the caller loops.
package com.winapk.aospapp;

import android.app.ActivityThread;
import android.app.IActivityClientController;
import android.app.IActivityManager;
import android.app.IActivityTaskManager;
import android.app.IApplicationThread;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.res.Configuration;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

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
    final android.content.pm.PackageManager packageManager;
    final Tasks tasks;
    final IActivityClientController clientController;
    private final Intent launch;
    private final Runnable onTaskEmpty;

    private AospApp(ApplicationInfo info, Configuration config, Resolver resolver, ClassLoader cl,
                    android.content.pm.PackageManager pm, Intent launch, Runnable empty) {
        this.info = info;
        this.packageManager = pm;
        this.classLoader = cl;
        this.config = config;
        this.resolver = resolver;
        this.launch = launch;
        this.onTaskEmpty = empty;
        HandlerThread system = new HandlerThread("winapk-system");
        system.start();
        this.tasks = new Tasks(new Handler(system.getLooper()));
        this.clientController = Services.of(IActivityClientController.class, new ActivityClientService(this));
        Services.register(Context.ACTIVITY_SERVICE, IActivityManager.class, new ActivityManagerService(this));
        Services.register("activity_task", IActivityTaskManager.class, new ActivityTaskService(this));
        Services.register("package", android.content.pm.IPackageManager.class, new PackageService(this));
    }

    public static AospApp get() { return sInstance; }

    /** ActivityThread.main's work, on the calling thread, which must have the main looper: the
     *  services up, a new ActivityThread attached. The caller then runs Looper.loop(). */
    public static ActivityThread start(ApplicationInfo info, Configuration config, Resolver resolver,
                                       ClassLoader appClassLoader, android.content.pm.PackageManager pm,
                                       Intent launch, Runnable onTaskEmpty) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            throw new IllegalStateException("AospApp.start runs on the main looper's thread");
        }
        AospApp app = new AospApp(info, config, resolver, appClassLoader, pm, launch, onTaskEmpty);
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

    /** The activity manager bound the application: the task manager starts the launch activity, as
     *  ActivityTaskManagerService.attachApplication does with the process's first activity. */
    void attached(IApplicationThread appThread) {
        tasks.handler.post(() -> {
            tasks.attach(appThread, clientController, config, onTaskEmpty);
            if (launch == null) return;
            ActivityInfo ai = resolver.resolveActivity(launch, null);
            if (ai == null) {
                System.err.println("[aosp-app] the launch intent resolves to no activity: " + launch);
                return;
            }
            Intent i = new Intent(launch);
            i.setComponent(new android.content.ComponentName(ai.packageName, ai.name));
            tasks.start(ai, i, info.packageName, null, null, -1);
        });
    }
}
