// The runtime's AppHost (com.macapk.loader.app.AppHost): Boot starts the app's process through it
// on AOSP's ActivityThread (AospApp), and the host's window reaches the task through it: closed
// (every activity destroyed, top first), minimized (the task behind), restored (the task in front).
package com.winapk.aospapp;

import com.macapk.loader.app.AppHost;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class AospAppHost implements AppHost {
    public static final AospAppHost INSTANCE = new AospAppHost();

    private AospAppHost() {}

    @Override public void start(Start s) { AospApp.start(s); }

    @Override public boolean started() { return AospApp.get() != null; }

    @Override public android.app.Activity topActivity() {
        AospApp app = AospApp.get();
        android.app.ActivityThread thread = android.app.ActivityThread.currentActivityThread();
        if (app == null || thread == null) return null;
        android.os.IBinder token = app.tasks.topToken();
        return token != null ? thread.getActivity(token) : null;
    }

    @Override public CountDownLatch terminate(Runnable topPaused) {
        CountDownLatch done = new CountDownLatch(1);
        AospApp app = AospApp.get();
        if (app == null) {
            if (topPaused != null) topPaused.run();
            done.countDown();
            return done;
        }
        app.tasks.handler.post(() -> app.tasks.terminate(done, topPaused));
        return done;
    }

    @Override public void hostMinimized() {
        AospApp app = AospApp.get();
        if (app != null) app.tasks.handler.post(app.tasks::toBack);
    }

    @Override public com.macapk.loader.SpecialAccessHandler.Cover hostCover(boolean opaque) {
        return new com.macapk.loader.SpecialAccessHandler.Cover() {
            @Override public void cover() {
                AospApp app = AospApp.get();
                if (app != null) app.tasks.handler.post(() -> app.tasks.cover(opaque));
            }

            @Override public void uncover() { uncoverWithResult(null); }

            @Override public void uncoverWithResult(Runnable result) {
                AospApp app = AospApp.get();
                if (app == null) {
                    if (result != null) result.run();
                    return;
                }
                app.tasks.handler.post(() -> app.tasks.uncover(result));
            }
        };
    }

    @Override public void configurationChanged(android.content.res.Configuration next) {
        AospApp app = AospApp.get();
        if (app == null) return;
        android.content.res.Configuration copy = new android.content.res.Configuration(next);
        app.tasks.handler.post(() -> app.tasks.configurationChanged(copy));
    }

    @Override public void deliverBroadcast(android.content.Intent intent, boolean fromOtherApp) {
        AospApp app = AospApp.get();
        if (app != null) app.broadcasts.deliverFromOutside(intent, fromOtherApp, null);
    }

    @Override public void deliverBroadcast(android.content.Intent intent, boolean fromOtherApp, String sender) {
        AospApp app = AospApp.get();
        if (app != null) app.broadcasts.deliverFromOutside(intent, fromOtherApp, sender);
    }

    @Override public android.os.IBinder bindServiceNow(android.content.Intent intent) {
        AospApp app = AospApp.get();
        return app != null ? app.services.bindNow(intent) : null;
    }

    @Override public android.app.Service jobBind(android.content.ComponentName service) {
        AospApp app = AospApp.get();
        return app != null ? app.services.jobBind(service) : null;
    }

    @Override public boolean jobUnbind(android.content.ComponentName service) {
        AospApp app = AospApp.get();
        return app != null && app.services.jobUnbind(service);
    }

    @Override public android.app.Service peekService(String className) {
        AospApp app = AospApp.get();
        return app != null ? app.services.peek(className) : null;
    }

    @Override public void finishActivities(java.util.Set<String> classNames) {
        AospApp app = AospApp.get();
        if (app != null) app.tasks.handler.post(() -> app.tasks.finishActivities(classNames));
    }

    @Override public java.util.List<android.content.Intent> pendingStarts() {
        AospApp app = AospApp.get();
        if (app == null) return java.util.List.of();
        if (app.tasks.handler.getLooper().isCurrentThread()) return app.tasks.pendingStarts();
        java.util.concurrent.FutureTask<java.util.List<android.content.Intent>> f =
                new java.util.concurrent.FutureTask<>(app.tasks::pendingStarts);
        app.tasks.handler.post(f);
        try {
            return f.get(2, TimeUnit.SECONDS);
        } catch (Exception e) {
            return java.util.List.of();
        }
    }

    @Override public void deliver(android.content.Intent intent, String sender) {
        AospApp app = AospApp.get();
        if (app == null) return;
        android.content.Intent copy = new android.content.Intent(intent);
        app.tasks.handler.post(() -> app.tasks.arrive(copy, sender));
    }

    @Override public void taskToFront() {
        AospApp app = AospApp.get();
        if (app != null) app.tasks.handler.post(app.tasks::toFront);
    }
}
