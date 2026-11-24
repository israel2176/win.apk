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
        /** Intents delivered to it while it was not resumed (onNewIntent before onResume). */
        final List<com.android.internal.content.ReferrerIntent> newIntents = new ArrayList<>();
        int requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
        /** The configuration the activity last got (ActivityRecord's last reported one). */
        Configuration reported;

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
    private AospApp owner;
    /** The task is behind (the window minimized, or moved back by the app or Back at its root). */
    private boolean inBackground;
    /** The window was closed: every activity goes, and the empty task ends the process. */
    private boolean terminating;
    private java.util.concurrent.CountDownLatch terminated;

    Tasks(Handler handler) { this.handler = handler; }

    void attach(AospApp owner, IApplicationThread thread, IActivityClientController controller, Configuration cfg) {
        this.owner = owner;
        app = thread;
        clientController = controller;
        config = cfg;
    }

    /** The process's first activity, or the launcher activity again when the window comes back to
     *  an empty task. */
    void startLaunch(Intent launch) {
        ActivityInfo ai = owner.resolver.resolveActivity(launch, null);
        if (ai == null) {
            System.err.println("[aosp-app] the launch intent resolves to no activity: " + launch);
            return;
        }
        Intent i = new Intent(launch);
        i.setComponent(new ComponentName(ai.packageName, ai.name));
        start(ai, i, owner.info.packageName, null, null, -1);
    }

    // ---- What the host asks (the window) ----

    /** ActivityTaskManagerService.removeTask for the window's close: every activity is destroyed,
     *  top first, from whatever state it is in (the executor passes through onPause and onStop);
     *  `done` counts down once the task is empty. */
    void terminate(java.util.concurrent.CountDownLatch done) {
        terminating = true;
        terminated = done;
        pendingLaunch = null;
        if (task.isEmpty()) {
            done.countDown();
            return;
        }
        for (int i = task.size() - 1; i >= 0; i--) {
            Record r = task.get(i);
            r.finishing = true;
            if (r.state != State.DESTROYING && r.state != State.DESTROYED) destroy(r);
        }
    }

    /** The task moved behind (Task.moveTaskToBack): the resumed top is paused and stopped. */
    void toBack() {
        if (inBackground || terminating) return;
        inBackground = true;
        Record r = top();
        if (r == null || r.state != State.RESUMED) return;
        r.state = State.STOPPING;
        ClientTransaction tx = ClientTransaction.obtain(app, r.token);
        tx.setLifecycleStateRequest(StopActivityItem.obtain(0));
        schedule(tx);
    }

    /** The task came to the front: its top restarted, started and resumed; an empty task starts its
     *  launcher activity again, as the home screen's icon would. */
    void toFront() {
        if (!inBackground || terminating) return;
        inBackground = false;
        Record r = top();
        if (r == null) {
            if (owner.launch != null) startLaunch(owner.launch);
            return;
        }
        if (r.state == State.STOPPED || r.state == State.STOPPING || r.state == State.PAUSED) resume(r);
    }

    boolean inBackground() { return inBackground; }

    /** ActivityTaskManagerService.updateGlobalConfigurationLocked, then each activity's
     *  ActivityRecord.ensureActivityConfiguration: the process gets the configuration
     *  (ConfigurationChangeItem: its Resources, the Application's onConfigurationChanged); an
     *  activity that declares every change in its configChanges gets it too
     *  (ActivityConfigurationChangeItem: onConfigurationChanged), any other is relaunched
     *  (ActivityRelaunchItem: destroyed and made again with its saved state) and brought back to the
     *  state it was in. */
    void configurationChanged(Configuration next) {
        if (config != null && config.diff(next) == 0 && config.equals(next)) return;
        config = new Configuration(next);
        ClientTransaction process = ClientTransaction.obtain(app, null);
        process.addCallback(android.app.servertransaction.ConfigurationChangeItem.obtain(new Configuration(next), 0));
        schedule(process);
        for (Record r : new ArrayList<>(task)) {
            if (!r.launched || r.finishing || r.state == State.DESTROYING || r.state == State.DESTROYED) continue;
            Configuration last = r.reported != null ? r.reported : next;
            int changes = last.diff(next) & ~CONFIG_WINDOW_CONFIGURATION;
            r.reported = new Configuration(next);
            if (changes == 0) continue;
            ClientTransaction tx = ClientTransaction.obtain(app, r.token);
            if ((changes & ~Compat.getRealConfigChanged(r.info)) != 0) {
                tx.addCallback(android.app.servertransaction.ActivityRelaunchItem.obtain(null, null, changes,
                        new android.util.MergedConfiguration(new Configuration(next), new Configuration()), true));
                tx.setLifecycleStateRequest(r.state == State.RESUMED
                        ? ResumeActivityItem.obtain(false, false)
                        : r.state == State.PAUSED || r.state == State.PAUSING
                                ? PauseActivityItem.obtain()
                                : StopActivityItem.obtain(0));
            } else {
                tx.addCallback(android.app.servertransaction.ActivityConfigurationChangeItem.obtain(
                        new Configuration(next)));
            }
            schedule(tx);
        }
    }

    /** ActivityInfo.CONFIG_WINDOW_CONFIGURATION (hidden): never a reason to relaunch. */
    private static final int CONFIG_WINDOW_CONFIGURATION = 0x20000000;

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

    /** startActivities: every record on the task in order, the last one started as a start is (the
     *  resumed activity paused first); the others wait, not launched, until revealed. */
    void startAll(ActivityInfo[] infos, Intent[] intents, String from) {
        Record prev = top();
        int last = infos.length - 1;
        for (int i = 0; i < last; i++) task.add(new Record(infos[i], intents[i], from, null, null, -1));
        Record r = new Record(infos[last], intents[last], from, null, null, -1);
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
        r.reported = new Configuration(config);
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
        if (!r.newIntents.isEmpty()) {
            tx.addCallback(android.app.servertransaction.NewIntentItem.obtain(new ArrayList<>(r.newIntents), true));
            r.newIntents.clear();
        }
        tx.setLifecycleStateRequest(ResumeActivityItem.obtain(false /* isForward */, false));
        r.state = State.RESUMED;
        schedule(tx);
    }

    // ---- Up navigation (Task.navigateUpTo, ActivityRecord.shouldUpRecreateTaskLocked) ----

    /** Whether Up from `r` must make a new task: only when the destination's affinity is not the
     *  task's (every activity of the app shares the app's here; a document task is not made). */
    boolean shouldUpRecreateTask(Record r, String destAffinity) {
        if (destAffinity == null) return true;
        String taskAffinity = owner.info.taskAffinity != null ? owner.info.taskAffinity : owner.info.packageName;
        return !destAffinity.equals(taskAffinity);
    }

    /** Task.navigateUpTo: the parent is the destination's record below `r` if there is one, else
     *  the activity under `r`; everything from `r` down to it finishes, the first with r's result;
     *  a parent found gets the intent (onNewIntent, for singleTop/singleTask/singleInstance or
     *  FLAG_ACTIVITY_CLEAR_TOP) or is replaced by a new instance started from it. */
    boolean navigateUpTo(Record r, Intent destIntent, int resultCode, Intent resultData) {
        int at = task.indexOf(r);
        if (at < 0) return false;
        ComponentName dest = destIntent != null ? destIntent.getComponent() : null;
        Record parent = below(r);
        boolean found = false;
        if (at > 0 && dest != null) {
            for (int i = at - 1; i >= 0; i--) {
                Record c = task.get(i);
                if (!c.finishing && c.info.packageName.equals(dest.getPackageName())
                        && c.info.name.equals(dest.getClassName())) {
                    parent = c;
                    found = true;
                    break;
                }
            }
        }
        int code = resultCode;
        Intent data = resultData;
        for (int i = at; i >= 0; i--) {
            Record c = task.get(i);
            if (c == parent) break;
            if (c.finishing) continue;
            finish(c, code, data);
            code = android.app.Activity.RESULT_CANCELED;
            data = null;
        }
        if (parent != null && found) {
            int mode = parent.info.launchMode;
            if (mode == ActivityInfo.LAUNCH_SINGLE_INSTANCE || mode == ActivityInfo.LAUNCH_SINGLE_TASK
                    || mode == ActivityInfo.LAUNCH_SINGLE_TOP
                    || (destIntent.getFlags() & Intent.FLAG_ACTIVITY_CLEAR_TOP) != 0) {
                parent.newIntents.add(new com.android.internal.content.ReferrerIntent(destIntent, r.info.packageName));
            } else {
                ActivityInfo ai = owner.resolver.resolveActivity(destIntent, null);
                if (ai == null) return false;
                start(ai, new Intent(destIntent), r.info.packageName, null, null, -1);
                finish(parent, code, data);
            }
        }
        return found;
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
        if (!task.isEmpty()) return;
        if (terminating) {
            if (terminated != null) terminated.countDown();
        } else {
            inBackground = true;
            owner.taskEmpty();
        }
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
