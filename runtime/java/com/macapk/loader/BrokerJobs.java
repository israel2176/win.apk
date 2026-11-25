// com.macapk.loader.BrokerJobs -- the system broker's half of JobSchedulerService:
// the job store that outlives an app's process.
//
// WHY IT EXISTS. On a device the jobs an app schedules live in system_server
// (JobSchedulerService's JobStore), not in the app: the app's process may die
// at any time and the system starts it again when one of its jobs is due. That
// is how the Play Store updates apps with nothing on screen: its daily jobs run
// in a process the system started for them. Win.apk's JobScheduler
// (android.app.job.MacApkJobScheduler) runs in the app's own process, so before
// this every job of an app died with it, and nothing ran for an app that was
// not open: the Play Store's update checks ran only while its window was.
//
// HOW. The app's scheduler stays the one that runs its jobs while the process
// lives (nothing changes in how a job executes). With the broker on, it also
// reports every job it holds here (SystemClient.reportJob: put with the next
// run time, running, finished, removed). When the process that held a job dies,
// the broker keeps it ("held"), as JobSchedulerService keeps it: once it is due
// and its constraints hold (AOSP's controllers, asked of the host: network and
// an unmetered one, charging, battery not low, storage not low, idle), the
// broker starts the process its service lives in, headless, as it starts one
// for a bind, and hands it the job to run now (SystemServer, JOB_RUN); from
// then on that process reports it again.
//
// PERSISTENCE (AOSP JobStore, /data/system/job/jobs.xml). The store is written
// to <system_ce>/jobs.bin. A job built with setPersisted(true) survives a
// reboot, as on Android. The broker itself stops a minute after the last app
// closes and starts again with the next one, which is not a reboot: a job that
// is not persisted is kept across that too, and dropped only when Windows
// itself has restarted since it was written (the boot time below), as Android
// drops it at a reboot.
//
// NOT HELD: a job with enqueued work (JobWorkItems are not persisted on Android
// either and die with the process here) and a content-trigger job (its
// observer lives in the app). Both keep running in the app's process as before.
//
// This class is the store and its rules only, with no process or binder state,
// so a test drives it with its own clock and conditions
// (windows/test/java/com/macapk/loader/BrokerJobsTest.java).
package com.macapk.loader;

import android.app.job.JobInfo;
import android.os.Parcel;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

final class BrokerJobs {
    /** What a process tells the broker about one of its jobs (SystemClient.reportJob). */
    static final int REPORT_PUT = 0, REPORT_REMOVE = 1, REPORT_REMOVE_ALL = 2, REPORT_RUNNING = 3,
            REPORT_FINISHED = 4;
    /** REPORT_PUT flags: the job carries enqueued work / waits on a content change. */
    static final int FLAG_WORK = 1, FLAG_CONTENT_TRIGGER = 2;

    /** AOSP JobServiceContext's execution limit for a regular job (10 minutes); a job the
     *  process says is still running after this no longer keeps the broker up. */
    static final long MAX_RUNNING_HOLD_MS = 10 * 60_000L;
    /** A dispatch that failed (the process did not start) is tried again after this,
     *  doubling, up to an hour: a broken app must not be started every few seconds. */
    static final long DISPATCH_RETRY_MS = 60_000L, DISPATCH_RETRY_MAX_MS = 60 * 60_000L;
    /** Two boot times this far apart are two boots (the estimate below wanders by the
     *  clock's adjustments, never by minutes). */
    static final long SAME_BOOT_SLACK_MS = 5 * 60_000L;
    /** AOSP StorageController's "storage low" floor, as the app-side scheduler uses it. */
    static final long STORAGE_LOW_BYTES = 500L * 1024 * 1024;

    private static final int FILE_MAGIC = 0x57414a31;   // "WAJ1"

    static final class Job {
        final String pkg;
        final int id;
        JobInfo info;
        /** The process its JobService runs in (the manifest's, never the caller's word). */
        String proc;
        /** When it may run next, wall clock (AOSP's earliest run time). */
        long dueWallMs;
        /** AOSP's override deadline, wall clock, 0 = none: past it, constraints are ignored. */
        long deadlineWallMs;
        int flags;
        /** No live process holds it: the broker starts one when it is due. */
        boolean held;
        /** A process is running it now (its onStartJob was called and it has not finished). */
        boolean running;
        long runningSinceMs;
        /** The broker is starting its process / handing it over right now. */
        boolean dispatching;
        int dispatchFailures;
        long retryAtMs;

        Job(String pkg, int id) { this.pkg = pkg; this.id = id; }

        String key() { return BrokerJobs.key(pkg, id); }

        @Override public String toString() {
            return pkg + "#" + id + (held ? " held" : "") + (running ? " running" : "")
                    + " due=" + dueWallMs + " proc=" + proc;
        }
    }

    static String key(String pkg, int id) { return pkg + "/" + id; }

    /** The host's state for AOSP's constraint controllers. */
    interface Conditions {
        boolean networkUp();
        boolean networkMetered();
        boolean charging();
        /** Battery percent, or -1 when there is none (a desktop PC). */
        int batteryPercent();
        long freeStorageBytes();
        /** AOSP IdleController: the device is not in use. Here: no app the user opened is open. */
        boolean idle();
    }

    private final Map<String, Job> jobs = new LinkedHashMap<>();
    private final File file;
    /** When this Windows boot started, wall clock (an estimate, see bootWallMs). */
    private final long bootMs;
    private boolean dirty;

    BrokerJobs(File file, long bootMs) {
        this.file = file;
        this.bootMs = bootMs;
    }

    /** When the machine booted, from the JVM's monotonic clock: System.nanoTime is
     *  QueryPerformanceCounter on Windows (CLOCK_MONOTONIC elsewhere), which counts
     *  from the boot. */
    static long bootWallMs() {
        return System.currentTimeMillis() - System.nanoTime() / 1_000_000L;
    }

    // ---- what the app processes report -------------------------------------------

    /** AOSP schedule(): a job (re)scheduled, or re-armed after a run, by the process
     *  that runs it. It is that process's again, not the broker's. */
    synchronized void put(String pkg, String proc, JobInfo info, long dueWallMs, int flags, long nowMs) {
        Job j = jobs.get(key(pkg, info.getId()));
        if (j == null) {
            j = new Job(pkg, info.getId());
            jobs.put(j.key(), j);
        }
        j.info = info;
        j.proc = proc;
        j.dueWallMs = dueWallMs;
        j.deadlineWallMs = info.getMaxExecutionDelayMillis() > 0 ? nowMs + info.getMaxExecutionDelayMillis() : 0L;
        j.flags = flags;
        j.held = false;
        j.dispatching = false;
        j.dispatchFailures = 0;
        j.retryAtMs = 0;
        dirty = true;
    }

    /** A one-shot that finished, a cancel: no longer a pending job. */
    synchronized void remove(String pkg, int id) {
        if (jobs.remove(key(pkg, id)) != null) dirty = true;
    }

    /** cancelAll, or an uninstall: every job of the package. */
    synchronized void removeAll(String pkg) {
        if (jobs.values().removeIf(j -> j.pkg.equals(pkg))) dirty = true;
    }

    synchronized void running(String pkg, int id, boolean running, long nowMs) {
        Job j = jobs.get(key(pkg, id));
        if (j == null) return;
        j.running = running;
        j.runningSinceMs = running ? nowMs : 0L;
    }

    /** The process `proc` of `pkg` died: its jobs are the broker's now (JobStore keeps
     *  them). A job it was running is rescheduled with its backoff, as AOSP treats a job
     *  whose process died under it (JobServiceContext: a failed execution). `stopping`:
     *  the broker itself is stopping it (idle exit), which is no failure. */
    synchronized List<Job> processDied(String pkg, String proc, long nowMs, boolean stopping) {
        List<Job> held = new ArrayList<>();
        for (java.util.Iterator<Job> it = jobs.values().iterator(); it.hasNext();) {
            Job j = it.next();
            if (!j.pkg.equals(pkg) || !proc.equals(j.proc)) continue;
            if ((j.flags & (FLAG_WORK | FLAG_CONTENT_TRIGGER)) != 0) {
                it.remove();   // their state was the process's own (see the header)
                dirty = true;
                continue;
            }
            if (j.running && !stopping) {
                long backoff = Math.max(j.info.getInitialBackoffMillis(), JobInfo.MIN_BACKOFF_MILLIS);
                j.dueWallMs = Math.max(j.dueWallMs, nowMs + backoff);
            }
            j.running = false;
            j.held = true;
            j.dispatching = false;
            held.add(j);
            dirty = true;
        }
        return held;
    }

    // ---- what the broker does with them --------------------------------------------

    /** The first constraint of `info` the host does not meet, or null. AOSP's
     *  controllers (ConnectivityController, BatteryController, StorageController,
     *  IdleController), each asked of the host. */
    static String unsatisfied(JobInfo info, Conditions c) {
        int net = info.getNetworkType();
        boolean wantsUnmetered = net == JobInfo.NETWORK_TYPE_UNMETERED || requestsUnmetered(info);
        if (net != JobInfo.NETWORK_TYPE_NONE || wantsUnmetered) {
            if (!c.networkUp()) return "network";
            if (wantsUnmetered && c.networkMetered()) return "unmetered-network";
        }
        if (info.isRequireCharging() && !c.charging()) return "charging";
        if (info.isRequireBatteryNotLow()) {
            int pct = c.batteryPercent();
            // AOSP BatteryController: "not low" while charging, or above the low level (15%).
            if (pct >= 0 && pct < 15 && !c.charging()) return "battery-not-low";
        }
        if (info.isRequireStorageNotLow()) {
            long free = c.freeStorageBytes();
            if (free >= 0 && free < STORAGE_LOW_BYTES) return "storage-not-low";
        }
        if (info.isRequireDeviceIdle() && !c.idle()) return "device-idle";
        return null;
    }

    private static boolean requestsUnmetered(JobInfo info) {
        try {
            android.net.NetworkRequest r = info.getRequiredNetwork();
            return r != null && r.hasCapability(android.net.NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
        } catch (Throwable t) {
            return false;
        }
    }

    /** Whether a held job may start now: due, not already being handed over, past a
     *  failed dispatch's retry time, with its constraints met or its override deadline
     *  passed (AOSP JobStatus.isReady), and `runnable` (the broker's standby rule: may
     *  its process be started, or is it running). */
    private static boolean ready(Job j, long nowMs, Conditions c, java.util.function.Predicate<Job> runnable) {
        if (!j.held || j.dispatching || j.dueWallMs > nowMs || j.retryAtMs > nowMs) return false;
        boolean deadline = j.deadlineWallMs > 0 && nowMs >= j.deadlineWallMs;
        if (!deadline && unsatisfied(j.info, c) != null) return false;
        return runnable == null || runnable.test(j);
    }

    /** The held jobs to start now (see ready), each marked dispatching. */
    synchronized List<Job> takeReady(long nowMs, Conditions c, java.util.function.Predicate<Job> runnable) {
        List<Job> out = new ArrayList<>();
        for (Job j : jobs.values()) {
            if (!ready(j, nowMs, c, runnable)) continue;
            j.dispatching = true;
            out.add(j);
        }
        return out;
    }

    /** The process runs it now and reports it from here on. */
    synchronized void dispatched(Job j) {
        j.dispatching = false;
        j.held = false;
        j.dispatchFailures = 0;
        j.retryAtMs = 0;
        dirty = true;
    }

    synchronized void dispatchFailed(Job j, long nowMs) {
        j.dispatching = false;
        j.dispatchFailures++;
        long wait = DISPATCH_RETRY_MS << Math.min(10, j.dispatchFailures - 1);
        j.retryAtMs = nowMs + Math.min(wait, DISPATCH_RETRY_MAX_MS);
    }

    /** Whether a job keeps the broker from stopping: one being handed to its process,
     *  one running (for at most MAX_RUNNING_HOLD_MS), or, when `dueCounts`, a held job
     *  that could start now (a maintenance window stays for it). */
    synchronized boolean keepsBrokerUp(long nowMs, Conditions c, boolean dueCounts,
                                       java.util.function.Predicate<Job> runnable) {
        for (Job j : jobs.values()) {
            if (j.dispatching) return true;
            if (j.running && nowMs - j.runningSinceMs < MAX_RUNNING_HOLD_MS) return true;
            if (dueCounts && ready(j, nowMs, c, runnable)) return true;
        }
        return false;
    }

    /** The held jobs of `pkg` (getAllPendingJobs on a device lists them: JobSchedulerService
     *  holds them while no process does). */
    synchronized List<JobInfo> heldOf(String pkg) {
        List<JobInfo> out = new ArrayList<>();
        for (Job j : jobs.values()) if (j.pkg.equals(pkg) && j.held) out.add(j.info);
        return out;
    }

    synchronized Job get(String pkg, int id) { return jobs.get(key(pkg, id)); }

    synchronized List<Job> all() { return new ArrayList<>(jobs.values()); }

    synchronized int size() { return jobs.size(); }

    // ---- persistence -----------------------------------------------------------------

    /** Write the store if it changed (tmp file, then renamed over: a reader sees the old
     *  store or the new one). */
    synchronized void saveIfDirty() throws IOException {
        if (!dirty || file == null) return;
        File dir = file.getParentFile();
        if (dir != null) dir.mkdirs();
        File tmp = new File(file.getPath() + ".tmp");
        try (DataOutputStream out = new DataOutputStream(new FileOutputStream(tmp))) {
            out.writeInt(FILE_MAGIC);
            out.writeLong(bootMs);
            List<Job> keep = new ArrayList<>();
            for (Job j : jobs.values()) if ((j.flags & (FLAG_WORK | FLAG_CONTENT_TRIGGER)) == 0) keep.add(j);
            out.writeInt(keep.size());
            for (Job j : keep) {
                out.writeUTF(j.pkg);
                out.writeUTF(j.proc != null ? j.proc : j.pkg);
                out.writeLong(j.dueWallMs);
                out.writeLong(j.deadlineWallMs);
                Parcel p = Parcel.obtain();
                try {
                    j.info.writeToParcel(p, 0);
                    byte[] b = p.marshall();
                    out.writeInt(b.length);
                    out.write(b);
                } finally { p.recycle(); }
            }
        }
        java.nio.file.Files.move(tmp.toPath(), file.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        dirty = false;
    }

    /** The store a previous broker wrote, every job held (no process is attached to a
     *  broker that is starting). A job that is not persisted is kept only within the same
     *  Windows boot. Unreadable or missing: empty. */
    static BrokerJobs load(File file, long bootMs) {
        BrokerJobs s = new BrokerJobs(file, bootMs);
        if (file == null || !file.isFile()) return s;
        try (DataInputStream in = new DataInputStream(new FileInputStream(file))) {
            if (in.readInt() != FILE_MAGIC) return s;
            long writtenBoot = in.readLong();
            boolean sameBoot = Math.abs(writtenBoot - bootMs) < SAME_BOOT_SLACK_MS;
            int n = in.readInt();
            for (int i = 0; i < n; i++) {
                String pkg = in.readUTF(), proc = in.readUTF();
                long due = in.readLong(), deadline = in.readLong();
                byte[] b = new byte[in.readInt()];
                in.readFully(b);
                Parcel p = Parcel.obtain();
                JobInfo info;
                try {
                    p.unmarshall(b, 0, b.length);
                    p.setDataPosition(0);
                    info = JobInfo.CREATOR.createFromParcel(p);
                } finally { p.recycle(); }
                if (!info.isPersisted() && !sameBoot) { s.dirty = true; continue; }   // a reboot drops it
                Job j = new Job(pkg, info.getId());
                j.info = info;
                j.proc = proc;
                j.dueWallMs = due;
                j.deadlineWallMs = deadline;
                j.held = true;
                s.jobs.put(j.key(), j);
            }
            if (!sameBoot) s.dirty = true;   // rewritten with this boot's time
        } catch (Throwable t) {
            SystemServer.log("jobs: cannot read " + file + " (" + t + "); starting empty");
            s.jobs.clear();
        }
        return s;
    }
}
