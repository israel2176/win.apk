// BrokerJobsTest -- the system broker's job store (com.macapk.loader.BrokerJobs): the jobs an
// app's process held outlive it, as JobSchedulerService's do, and the rules for when the
// broker starts the process again for one.
//
//   java -cp android-framework.jar;macapk-loader.jar;<classes> com.macapk.loader.BrokerJobsTest <scratch dir>
//
// Checks: a reported job belongs to its process until the process dies, then the broker
// holds it; a held job is not started before it is due, nor while a constraint is unmet
// (network, an unmetered one, charging, battery not low, storage not low, idle), but is
// once the override deadline passes; a job running when its process died is held with its
// backoff; jobs with enqueued work or a content trigger die with their process; a dispatch
// that failed waits before the next try; a running job keeps the broker up for at most
// AOSP's 10 minutes, a held one that may start keeps a maintenance window up; the store is
// written and read back (JobInfo through its Parcel), a reboot of Windows drops the jobs
// that are not persisted and keeps the persisted ones; JobInfo.setRequiredNetwork with
// NOT_METERED becomes NETWORK_TYPE_UNMETERED, which survives the trip.
// Prints PASS/FAIL per check and "ALL PASSED" at the end; exits 1 on a failure.
package com.macapk.loader;

import android.app.job.JobInfo;
import android.content.ComponentName;

import java.io.File;
import java.util.List;
import java.util.Objects;

public class BrokerJobsTest {
    static int failures;

    static void check(String name, boolean ok) {
        System.out.println((ok ? "PASS " : "FAIL ") + name);
        if (!ok) failures++;
    }

    static void eq(String name, Object want, Object got) {
        check(name + " (want " + want + ", got " + got + ")", Objects.equals(want, got));
    }

    static final String PLAY = "com.android.vending";
    static final String PROC = PLAY + ":background";

    /** The host as a test wants it. */
    static final class Host implements BrokerJobs.Conditions {
        boolean up = true, metered, charging = true, idle = true;
        int battery = -1;
        long free = Long.MAX_VALUE;
        @Override public boolean networkUp() { return up; }
        @Override public boolean networkMetered() { return metered; }
        @Override public boolean charging() { return charging; }
        @Override public int batteryPercent() { return battery; }
        @Override public long freeStorageBytes() { return free; }
        @Override public boolean idle() { return idle; }
    }

    static JobInfo.Builder job(int id) {
        return new JobInfo.Builder(id, new ComponentName(PLAY, PLAY + ".UpdateJobService"));
    }

    static int ids(List<BrokerJobs.Job> l) { return l.size() == 1 ? l.get(0).id : -l.size(); }

    public static void main(String[] args) throws Exception {
        File dir = new File(args.length > 0 ? args[0] : "broker-jobs-test");
        dir.mkdirs();
        File file = new File(dir, "jobs.bin");
        file.delete();
        long boot = 1_000_000L, t = 50_000_000L;
        Host host = new Host();

        BrokerJobs s = new BrokerJobs(file, boot);
        s.put(PLAY, PROC, job(1).setRequiredNetworkType(JobInfo.NETWORK_TYPE_UNMETERED).setPersisted(true).build(),
                t + 60_000, 0, t);
        eq("a job its process holds is not the broker's to start", 0, s.takeReady(t + 120_000, host, null).size());
        s.processDied(PLAY, "com.android.vending", t, false);
        eq("another process's death leaves it alone", 0, s.takeReady(t + 120_000, host, null).size());
        List<BrokerJobs.Job> held = s.processDied(PLAY, PROC, t, false);
        eq("its own process's death: the broker holds it", 1, held.size());
        eq("not before it is due", 0, s.takeReady(t + 59_000, host, null).size());
        host.metered = true;
        eq("not on a metered network when it wants an unmetered one", 0, s.takeReady(t + 61_000, host, null).size());
        host.metered = false;
        host.up = false;
        eq("not with no network", 0, s.takeReady(t + 61_000, host, null).size());
        host.up = true;
        eq("not when the standby rule says no", 0, s.takeReady(t + 61_000, host, j -> false).size());
        List<BrokerJobs.Job> ready = s.takeReady(t + 61_000, host, null);
        eq("due, unmetered network: ready", 1, ids(ready));
        check("being handed over keeps the broker up", s.keepsBrokerUp(t + 61_000, host, false, null));
        eq("not taken twice while it is handed over", 0, s.takeReady(t + 62_000, host, null).size());
        s.dispatchFailed(ready.get(0), t + 62_000);
        eq("a failed hand-over waits a minute", 0, s.takeReady(t + 62_000 + 59_000, host, null).size());
        ready = s.takeReady(t + 62_000 + 61_000, host, null);
        eq("and is tried again after it", 1, ids(ready));
        s.dispatched(ready.get(0));
        eq("handed over: its process's again", 0, s.heldOf(PLAY).size());

        // Running when its process died: held again, with its backoff.
        s.running(PLAY, 1, true, t + 200_000);
        check("a running job keeps the broker up", s.keepsBrokerUp(t + 200_000 + 60_000, host, false, null));
        check("for at most 10 minutes", !s.keepsBrokerUp(t + 200_000 + 11 * 60_000, host, false, null));
        s.processDied(PLAY, PROC, t + 300_000, false);
        BrokerJobs.Job j1 = s.get(PLAY, 1);
        check("a job whose process died under it is held, with its backoff (30 s)",
                j1.held && !j1.running && j1.dueWallMs >= t + 330_000);
        check("a maintenance window stays for a held job that may start",
                s.keepsBrokerUp(t + 331_000, host, true, null));
        check("an ordinary broker does not", !s.keepsBrokerUp(t + 331_000, host, false, null));

        // Constraints.
        s.put(PLAY, PROC, job(2).setRequiresCharging(true).build(), t, 0, t);
        s.put(PLAY, PROC, job(3).setRequiresDeviceIdle(true).build(), t, 0, t);
        s.put(PLAY, PROC, job(4).setRequiresBatteryNotLow(true).build(), t, 0, t);
        s.put(PLAY, PROC, job(5).setRequiresStorageNotLow(true).build(), t, 0, t);
        s.put(PLAY, PROC, job(6).setRequiresCharging(true).setOverrideDeadline(60_000).build(), t, 0, t);
        s.remove(PLAY, 1);
        s.processDied(PLAY, PROC, t, false);
        host.charging = false;
        host.idle = false;
        host.battery = 10;
        host.free = 100L * 1024 * 1024;
        eq("charging, idle, battery, storage all unmet: nothing", 0, s.takeReady(t + 1_000, host, null).size());
        eq("past its override deadline, a job runs anyway", 6, ids(s.takeReady(t + 61_000, host, null)));
        host.charging = true;
        List<BrokerJobs.Job> r2 = s.takeReady(t + 62_000, host, null);
        check("on mains: the charging job, and the battery one (not low while charging)",
                r2.size() == 2 && r2.get(0).id == 2 && r2.get(1).id == 4);
        host.idle = true;
        eq("no app open: the idle job", 3, ids(s.takeReady(t + 63_000, host, null)));
        host.free = 10L * 1024 * 1024 * 1024;
        eq("storage freed: the storage job", 5, ids(s.takeReady(t + 64_000, host, null)));
        s.removeAll(PLAY);
        eq("removeAll", 0, s.size());

        // Work and content triggers die with their process.
        s.put(PLAY, PROC, job(7).build(), t, SystemClient.JOB_FLAG_WORK, t);
        s.put(PLAY, PROC, job(8).build(), t, SystemClient.JOB_FLAG_CONTENT_TRIGGER, t);
        s.processDied(PLAY, PROC, t, false);
        eq("a job with enqueued work or a content trigger goes with its process", 0, s.size());

        // The store on disk, and a reboot.
        s.put(PLAY, PROC, job(10).setPersisted(true).setPeriodic(24 * 3_600_000L).build(), t + 9_000, 0, t);
        s.put(PLAY, PROC, job(11).setPersisted(false).build(), t + 9_000, 0, t);
        s.saveIfDirty();
        check("written", file.isFile() && file.length() > 0);
        BrokerJobs same = BrokerJobs.load(file, boot + 1_000);
        eq("the same boot: both back", 2, same.size());
        BrokerJobs.Job back = same.get(PLAY, 10);
        check("held, with its process, due time and JobInfo",
                back != null && back.held && PROC.equals(back.proc) && back.dueWallMs == t + 9_000
                        && back.info.isPeriodic() && back.info.getIntervalMillis() == 24 * 3_600_000L
                        && back.info.isPersisted()
                        && back.info.getService().equals(new ComponentName(PLAY, PLAY + ".UpdateJobService")));
        BrokerJobs rebooted = BrokerJobs.load(file, boot + 3_600_000L);
        check("after a reboot only the persisted job", rebooted.size() == 1 && rebooted.get(PLAY, 10) != null);
        rebooted.saveIfDirty();
        eq("and the store is rewritten for this boot", 1, BrokerJobs.load(file, boot + 3_600_000L).size());
        java.nio.file.Files.write(file.toPath(), new byte[] { 1, 2, 3 });
        eq("an unreadable store: empty", 0, BrokerJobs.load(file, boot).size());

        // A NetworkRequest asking for NOT_METERED is an unmetered job, also across a Parcel.
        JobInfo viaRequest = job(20).setRequiredNetwork(new android.net.NetworkRequest.Builder()
                .addCapability(android.net.NetworkCapabilities.NET_CAPABILITY_NOT_METERED).build()).build();
        eq("setRequiredNetwork(NOT_METERED) is NETWORK_TYPE_UNMETERED", JobInfo.NETWORK_TYPE_UNMETERED,
                viaRequest.getNetworkType());
        android.os.Parcel p = android.os.Parcel.obtain();
        viaRequest.writeToParcel(p, 0);
        p.setDataPosition(0);
        JobInfo read = JobInfo.CREATOR.createFromParcel(p);
        host.metered = true;
        eq("across a Parcel the broker still sees it", "unmetered-network", BrokerJobs.unsatisfied(read, host));
        JobInfo any = job(21).setRequiredNetwork(new android.net.NetworkRequest.Builder()
                .addCapability(android.net.NetworkCapabilities.NET_CAPABILITY_INTERNET).build()).build();
        eq("any other request is NETWORK_TYPE_ANY", JobInfo.NETWORK_TYPE_ANY, any.getNetworkType());
        eq("which a metered network meets", null, BrokerJobs.unsatisfied(any, host));

        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
