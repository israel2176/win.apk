// The app's task, as the system side keeps it on Android (ActivityTaskManagerService's ActivityRecords
// in a Task, ActivityTaskSupervisor, ClientLifecycleManager), for the one process Win.apk runs: which
// activities exist, in which order, in which state, and the client transactions that move them. The
// lifecycle itself is the app's own AOSP code (ActivityThread's TransactionExecutor and handlers);
// this only decides what to ask for and when, in the order WindowManagerService's side asks:
//
//   * A start pauses the resumed activity first (PauseActivityItem) and launches the new one when that
//     reports paused (LaunchActivityItem with ResumeActivityItem as its final state); the paused one
//     is stopped once the new one is idle (TaskFragment.resumeTopActivity, ActivityTaskSupervisor
//     .activityIdleInternal and its stopping list).
//   * A finish pauses the finishing activity if it is resumed, resumes the one under it with its
//     pending results first (ActivityResultItem before ResumeActivityItem, as
//     TaskFragment.resumeTopActivity adds them), and destroys the finishing one once the one under it
//     is idle (DestroyActivityItem; the executor passes through onStop on the way).
//   * The task's last activity destroyed ends the task: `onTaskEmpty` runs (Win.apk ends the run).
//
// Step A1 (docs/research/aosp-app.md): one task, the standard launch mode; launch modes, intent flags
// (CLEAR_TOP, SINGLE_TOP, NEW_TASK...), configuration changes and relaunch, moving the task to the
// back and front are step A2. Every method runs on the system thread (`handler`), as
// system_server's binder calls do; the client transactions go to the app's main thread through
// IApplicationThread.scheduleTransaction, as on Android.
package com.winapk.aospapp;

import android.app.IActivityClientController;
import android.app.IApplicationThread;
import android.app.ResultInfo;
import android.app.servertransaction.ActivityResultItem;
import android.app.servertransaction.ClientTransaction;
import android.app.servertransaction.DestroyActivityItem;
import android.app.servertransaction.LaunchActivityItem;
import android.app.servertransaction.PauseActivityItem;
import android.app.servertransaction.ResumeActivityItem;
import android.app.servertransaction.StopActivityItem;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.PersistableBundle;
import android.os.RemoteException;

import java.util.ArrayList;
import java.util.List;

public final class Tasks {
    /** AOSP's ActivityRecord.State, the part a client transaction moves. */
    enum State { INITIALIZING, RESUMED, PAUSING, PAUSED, STOPPING, STOPPED, DESTROYING, DESTROYED }

    /** One activity of the task (AOSP's ActivityRecord, the fields this step uses). */
    static final class Record {
        final IBinder token = new Binder();
        final IBinder assistToken = new Binder();
        final IBinder shareableToken = new Binder();
        final ActivityInfo info;
        final Intent intent;
        final String launchedFromPackage;
        final Record resultTo;
        final String resultWho;
        final int requestCode;
        State state = State.INITIALIZING;
        boolean finishing;
        boolean launched;
        Bundle icicle;
        PersistableBundle persistentState;
        final List<ResultInfo> results = new ArrayList<>();
        int requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;

        Record(ActivityInfo info, Intent intent, String from, Record resultTo, String resultWho, int requestCode) {
            this.info = info;
            this.intent = intent;
            this.launchedFromPackage = from;
            this.resultTo = resultTo;
            this.resultWho = resultWho;
            this.requestCode = requestCode;
        }

        ComponentName component() { return new ComponentName(info.packageName, info.name); }

        @Override public String toString() { return info.name + "{" + state + (finishing ? ",finishing" : "") + "}"; }
    }

    final Handler handler;
    private final List<Record> task = new ArrayList<>();       // bottom first
    private final List<Record> stopping = new ArrayList<>();   // paused, to stop or destroy on the next idle
    private Record pendingLaunch;                               // waits for the resumed one to pause
    private IApplicationThread app;
    private IActivityClientController clientController;
    private Configuration config;
    private Runnable onTaskEmpty;

    Tasks(Handler handler) { this.handler = handler; }

    void attach(IApplicationThread thread, IActivityClientController controller, Configuration cfg, Runnable empty) {
        app = thread;
        clientController = controller;
        config = cfg;
        onTaskEmpty = empty;
    }

    Record find(IBinder token) {
        for (Record r : task) if (r.token == token) return r;
        for (Record r : stopping) if (r.token == token) return r;
        return null;
    }

    Record top() {
        for (int i = task.size() - 1; i >= 0; i--) if (!task.get(i).finishing) return task.get(i);
        return null;
    }

    Record below(Record r) {
        int i = task.indexOf(r);
        for (int j = i - 1; j >= 0; j--) if (!task.get(j).finishing) return task.get(j);
        return null;
    }

    boolean isRoot(Record r) { return !task.isEmpty() && task.get(0) == r; }

    int size() { return task.size(); }

    // ---- Starting ----

    /** ActivityStarter.startActivityInner, for the standard launch mode in one task: the record goes
     *  on top; the resumed activity is paused first. */
    void start(ActivityInfo info, Intent intent, String from, Record resultTo, String resultWho, int requestCode) {
        Record r = new Record(info, intent, from, resultTo, resultWho, requestCode);
        Record prev = top();
        task.add(r);
        if (prev != null && prev.state == State.RESUMED) {
            pendingLaunch = r;
            pause(prev, true);
        } else {
            realStart(r);
        }
    }

    /** ActivityTaskSupervisor.realStartActivityLocked: launch, then resume, in one transaction. */
    private void realStart(Record r) {
        ClientTransaction tx = ClientTransaction.obtain(app, r.token);
        tx.addCallback(LaunchActivityItem.obtain(new Intent(r.intent), System.identityHashCode(r), r.info,
                new Configuration(config), new Configuration(), 0 /* deviceId */, r.launchedFromPackage,
                null /* voiceInteractor */, 2 /* PROCESS_STATE_TOP */, r.icicle, r.persistentState,
                r.results.isEmpty() ? null : new ArrayList<>(r.results), null /* pendingNewIntents */,
                null /* activityOptions */, true /* isForward */, null /* profilerInfo */, r.assistToken,
                clientController, r.shareableToken, false /* launchedFromBubble */, null /* taskFragmentToken */));
        r.results.clear();
        tx.setLifecycleStateRequest(ResumeActivityItem.obtain(true /* isForward */, false));
        r.launched = true;
        r.state = State.RESUMED;
        schedule(tx);
    }

    private void pause(Record r, boolean userLeaving) {
        r.state = State.PAUSING;
        ClientTransaction tx = ClientTransaction.obtain(app, r.token);
        tx.setLifecycleStateRequest(PauseActivityItem.obtain(r.finishing, userLeaving, 0, false, false));
        schedule(tx);
    }

    /** TaskFragment.resumeTopActivity for an activity already launched: its pending results, then
     *  resume (the executor passes through onRestart and onStart from stopped). */
    private void resume(Record r) {
        if (!r.launched) {
            realStart(r);
            return;
        }
        ClientTransaction tx = ClientTransaction.obtain(app, r.token);
        if (!r.results.isEmpty()) {
            tx.addCallback(ActivityResultItem.obtain(new ArrayList<>(r.results)));
            r.results.clear();
        }
        tx.setLifecycleStateRequest(ResumeActivityItem.obtain(false /* isForward */, false));
        r.state = State.RESUMED;
        schedule(tx);
    }

    // ---- Finishing ----

    /** ActivityRecord.finishIfPossible: the result goes to the activity that asked for it; a resumed
     *  activity is paused first; one already paused or stopped is destroyed once the next is idle. */
    boolean finish(Record r, int resultCode, Intent data) {
        if (r.finishing) return true;
        r.finishing = true;
        if (r.resultTo != null && r.requestCode >= 0) {
            r.resultTo.results.add(new ResultInfo(r.resultWho, r.requestCode, resultCode, data));
        }
        if (r.state == State.RESUMED) {
            pause(r, false);
        } else if (r.state == State.PAUSED || r.state == State.STOPPED) {
            destroy(r);
        }
        return true;
    }

    private void destroy(Record r) {
        r.state = State.DESTROYING;
        stopping.remove(r);
        ClientTransaction tx = ClientTransaction.obtain(app, r.token);
        tx.setLifecycleStateRequest(DestroyActivityItem.obtain(r.finishing, 0));
        schedule(tx);
    }

    // ---- What the app reports (IActivityClientController) ----

    void paused(IBinder token) {
        Record r = find(token);
        if (r == null) return;
        r.state = State.PAUSED;
        if (pendingLaunch != null) {
            Record next = pendingLaunch;
            pendingLaunch = null;
            stopping.add(r);
            realStart(next);
        } else if (r.finishing) {
            Record next = top();
            stopping.add(r);
            if (next != null) {
                resume(next);
            } else {
                // The task's last activity: nothing will be idle to wait for; what is finishing goes.
                for (Record s : new ArrayList<>(stopping)) if (s.finishing) destroy(s);
            }
        }
    }

    /** ActivityTaskSupervisor.activityIdleInternal: what waited for the top to be idle stops (or, if
     *  finishing, is destroyed). */
    void idle(IBinder token) {
        Record r = find(token);
        if (r == null || r != top()) return;
        for (Record s : new ArrayList<>(stopping)) {
            if (s.finishing) {
                destroy(s);
            } else if (s.state == State.PAUSED) {
                s.state = State.STOPPING;
                stopping.remove(s);
                ClientTransaction tx = ClientTransaction.obtain(app, s.token);
                tx.setLifecycleStateRequest(StopActivityItem.obtain(0));
                schedule(tx);
            }
        }
    }

    void stopped(IBinder token, Bundle icicle, PersistableBundle persistent) {
        Record r = find(token);
        if (r == null) return;
        if (r.state == State.STOPPING) r.state = State.STOPPED;
        r.icicle = icicle;
        r.persistentState = persistent;
    }

    void destroyed(IBinder token) {
        Record r = find(token);
        if (r == null) return;
        r.state = State.DESTROYED;
        task.remove(r);
        stopping.remove(r);
        if (task.isEmpty() && onTaskEmpty != null) onTaskEmpty.run();
    }

    private void schedule(ClientTransaction tx) {
        try {
            // ClientLifecycleManager.scheduleTransaction: the transaction goes to the client; one sent
            // to a local client is not recycled here (the client still holds it).
            tx.schedule();
        } catch (RemoteException e) {
            throw new IllegalStateException(e);
        }
    }
}
