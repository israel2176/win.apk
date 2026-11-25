// The runtime's door into the app layer (docs/research/aosp-app.md, step A0): starting the app's
// process on AOSP's own ActivityThread, and what the host asks of the app's task (the window
// closed, minimized, restored). Implemented in aosp-app.jar by com.winapk.aospapp.AospAppHost, over
// AOSP 14's ActivityThread and the in-process activity manager and task manager; Boot and the host's
// natives (native_cocoa.cpp, through ActivityHost's entries) reach it only through here, as the
// views are reached through ViewHost and the resources through ResBackend.
package com.macapk.loader.app;

import android.app.Application;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;

import java.util.function.Consumer;

public interface AppHost {

    /** What the system knows when it starts an app's process. */
    final class Start {
        /** The app's ApplicationInfo, as the package manager reports it. */
        public ApplicationInfo info;
        /** The process's configuration. */
        public Configuration config;
        /** The package manager the app's contexts hand out (MacApkPackageManager). */
        public PackageManager packageManager;
        /** The app's class loader (over its converted DEX). */
        public ClassLoader classLoader;
        /** The launcher activity's intent, which also starts again on an empty task brought back;
         *  null for a process started for a service (no activity). */
        public Intent launch;
        /** The intent the process starts with when another app's start or a URL opened it (its
         *  sender in `firstSender`); null: `launch`. */
        public Intent first;
        public String firstSender;
        /** The app's providers for this process (ActivityThread.installContentProviders creates,
         *  attaches and publishes them before Application.onCreate). */
        public java.util.List<android.content.pm.ProviderInfo> providers;
        /** Runs on the main thread after the Application is made and attached, before its providers
         *  are installed. */
        public Consumer<Application> afterApplicationAttach;
        /** Runs on the main thread right before Application.onCreate, after its providers. */
        public Consumer<Application> beforeApplicationOnCreate;
        /** The task's last activity is gone (Win.apk hides the window, as the home screen would
         *  show; the launcher activity starts again when the window comes back). */
        public Runnable taskEmpty;
        /** The app moved its task behind (Activity.moveTaskToBack, Back at the launcher's root
         *  activity): the window goes behind, as the home screen comes over it. */
        public Runnable moveToBack;
        /** A start no activity of the app resolves, offered to the host (its hand-offs to the
         *  browser, mail, the share sheet, an install, a settings page, a picker, the camera):
         *  true when the host took it, its result (if any) to the HostResult HostResults offers. */
        public java.util.function.Predicate<Intent> hostStart;
        /** The permission prompt (the permission controller's GrantPermissionsActivity on a
         *  device): asks the user for the permissions, the answer to the sink. */
        public java.util.function.BiConsumer<String[], com.macapk.loader.SpecialAccessHandler.PermissionSink>
                hostPermissions;
        /** The host window turned for an orientation that fixes a side (ActivityInfo's
         *  screenOrientation values; any other frees the window), as the display follows the
         *  activity whose box it shows. */
        public java.util.function.IntConsumer orientation;
        /** The window back to the user's rotation (the one the app was started in), freed. */
        public Runnable userOrientation;
    }

    /** ActivityThread.main's work on the calling thread, the main looper's: the system services
     *  in this process, ActivityThread attached, the application bound, the first activity
     *  launched as messages on the main looper. The caller loops. */
    void start(Start s);

    /** The task's top activity that has been launched (its instance in ActivityThread), or null:
     *  where the loader's input injection, gamepad and dumps send what they send to the app. */
    android.app.Activity topActivity();

    /** Whether start ran (the app's process is AOSP's). */
    boolean started();

    /** The window was closed: the task removed, as ActivityRecord.finishIfPossible and
     *  Task.removeActivities do: the resumed top paused first (`topPaused` runs once it has, or at
     *  once when nothing is resumed), then every activity stopped and destroyed from the top down.
     *  The latch counts down once the task is empty. */
    java.util.concurrent.CountDownLatch terminate(Runnable topPaused);

    /** The window was minimized: the task goes to the back (the resumed activity paused and
     *  stopped). */
    void hostMinimized();

    /** The window came back: the task comes to the front (restarted, started, resumed). */
    void taskToFront();

    /** A process of the app's other than the default one left (System.exit from a component
     *  declared android:process): its activities, by class name, finish, as the system removes a
     *  dead process's activities. */
    void finishActivities(java.util.Set<String> classNames);

    /** The starts the task manager has not launched yet (one waiting for the top to pause): what
     *  a process that leaves now still owes, which the next process starts. */
    java.util.List<Intent> pendingStarts();

    /** A broadcast another app sent (`fromOtherApp`: only exported receivers hear it) or the system
     *  broker relayed, to this process's receivers. */
    void deliverBroadcast(Intent intent, boolean fromOtherApp);

    /** A service of the app's bound for the host, which waits for its binder: one the system broker
     *  binds into this app for another, an account authenticator, the measurement service. The
     *  service is created if it was not and the binding held for the process's life; null when
     *  nothing of the app's answers or its onBind returned null. Any thread. */
    android.os.IBinder bindServiceNow(Intent intent);

    /** JobServiceContext's bind for one execution of a job: the service's instance (created if
     *  needed), on the main thread. */
    android.app.Service jobBind(android.content.ComponentName service);

    /** The end of that execution: true when it destroyed the service (nothing else held it). */
    boolean jobUnbind(android.content.ComponentName service);

    /** The live instance of the app's service `className`, or null. */
    android.app.Service peekService(String className);

    /** Another app's start of one of this app's activities, or a URL the host hands over, while the
     *  process runs: started as ActivityStarter starts an intent from outside the task. */
    void deliver(Intent intent, String sender);

    /** The cover a host window puts over the app (a prompt, a picker): the top activity paused,
     *  and stopped too when the window hides it, until the host brings it back, with a result. */
    com.macapk.loader.SpecialAccessHandler.Cover hostCover(boolean opaque);

    /** The display's configuration changed (the window resized or turned, the system's language
     *  or night mode): the process's, then each activity's, as WindowManagerService sends them. */
    void configurationChanged(Configuration next);

    /** aosp-app.jar's, or null when the class path has none (a plain JVM's test). */
    static AppHost get() { return Holder.HOST; }

    final class Holder {
        private Holder() {}
        static final AppHost HOST = load();

        private static AppHost load() {
            try {
                return (AppHost) Class.forName("com.winapk.aospapp.AospAppHost").getField("INSTANCE").get(null);
            } catch (ReflectiveOperationException | LinkageError e) {
                return null;
            }
        }
    }
}
