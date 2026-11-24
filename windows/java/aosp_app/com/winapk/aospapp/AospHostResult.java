// A HostResult (com.macapk.loader.app) for an activity of the task: a result the host produced for a
// start it took over goes into the activity's record, and reaches the activity as any result does
// (ActivityResultItem before its next onResume; it is paused for it first when it is resumed).
package com.winapk.aospapp;

import android.content.Intent;
import android.os.IBinder;

import com.macapk.loader.app.HostResult;

final class AospHostResult implements HostResult {
    private final Tasks tasks;
    private final IBinder token;
    private final String who;
    private final int requestCode;

    AospHostResult(Tasks tasks, IBinder token, String who, int requestCode) {
        this.tasks = tasks;
        this.token = token;
        this.who = who;
        this.requestCode = requestCode;
    }

    @Override public int requestCode() { return requestCode; }

    @Override public void deliver(int resultCode, Intent data) {
        tasks.handler.post(() -> tasks.deliverResult(token, who, requestCode, resultCode, data, true));
    }

    @Override public void deliverNow(int resultCode, Intent data) {
        // A Cover's uncoverWithResult runs this on the system thread right before the resume that
        // carries it; from any other thread it is queued there.
        if (tasks.handler.getLooper().isCurrentThread()) {
            tasks.deliverResult(token, who, requestCode, resultCode, data, false);
        } else {
            tasks.handler.post(() -> tasks.deliverResult(token, who, requestCode, resultCode, data, false));
        }
    }
}
