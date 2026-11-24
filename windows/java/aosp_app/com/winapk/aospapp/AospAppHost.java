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

    @Override public boolean terminate(long timeoutMs) {
        AospApp app = AospApp.get();
        if (app == null) return true;
        CountDownLatch done = new CountDownLatch(1);
        app.tasks.handler.post(() -> app.tasks.terminate(done));
        try {
            return done.await(timeoutMs, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
    }

    @Override public void hostMinimized() {
        AospApp app = AospApp.get();
        if (app != null) app.tasks.handler.post(app.tasks::toBack);
    }

    @Override public void taskToFront() {
        AospApp app = AospApp.get();
        if (app != null) app.tasks.handler.post(app.tasks::toFront);
    }
}
