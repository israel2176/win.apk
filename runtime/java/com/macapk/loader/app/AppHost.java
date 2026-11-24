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
        /** The first activity's intent; null for a process started for a service (no activity). */
        public Intent launch;
        /** Runs on the main thread after the Application is made and attached, right before its
         *  onCreate, where ActivityThread.handleBindApplication installs the app's providers. */
        public Consumer<Application> beforeApplicationOnCreate;
        /** The task's last activity is gone (Win.apk hides the window, as the home screen would
         *  show; the launcher activity starts again when the window comes back). */
        public Runnable taskEmpty;
        /** The app moved its task behind (Activity.moveTaskToBack, Back at the launcher's root
         *  activity): the window goes behind, as the home screen comes over it. */
        public Runnable moveToBack;
    }

    /** ActivityThread.main's work on the calling thread, the main looper's: the system services
     *  in this process, ActivityThread attached, the application bound, the first activity
     *  launched as messages on the main looper. The caller loops. */
    void start(Start s);

    /** Whether start ran (the app's process is AOSP's). */
    boolean started();

    /** The window was closed: every activity of the task paused, stopped and destroyed from the
     *  top down, as when the task is removed; true when that finished within `timeoutMs`. */
    boolean terminate(long timeoutMs);

    /** The window was minimized: the task goes to the back (the resumed activity paused and
     *  stopped). */
    void hostMinimized();

    /** The window came back: the task comes to the front (restarted, started, resumed). */
    void taskToFront();

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
