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
//   * The records outlive the app's process as they outlive it in system_server: each change is written
//     to the task's saved state (SavedTask), and the next process of the app restores the task from it,
//     the top relaunched with its saved Bundle and the others as they are revealed. The user dismissing
//     the task (`terminate`, the window closed) drops it; the end of the Windows session (`shutdown`)
//     stops every activity first, as ActivityTaskManagerService.shuttingDown puts them to sleep, so that
//     each saves its state.
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
        /** The activity its result goes to (null when started without a request code, or when its
         *  result was forwarded to the activity it started). */
        Record resultTo;
        String resultWho;
        int requestCode;
        State state = State.INITIALIZING;
        boolean finishing;
        boolean launched;
        Bundle icicle;
        PersistableBundle persistentState;
        final List<ResultInfo> results = new ArrayList<>();
        /** Intents delivered to it while it was not resumed (onNewIntent before onResume). */
        final List<com.android.internal.content.ReferrerIntent> newIntents = new ArrayList<>();
        /** ActivityRecord's orientation: the manifest's, until the activity requests its own. */
        int requestedOrientation;
        /** Its theme hides what is below, or it started an empty task: it owns the window's box. */
        boolean ownsBox;
        /** The configuration the activity last got (ActivityRecord's last reported one). */
        Configuration reported;
        /** ActivityRecord.mHaveState: true until it is launched, then again once it stops with its
         *  saved state, false from its resume on. A record without it does not survive the process
         *  (handleAppDied). */
        boolean haveState = true;
        /** The icicle and persistent state as the saved task writes them (SavedTask.marshall), made
         *  once when the activity reports them. */
        byte[] marshalledIcicle;
        byte[] marshalledPersistent;
        private byte[] marshalledIntent;

        byte[] marshalledIntent() {
            if (marshalledIntent == null) marshalledIntent = SavedTask.marshall(intent);
            return marshalledIntent;
        }

        Record(ActivityInfo info, Intent intent, String from, Record resultTo, String resultWho, int requestCode) {
            this.info = info;
            this.intent = intent;
            this.launchedFromPackage = from;
            this.resultTo = resultTo;
            this.resultWho = resultWho;
            this.requestCode = requestCode;
            this.requestedOrientation = info.screenOrientation;
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
    /** Where the records are kept across the process's end (null: not kept, as for a process started
     *  for a service, with no activity of its own). */
    private SavedTask saved;
    /** The user dismissed the task: nothing of it is kept from now on. */
    private boolean dismissed;
    /** The Windows session is ending: every activity is stopped, and nothing new starts. */
    private boolean shuttingDown;
    private java.util.concurrent.CountDownLatch shutDown;

    Tasks(Handler handler) { this.handler = handler; }

    void attach(AospApp owner, IApplicationThread thread, IActivityClientController controller, Configuration cfg,
                SavedTask saved) {
        this.owner = owner;
        app = thread;
        clientController = controller;
        config = cfg;
        this.saved = saved;
    }

    // ---- The task across the process's end (SavedTask) ----

    /** The records the last process of the app left, back in the task, none launched: what
     *  ActivityTaskManagerService still has of a task whose process died. True when there are any;
     *  the caller then resumes the top (`resumeTop`) or starts on top of them. */
    boolean restore() {
        if (saved == null) return false;
        List<SavedTask.Entry> entries = saved.read();
        List<Record> made = new ArrayList<>();
        for (SavedTask.Entry e : entries) {
            Record r = null;
            if (e.intent != null && e.activity != null) {
                Intent i = new Intent(e.intent);
                if (i.getComponent() == null) i.setComponent(new ComponentName(owner.info.packageName, e.activity));
                ActivityInfo ai = owner.resolver.resolveActivity(i, null);
                if (ai != null && ai.name.equals(e.activity)) {
                    Record to = e.resultTo >= 0 && e.resultTo < made.size() ? made.get(e.resultTo) : null;
                    r = new Record(ai, i, e.launchedFrom, to, to != null ? e.resultWho : null,
                            to != null ? e.requestCode : -1);
                    r.requestedOrientation = e.requestedOrientation;
                    r.icicle = e.icicle;
                    r.persistentState = e.persistentState;
                    r.marshalledIcicle = SavedTask.marshall(e.icicle);
                    r.marshalledPersistent = SavedTask.marshall(e.persistentState);
                } else {
                    System.err.println("[aosp-app] a saved activity the app no longer has: " + e.activity);
                }
            }
            made.add(r);   // null keeps the result targets' indices
        }
        StringBuilder names = new StringBuilder();
        for (Record r : made) {
            if (r == null) continue;
            windowOnStart(r, task.isEmpty());
            task.add(r);
            names.append(names.length() == 0 ? "" : ", ").append(r.info.name)
                    .append(r.icicle != null ? " (saved state)" : "");
        }
        persist();
        if (task.isEmpty()) return false;
        System.err.println("[aosp-app] the task restored after its process ended, bottom first: " + names);
        return true;
    }

    /** The restored task's top launched with its saved state and resumed (the task to the front). */
    void resumeTop() {
        Record t = top();
        if (t != null) resume(t);
    }

    /** Writes what of the task outlives the process: every record ActivityRecord.handleAppDied keeps. */
    private void persist() {
        if (saved == null || dismissed) return;
        List<Record> keep = new ArrayList<>();
        for (Record r : task) {
            if (r.finishing || !r.haveState || r.state == State.DESTROYING || r.state == State.DESTROYED) continue;
            // A launched record whose state could not be marshalled has none to come back with.
            if (r.launched && r.marshalledIcicle == null) continue;
            keep.add(r);
        }
        saved.write(keep);
    }

    /** RuntimeInit's handler reported the app's crash (ActivityManagerService.handleApplicationCrash):
     *  ActivityTaskManagerService.finishTopCrashedActivities finishes the task's top, so it does not
     *  come back; the rest keep what they saved. Written now: the process ends next. */
    void crashed() {
        Record t = top();
        if (t != null) t.haveState = false;
        persist();
    }

    /** The Windows session ends (ActivityTaskManagerService.shuttingDown, then
     *  ActivityTaskSupervisor.goingToSleepLocked): every activity that is resumed or paused is stopped,
     *  which saves its state; nothing is destroyed, the process just ends after. `done` counts down
     *  once every one has reported stopped. */
    void shutdown(java.util.concurrent.CountDownLatch done) {
        shuttingDown = true;
        shutDown = done;
        pendingLaunch = null;
        System.err.println("[aosp-app] the session ends: the task's activities stopped, their state kept");
        for (Record r : task) {
            if (!r.launched || r.finishing) continue;
            if (r.state == State.RESUMED || r.state == State.PAUSING || r.state == State.PAUSED) {
                r.state = State.STOPPING;
                stopping.remove(r);
                ClientTransaction tx = ClientTransaction.obtain(app, r.token);
                tx.setLifecycleStateRequest(StopActivityItem.obtain(0));
                schedule(tx);
            }
        }
        checkShutDown();
    }

    private void checkShutDown() {
        if (!shuttingDown || shutDown == null) return;
        for (Record r : task) if (!r.finishing && r.state == State.STOPPING) return;
        shutDown.countDown();
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

    /** A start from outside the task (another app's, a URL the host hands over), as ActivityStarter
     *  takes one: a MAIN/LAUNCHER intent for a task with activities brings it to the front, as the
     *  home screen's icon does; anything else starts as a start from inside would, its launch mode
     *  and flags deciding, with the task in front. */
    void arrive(Intent intent, String sender) {
        if (terminating || shuttingDown) return;
        boolean launcher = Intent.ACTION_MAIN.equals(intent.getAction())
                && intent.hasCategory(Intent.CATEGORY_LAUNCHER);
        if (launcher && top() != null) {
            toFront();
            return;
        }
        ActivityInfo ai = owner.resolver.resolveActivity(intent, intent.getType());
        if (ai == null) {
            System.err.println("[aosp-app] a start from " + sender + " resolves to no activity: " + intent);
            return;
        }
        Intent i = new Intent(intent);
        i.setComponent(new ComponentName(ai.packageName, ai.name));
        inBackground = false;
        start(ai, i, sender, null, null, -1);
    }

    /** The activities of a process that died (MacApkProcessExit): finished, as
     *  ActivityTaskManagerService removes a dead process's records. */
    void finishActivities(java.util.Set<String> classNames) {
        for (int i = task.size() - 1; i >= 0; i--) {
            Record r = task.get(i);
            if (!r.finishing && classNames.contains(r.info.name)) {
                finish(r, android.app.Activity.RESULT_CANCELED, null);
            }
        }
    }

    /** ActivityClientController.finishActivityAffinity: `r` and the activities under it in the task, top
     *  to bottom, until one of another task affinity (ActivityRecord.finishIfSameAffinity), each
     *  finished as finishIfPossible does, with RESULT_CANCELED. Activity.finishAffinity calls it; with no
     *  answer an app that closes itself so stayed open. */
    void finishAffinity(Record r) {
        String affinity = affinityOf(r);
        for (int i = task.indexOf(r); i >= 0; i--) {
            Record c = task.get(i);
            if (!java.util.Objects.equals(affinityOf(c), affinity)) break;
            finish(c, android.app.Activity.RESULT_CANCELED, null);
        }
    }

    /** An activity's task affinity: its own, else the application's, else the package (the manifest's
     *  defaults, as PackageParser fills ActivityInfo.taskAffinity). */
    private static String affinityOf(Record r) {
        if (r.info.taskAffinity != null) return r.info.taskAffinity;
        if (r.info.applicationInfo != null && r.info.applicationInfo.taskAffinity != null) {
            return r.info.applicationInfo.taskAffinity;
        }
        return r.info.packageName;
    }

    /** The starts not launched yet: records waiting for the top to pause. */
    java.util.List<Intent> pendingStarts() {
        java.util.List<Intent> out = new ArrayList<>();
        for (Record r : task) if (!r.launched && !r.finishing) out.add(new Intent(r.intent));
        return out;
    }

    // ---- What the host asks (the window) ----

    /** ActivityTaskManagerService.removeTask for the window's close. The visible top is paused
     *  first, as ActivityRecord.finishIfPossible pauses a resumed activity before it goes
     *  (`topPaused` then runs: the host may take its window off screen); then every activity is
     *  destroyed, top first, from whatever state it is in (the executor passes through onStop).
     *  `done` counts down once the task is empty. */
    void terminate(java.util.concurrent.CountDownLatch done, Runnable topPaused) {
        terminating = true;
        terminated = done;
        pendingLaunch = null;
        // The user dismissed the task (ActivityTaskManagerService.removeTask, a task swiped from
        // recents): nothing of it comes back.
        dismissed = true;
        if (saved != null) saved.delete();
        if (task.isEmpty()) {
            if (topPaused != null) topPaused.run();
            done.countDown();
            return;
        }
        for (Record r : task) r.finishing = true;
        Record t = task.get(task.size() - 1);
        if (t.state == State.RESUMED || t.state == State.PAUSING) {
            terminatePaused = topPaused != null ? topPaused : () -> {};
            if (t.state == State.RESUMED) pause(t, false);
            return;
        }
        if (topPaused != null) topPaused.run();
        destroyAll();
    }

    /** What runs once the top paused on the way out (terminate). */
    private Runnable terminatePaused;

    private void destroyAll() {
        for (int i = task.size() - 1; i >= 0; i--) {
            Record r = task.get(i);
            if (r.state != State.DESTROYING && r.state != State.DESTROYED) destroy(r);
        }
    }

    /** The task moved behind (Task.moveTaskToBack): the resumed top is paused and stopped. */
    void toBack() {
        if (inBackground || terminating) return;
        inBackground = true;
        System.err.println("[aosp-app] the task behind (the host's window minimized or the task moved back)");
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
        if (!inBackground || terminating || shuttingDown) return;
        inBackground = false;
        System.err.println("[aosp-app] the task in front again");
        Record r = top();
        if (r == null) {
            if (owner.launch != null) startLaunch(owner.launch);
            return;
        }
        if (r.state == State.STOPPED || r.state == State.STOPPING || r.state == State.PAUSED) resume(r);
    }

    boolean inBackground() { return inBackground; }

    // ---- The host's windows over the task (com.macapk.loader.SpecialAccessHandler.Cover) ----

    private Record covered;

    /** A window of the host's over the app (as an activity of the system's would be): the resumed
     *  top paused, and stopped too when the window hides it. */
    void cover(boolean opaque) {
        Record r = top();
        if (r == null || r.state != State.RESUMED) return;
        covered = r;
        if (opaque) {
            r.state = State.STOPPING;
            ClientTransaction tx = ClientTransaction.obtain(app, r.token);
            tx.setLifecycleStateRequest(StopActivityItem.obtain(0));
            schedule(tx);
        } else {
            pause(r, false);
        }
    }

    /** The host's window went: `result` (its HostResult's deliverNow) first, then the covered
     *  activity resumed with it, if it is still the top. */
    void uncover(Runnable result) {
        if (result != null) result.run();
        Record r = covered;
        covered = null;
        if (r != null && r == top() && !r.finishing && !inBackground) resume(r);
        else if (r == null) {
            Record t = top();
            if (t != null && t.state == State.RESUMED && !t.results.isEmpty()) resumeWithResults(t);
        }
    }

    /** ActivityRecord.sendResult: the result kept on the record; one that is resumed gets it now
     *  (paused for it, then resumed), any other at its next resume. */
    void deliverResult(IBinder token, String who, int requestCode, int resultCode, Intent data, boolean now) {
        Record r = find(token);
        if (r == null || r.finishing) return;
        r.results.add(new ResultInfo(who, requestCode, resultCode, data));
        if (now && r.state == State.RESUMED && covered != r) resumeWithResults(r);
    }

    private void resumeWithResults(Record r) {
        ClientTransaction tx = ClientTransaction.obtain(app, r.token);
        tx.addCallback(ActivityResultItem.obtain(new ArrayList<>(r.results)));
        r.results.clear();
        tx.setLifecycleStateRequest(ResumeActivityItem.obtain(false, false));
        schedule(tx);
    }

    /** ActivityTaskManagerService.updateGlobalConfigurationLocked, then each activity's
     *  ActivityRecord.ensureActivityConfiguration: the process gets the configuration
     *  (ConfigurationChangeItem: its Resources, the Application's onConfigurationChanged); an
     *  activity that declares every change in its configChanges gets it too
     *  (ActivityConfigurationChangeItem: onConfigurationChanged), any other is relaunched
     *  (ActivityRelaunchItem: destroyed and made again with its saved state) and brought back to the
     *  state it was in. An activity that is stopped (behind another, or the task minimized) is not
     *  told now: ensureActivityConfiguration skips an invisible one ("Skipping config check
     *  invisible"), and it gets the configuration when it comes back (`resume`). */
    void configurationChanged(Configuration next) {
        if (config != null && config.diff(next) == 0 && config.equals(next)) return;
        config = new Configuration(next);
        ClientTransaction process = ClientTransaction.obtain(app, null);
        process.addCallback(android.app.servertransaction.ConfigurationChangeItem.obtain(new Configuration(next), 0));
        schedule(process);
        for (Record r : new ArrayList<>(task)) {
            if (!r.launched || r.finishing || r.state == State.DESTROYING || r.state == State.DESTROYED) continue;
            if (r.state == State.STOPPING || r.state == State.STOPPED) continue;
            ClientTransaction tx = ClientTransaction.obtain(app, r.token);
            int kind = ensureConfiguration(tx, r, false);
            if (kind == 0) continue;
            if (kind == 2) {
                tx.setLifecycleStateRequest(r.state == State.RESUMED
                        ? ResumeActivityItem.obtain(false, false)
                        : PauseActivityItem.obtain());
            }
            schedule(tx);
        }
    }

    /** ActivityRecord.ensureActivityConfiguration for `r` against the task's configuration, into `tx`:
     *  0 when the activity has it already; else an ActivityConfigurationChangeItem
     *  (onConfigurationChanged) for a change its configChanges declares, or an ActivityRelaunchItem
     *  for any other, carrying its pending results and new intents when `andResume`
     *  (relaunchActivityLocked), which are then no longer pending. */
    private int ensureConfiguration(ClientTransaction tx, Record r, boolean andResume) {
        Configuration last = r.reported != null ? r.reported : config;
        int changes = last.diff(config) & ~CONFIG_WINDOW_CONFIGURATION;
        r.reported = new Configuration(config);
        if (changes == 0) return 0;
        if ((changes & ~Compat.getRealConfigChanged(r.info)) != 0) {
            // The window is kept across the relaunch only for a change of size
            // (isResizeOnlyChange); any other (a new density, a theme) makes it again, so nothing of
            // the old instance's views (a ComposeView found in the kept content and reused) outlives
            // it at the old configuration.
            boolean preserveWindow = isResizeOnlyChange(changes);
            List<ResultInfo> results = null;
            List<com.android.internal.content.ReferrerIntent> intents = null;
            if (andResume) {
                if (!r.results.isEmpty()) results = new ArrayList<>(r.results);
                if (!r.newIntents.isEmpty()) intents = new ArrayList<>(r.newIntents);
                r.results.clear();
                r.newIntents.clear();
            }
            tx.addCallback(android.app.servertransaction.ActivityRelaunchItem.obtain(results, intents, changes,
                    new android.util.MergedConfiguration(new Configuration(config), new Configuration()),
                    preserveWindow));
            return 2;
        }
        tx.addCallback(android.app.servertransaction.ActivityConfigurationChangeItem.obtain(new Configuration(config)));
        return 1;
    }

    /** ActivityInfo.CONFIG_WINDOW_CONFIGURATION (hidden): never a reason to relaunch. */
    private static final int CONFIG_WINDOW_CONFIGURATION = 0x20000000;

    /** ActivityRecord.isResizeOnlyChange: only the size, the orientation and the layout changed. */
    private static boolean isResizeOnlyChange(int change) {
        return (change & ~(android.content.pm.ActivityInfo.CONFIG_SCREEN_SIZE
                | android.content.pm.ActivityInfo.CONFIG_SMALLEST_SCREEN_SIZE
                | android.content.pm.ActivityInfo.CONFIG_ORIENTATION
                | android.content.pm.ActivityInfo.CONFIG_SCREEN_LAYOUT)) == 0;
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

    /** The top launched record's token, from any thread: asked on the system thread, which never
     *  waits for another (ActivityThread maps the token to the instance). */
    IBinder topToken() {
        java.util.concurrent.Callable<IBinder> q = () -> {
            for (int i = task.size() - 1; i >= 0; i--) {
                Record r = task.get(i);
                if (!r.finishing && r.launched) return r.token;
            }
            return null;
        };
        try {
            if (handler.getLooper().isCurrentThread()) return q.call();
            java.util.concurrent.FutureTask<IBinder> f = new java.util.concurrent.FutureTask<>(q);
            handler.post(f);
            return f.get();
        } catch (Exception e) {
            return null;
        }
    }

    /** The bottom record that is not finishing. */
    Record root() {
        for (Record r : task) if (!r.finishing) return r;
        return null;
    }

    boolean isRoot(Record r) { return !task.isEmpty() && task.get(0) == r; }

    int size() { return task.size(); }

    // ---- Starting ----

    /** ActivityStarter.executeRequest's result target for a start from `source`: the source itself
     *  when it asked for a result (a request code, and not finishing); with
     *  FLAG_ACTIVITY_FORWARD_RESULT the source's own target, which the source then no longer has
     *  (a trampoline passes its caller's request on). */
    void startFrom(ActivityInfo info, Intent intent, String from, Record source, String resultWho, int requestCode) {
        Record resultTo = null;
        if ((intent.getFlags() & Intent.FLAG_ACTIVITY_FORWARD_RESULT) != 0 && source != null) {
            resultTo = source.resultTo;
            resultWho = source.resultWho;
            requestCode = source.requestCode;
            source.resultTo = null;
            if (resultTo != null && resultTo.finishing) resultTo = null;
        } else if (source != null && requestCode >= 0 && !source.finishing) {
            resultTo = source;
        }
        if (resultTo == null) {
            resultWho = null;
            requestCode = -1;
        }
        start(info, intent, from, resultTo, resultWho, requestCode);
    }

    /** ActivityStarter.startActivityInner, for the standard launch mode in one task: the record goes
     *  on top; the resumed activity is paused first. */
    void start(ActivityInfo info, Intent intent, String from, Record resultTo, String resultWho, int requestCode) {
        try {
            startInner(info, intent, from, resultTo, resultWho, requestCode);
        } finally {
            persist();
        }
    }

    private void startInner(ActivityInfo info, Intent intent, String from, Record resultTo, String resultWho,
                            int requestCode) {
        int flags = intent.getFlags();
        int mode = info.launchMode;
        // ActivityStarter.computeLaunchingTaskFlags / startActivityInner, for the one task: what is
        // reused instead of a new record on top.
        if ((flags & Intent.FLAG_ACTIVITY_NEW_TASK) != 0 && (flags & Intent.FLAG_ACTIVITY_CLEAR_TASK) != 0) {
            // A new task in place of this one: everything in it finishes first.
            for (int i = task.size() - 1; i >= 0; i--) {
                Record c = task.get(i);
                if (!c.finishing) finish(c, android.app.Activity.RESULT_CANCELED, null);
            }
        } else {
            Record existing = null;
            boolean singleTaskLike = mode == ActivityInfo.LAUNCH_SINGLE_TASK
                    || mode == ActivityInfo.LAUNCH_SINGLE_INSTANCE;
            if ((flags & Intent.FLAG_ACTIVITY_CLEAR_TOP) != 0 || singleTaskLike
                    || (flags & Intent.FLAG_ACTIVITY_REORDER_TO_FRONT) != 0) {
                for (int i = task.size() - 1; i >= 0; i--) {
                    Record c = task.get(i);
                    if (!c.finishing && c.info.name.equals(info.name)) {
                        existing = c;
                        break;
                    }
                }
            }
            if (existing != null && (flags & Intent.FLAG_ACTIVITY_REORDER_TO_FRONT) != 0
                    && (flags & Intent.FLAG_ACTIVITY_CLEAR_TOP) == 0 && !singleTaskLike) {
                // Task.moveActivityToFront: the instance on top with the intent (onNewIntent).
                task.remove(existing);
                task.add(existing);
                existing.newIntents.add(new com.android.internal.content.ReferrerIntent(intent, from));
                bringToTop(existing);
                return;
            }
            if (existing != null) {
                // Task.performClearTop: everything above it finishes; a standard activity without
                // FLAG_ACTIVITY_SINGLE_TOP is itself finished and made again, any other gets the
                // intent (deliverNewIntent).
                for (int i = task.size() - 1; i >= 0; i--) {
                    Record c = task.get(i);
                    if (c == existing) break;
                    if (!c.finishing) finish(c, android.app.Activity.RESULT_CANCELED, null);
                }
                boolean reuse = mode != ActivityInfo.LAUNCH_MULTIPLE
                        || (flags & Intent.FLAG_ACTIVITY_SINGLE_TOP) != 0;
                if (reuse) {
                    existing.newIntents.add(new com.android.internal.content.ReferrerIntent(intent, from));
                    bringToTop(existing);
                    return;
                }
                finish(existing, android.app.Activity.RESULT_CANCELED, null);
            } else {
                Record t = top();
                if (t != null && t.info.name.equals(info.name) && (mode == ActivityInfo.LAUNCH_SINGLE_TOP
                        || (flags & Intent.FLAG_ACTIVITY_SINGLE_TOP) != 0)) {
                    // singleTop: the top instance gets the intent instead.
                    t.newIntents.add(new com.android.internal.content.ReferrerIntent(intent, from));
                    bringToTop(t);
                    return;
                }
            }
        }
        Record r = new Record(info, intent, from, resultTo, resultWho, requestCode);
        Record prev = top();
        windowOnStart(r, prev == null);
        task.add(r);
        if (prev != null && prev.state == State.RESUMED) {
            pendingLaunch = r;
            pause(prev, true);
        } else {
            realStart(r);
        }
    }

    /** A record already in the task is the top again with its new intents: delivered now when it is
     *  resumed (NewIntentItem pauses and resumes it around onNewIntent), else at its resume (after
     *  the finishing ones above it pause). */
    private void bringToTop(Record r) {
        if (r.state == State.RESUMED && !r.newIntents.isEmpty()) {
            ClientTransaction tx = ClientTransaction.obtain(app, r.token);
            tx.addCallback(android.app.servertransaction.NewIntentItem.obtain(new ArrayList<>(r.newIntents), true));
            r.newIntents.clear();
            schedule(tx);
            return;
        }
        Record t = topResumedOther(r);
        if (t != null) {
            // The one above is finishing and pausing: `r` resumes when it reports paused.
            return;
        }
        if (r.state != State.RESUMED) resume(r);
    }

    /** A resumed record other than `r` (one being paused on the way). */
    private Record topResumedOther(Record r) {
        for (Record c : task) if (c != r && (c.state == State.RESUMED || c.state == State.PAUSING)) return c;
        return null;
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
        persist();
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
        // Resumed, it has no saved state (ActivityRecord.activityResumedLocked: setSavedState(null)).
        forgetState(r);
    }

    private void forgetState(Record r) {
        r.haveState = false;
        r.icicle = null;
        r.marshalledIcicle = null;
        persist();
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
        windowOnRevealed();
        if (!r.launched) {
            realStart(r);
            return;
        }
        ClientTransaction tx = ClientTransaction.obtain(app, r.token);
        // A configuration it missed while stopped comes first (a relaunch takes the pending results
        // and intents with it).
        ensureConfiguration(tx, r, true);
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
        forgetState(r);
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
        } else if (r.state == State.PAUSED || r.state == State.STOPPED || !r.launched && r != pendingLaunch) {
            destroy(r);
        }
        persist();
        return true;
    }

    private void destroy(Record r) {
        if (!r.launched) {
            // A record never launched (one startActivities put under the top): the client has no
            // activity for it, so it leaves the task at once (ActivityRecord.destroyImmediately for a
            // record with no process).
            r.state = State.DESTROYED;
            destroyed(r.token);
            return;
        }
        r.state = State.DESTROYING;
        stopping.remove(r);
        ClientTransaction tx = ClientTransaction.obtain(app, r.token);
        tx.setLifecycleStateRequest(DestroyActivityItem.obtain(r.finishing, 0));
        schedule(tx);
    }

    // ---- The host window's orientation (DisplayContent.updateOrientation, for one window) ----

    /** The record whose box the window shows. */
    private Record boxOwner;

    /** The topmost live record that owns a box. */
    private Record topBoxOwner() {
        for (int i = task.size() - 1; i >= 0; i--) {
            Record c = task.get(i);
            if (!c.finishing && c.ownsBox) return c;
        }
        return null;
    }

    /** A starting activity whose theme hides what is below (or the first of an empty task) owns the
     *  box: the window takes its orientation. A translucent one inherits the box below. */
    private void windowOnStart(Record r, boolean emptyTask) {
        r.ownsBox = emptyTask || occludesParent(r.info);
        if (!r.ownsBox) return;
        boxOwner = r;
        if (owner.orientation != null) owner.orientation.accept(r.requestedOrientation);
    }

    /** An activity revealed as the box owner (the one above it finished): a fixed side is applied
     *  again, a free one returns the window to the user's rotation when the one that went had fixed
     *  the side. */
    private void windowOnRevealed() {
        Record o = topBoxOwner();
        Record was = boxOwner;
        if (o == null || o == was) return;
        boxOwner = o;
        if (fixedSide(o.requestedOrientation)) {
            if (owner.orientation != null) owner.orientation.accept(o.requestedOrientation);
        } else if (was != null && fixedSide(was.requestedOrientation) && owner.userOrientation != null) {
            owner.userOrientation.run();
        }
    }

    /** Activity.setRequestedOrientation (ActivityRecord.setRequestedOrientation): on its record, and
     *  on the window when the record shows the box, or is on top of it. */
    void requestOrientation(Record r, int orientation) {
        r.requestedOrientation = orientation;
        persist();
        if (r.finishing || owner.orientation == null) return;
        Record o = topBoxOwner();
        if (r == o || r == top()) owner.orientation.accept(orientation);
    }

    /** ActivityRecord.occludesParent from its theme (AttributeCache's Window entry,
     *  ActivityInfo.isTranslucentOrFloating): not translucent, floating, swipe-to-dismiss or
     *  no-display. Read through the application's resources, which hold the app's styles. */
    static boolean occludesParent(ActivityInfo info) {
        int theme = info.getThemeResource();
        android.app.Application app = android.app.ActivityThread.currentApplication();
        if (theme == 0 || app == null) return true;
        try {
            android.content.res.Resources.Theme t = app.getResources().newTheme();
            t.applyStyle(theme, true);
            android.content.res.TypedArray a = t.obtainStyledAttributes(new int[] {
                    android.R.attr.windowIsTranslucent, android.R.attr.windowIsFloating,
                    android.R.attr.windowSwipeToDismiss, android.R.attr.windowNoDisplay});
            boolean translucent = a.getBoolean(0, false);
            boolean floating = a.getBoolean(1, false);
            boolean swipeToDismiss = a.getBoolean(2, false) && !a.hasValue(0);
            boolean noDisplay = a.getBoolean(3, false);
            a.recycle();
            return !translucent && !floating && !swipeToDismiss && !noDisplay;
        } catch (RuntimeException e) {
            return true;   // a theme the resources cannot read: opaque, AOSP's default
        }
    }

    /** The landscape and portrait families: the values that fix a side. */
    static boolean fixedSide(int o) {
        switch (o) {
            case ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE:
            case ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE:
            case ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE:
            case ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE:
            case ActivityInfo.SCREEN_ORIENTATION_PORTRAIT:
            case ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT:
            case ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT:
            case ActivityInfo.SCREEN_ORIENTATION_USER_PORTRAIT:
                return true;
            default:
                return false;
        }
    }

    // ---- What the app reports (IActivityClientController) ----

    void paused(IBinder token) {
        Record r = find(token);
        if (r == null) return;
        if (shuttingDown) {
            // Stopped next (shutdown asked it): nothing starts or resumes now.
            if (r.state == State.PAUSING) r.state = State.PAUSED;
            return;
        }
        r.state = State.PAUSED;
        if (terminating) {
            Runnable p = terminatePaused;
            terminatePaused = null;
            if (p != null) {
                p.run();
                destroyAll();
            }
            return;
        }
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
        if (r == null || r != top() || shuttingDown) return;
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
        // ActivityRecord.activityStopped: a stop that saved state (an icicle; there is none for one
        // finishing) gives the record its state; a persistent state is kept whenever one comes.
        if (icicle != null) {
            r.icicle = icicle;
            r.marshalledIcicle = SavedTask.marshall(icicle);
            r.haveState = true;
        }
        if (persistent != null) {
            r.persistentState = persistent;
            r.marshalledPersistent = SavedTask.marshall(persistent);
        }
        persist();
        checkShutDown();
    }

    void destroyed(IBinder token) {
        Record r = find(token);
        if (r == null) return;
        r.state = State.DESTROYED;
        task.remove(r);
        stopping.remove(r);
        persist();
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
