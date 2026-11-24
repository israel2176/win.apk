// IActivityClientController in this process: what AOSP's ActivityClient reports and asks about one
// activity (ActivityClientController in system_server). The reports (resumed, paused, stopped, idle,
// destroyed) and finish move the task on the system thread (Tasks); the questions are answered from
// the activity's record. Methods with no answer here return their type's zero
// (com.winapk.aospapp.gen.IActivityClientControllerImpl logs each once).
package com.winapk.aospapp;

import android.content.ComponentName;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.IBinder;
import android.os.PersistableBundle;

import java.util.concurrent.Callable;
import java.util.concurrent.FutureTask;

public final class ActivityClientService {
    private final AospApp app;

    ActivityClientService(AospApp app) { this.app = app; }

    private Tasks tasks() { return app.tasks; }

    /** A question about the task, answered on the system thread as system_server answers it under
     *  its lock; the app's thread waits, as a binder call does. */
    private <T> T ask(Callable<T> q) {
        if (tasks().handler.getLooper().isCurrentThread()) {
            try { return q.call(); } catch (Exception e) { throw new IllegalStateException(e); }
        }
        FutureTask<T> f = new FutureTask<>(q);
        tasks().handler.post(f);
        try {
            return f.get();
        } catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }

    public void activityIdle(IBinder token, Configuration config, boolean stopProfiling) {
        tasks().handler.post(() -> tasks().idle(token));
    }

    public void activityResumed(IBinder token, boolean handleSplashScreenExit) {}

    public void activityTopResumedStateLost() {}

    public void activityPaused(IBinder token) {
        tasks().handler.post(() -> tasks().paused(token));
    }

    public void activityStopped(IBinder token, Bundle state, PersistableBundle persistentState, CharSequence description) {
        tasks().handler.post(() -> tasks().stopped(token, state, persistentState));
    }

    public void activityDestroyed(IBinder token) {
        tasks().handler.post(() -> tasks().destroyed(token));
    }

    public boolean finishActivity(IBinder token, int resultCode, Intent resultData, int finishTask) {
        return ask(() -> {
            Tasks.Record r = tasks().find(token);
            return r != null && tasks().finish(r, resultCode, resultData);
        });
    }

    public boolean isTopOfTask(IBinder token) {
        return ask(() -> {
            Tasks.Record r = tasks().find(token);
            return r != null && r == tasks().top();
        });
    }

    public int getTaskForActivity(IBinder token, boolean onlyRoot) {
        return ask(() -> {
            Tasks.Record r = tasks().find(token);
            if (r == null || onlyRoot && !tasks().isRoot(r)) return -1;
            return AospApp.TASK_ID;
        });
    }

    public int getDisplayId(IBinder token) { return 0; }

    public ComponentName getCallingActivity(IBinder token) {
        return ask(() -> {
            Tasks.Record r = tasks().find(token);
            return r != null && r.resultTo != null ? r.resultTo.component() : null;
        });
    }

    public String getCallingPackage(IBinder token) {
        return ask(() -> {
            Tasks.Record r = tasks().find(token);
            return r != null && r.resultTo != null ? r.resultTo.info.packageName : null;
        });
    }

    public String getLaunchedFromPackage(IBinder token) {
        return ask(() -> {
            Tasks.Record r = tasks().find(token);
            return r != null ? r.launchedFromPackage : null;
        });
    }

    public int getLaunchedFromUid(IBinder token) { return android.os.Process.myUid(); }

    public void setRequestedOrientation(IBinder token, int orientation) {
        tasks().handler.post(() -> {
            Tasks.Record r = tasks().find(token);
            if (r != null) tasks().requestOrientation(r, orientation);
        });
    }

    public int getRequestedOrientation(IBinder token) {
        return ask(() -> {
            Tasks.Record r = tasks().find(token);
            return r != null ? r.requestedOrientation : -1;
        });
    }

    public boolean willActivityBeVisible(IBinder token) { return true; }

    public boolean shouldUpRecreateTask(IBinder token, String destAffinity) {
        return ask(() -> {
            Tasks.Record r = tasks().find(token);
            return r == null || tasks().shouldUpRecreateTask(r, destAffinity);
        });
    }

    public boolean navigateUpTo(IBinder token, Intent destIntent, String resolvedType, int resultCode,
            Intent resultData) {
        return ask(() -> {
            Tasks.Record r = tasks().find(token);
            return r != null && tasks().navigateUpTo(r, destIntent, resultCode, resultData);
        });
    }

    /** ActivityClientController.onBackPressed: Back at the root activity of a task the launcher
     *  started (ACTION_MAIN, CATEGORY_LAUNCHER) moves the task behind instead of finishing it
     *  (Android 12's behaviour); any other activity is asked to finish (the callback runs
     *  finishAfterTransition on the app's side). */
    public void onBackPressed(IBinder token, android.app.IRequestFinishCallback callback) {
        tasks().handler.post(() -> {
            Tasks.Record r = tasks().find(token);
            if (r == null) return;
            Intent base = r.intent;
            boolean launcherRoot = tasks().isRoot(r) && base != null && Intent.ACTION_MAIN.equals(base.getAction())
                    && base.hasCategory(Intent.CATEGORY_LAUNCHER);
            if (launcherRoot) {
                moveToBack();
                return;
            }
            try {
                callback.requestFinish();
            } catch (android.os.RemoteException e) {
                throw new IllegalStateException(e);
            }
        });
    }

    /** ActivityClientController.moveActivityTaskToBack: refused for an activity that is not the
     *  task's root unless nonRoot. */
    public boolean moveActivityTaskToBack(IBinder token, boolean nonRoot) {
        return ask(() -> {
            Tasks.Record r = tasks().find(token);
            if (r == null || !nonRoot && !tasks().isRoot(r)) return false;
            moveToBack();
            return true;
        });
    }

    private void moveToBack() {
        tasks().toBack();
        app.movedToBack();
    }
}
