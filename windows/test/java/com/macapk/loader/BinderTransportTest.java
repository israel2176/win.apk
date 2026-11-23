// BinderTransportTest -- cross-process Binder on Windows (windows/host/binder_win.cpp
// under android.os.MacApkBinderTransport), run by winapk-binder-test.exe, which
// binds the natives and calls main. The test starts more copies of that
// program for its other processes.
//
// Transport, parent <-> "service" child:
//   transact and reply; the calling pid is the OS's; the calling uid comes from
//   the RECEIVER's resolver, never from the caller; oneway calls arrive, in
//   order; a binder passed both ways keeps its identity (a callback into the
//   caller while it waits, our own object coming home as itself, one proxy per
//   remote object); a Messenger across processes stamps the sender's uid on the
//   Message (Message.sendingUid) and its replyTo answers back, and a
//   PendingIntent in its Bundle keeps its creator's uid; file descriptors
//   cross as new fds of the same file; death
//   of the far process reaches linkToDeath, then isBinderAlive and transact.
// Broker, "client" child -> "broker" child (com.macapk.loader.SystemServer):
//   a process registering as, or calling as, a package it is not is refused;
//   the package of the process is what its command line (--apk) makes it; the
//   android:exported check of a cross-package resolve; the uid of a process is
//   its package's. The parent, which is no installed app, is refused too.
// Jobs, "jobclient" child -> "jobbroker" child: a job reported by a process that then
//   exits is kept by the broker (BrokerJobs) and, once due, handed to the package's
//   process, which the broker starts for it ("jobhost"): JobScheduler running a job
//   of an app that is not running, as the Play Store's update jobs run on a phone.
// Package changes, "pkgevents" child -> "pkgbroker" child, from a copy of this
//   program in <scratch>\inst\bin with another copy as <scratch>\inst\Win.apk.exe,
//   the launcher (its --notify runs winapk/package_events.h, the launcher's code):
//   a package added while the broker runs is known to it at once (its process
//   is accepted on attach, refused before), and a running app's dynamic receiver
//   hears PACKAGE_ADDED; an update sends PACKAGE_ADDED with EXTRA_REPLACING and
//   PACKAGE_REPLACED to the others and MY_PACKAGE_REPLACED to the package alone;
//   a removal drops it and sends PACKAGE_REMOVED and PACKAGE_FULLY_REMOVED, not
//   to the removed package; manifest receivers of an app targeting API 26+ are
//   left out of the implicit ones but FULLY_REMOVED; the change is refused from
//   any caller but the launcher (another runtime, this program not named
//   Win.apk.exe), and an app's own PACKAGE_ADDED broadcast is dropped.
// Google accounts, "accounts" child with its own GmsStore folder: the broker's
//   account service mirrors GmsStore's accounts into its table (master token as
//   the password, the gaia id as GoogleUserId), sees an account another process
//   signs in after the service started (the Play Store's "Sign in" in its own
//   process, Settings' `--gms signin-window`), with or without being asked, and
//   drops a mirrored account once it is signed out; a row it did not mirror stays.
package com.macapk.loader;

import android.content.ComponentName;
import android.content.Intent;
import android.app.PendingIntent;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.DeadObjectException;
import android.os.IBinder;
import android.os.MacApkBinderTransport;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class BinderTransportTest {
    static int failures;

    static void check(String what, boolean ok, String detail) {
        System.out.println("  [" + (ok ? "PASS" : "FAIL") + "] " + what + (detail.isEmpty() ? "" : ": " + detail));
        if (!ok) failures++;
    }

    static int myPid() { return (int) ProcessHandle.current().pid(); }

    /** A file or folder of this process in the temp folder. Not createTempFile:
     *  that seeds SecureRandom, which under Wine reaches a function Wine lacks. */
    static File scratch(String name) {
        File dir = new File(System.getProperty("java.io.tmpdir"), "winapk-binder-" + myPid());
        dir.mkdirs();
        dir.deleteOnExit();
        File f = new File(dir, name);
        if (name.indexOf('.') < 0) f.mkdirs();
        f.deleteOnExit();
        return f;
    }

    static List<String> self(String... args) {
        List<String> cmd = new ArrayList<>();
        cmd.add(ProcessHandle.current().info().command().orElseThrow());
        cmd.add(System.getProperty("java.class.path"));
        cmd.add(BinderTransportTest.class.getName());
        for (String a : args) cmd.add(a);
        return cmd;
    }

    static IBinder waitFor(String name) throws InterruptedException {
        long deadline = System.currentTimeMillis() + 60_000;
        while (System.currentTimeMillis() < deadline) {
            IBinder b = MacApkBinderTransport.lookUp(name);
            if (b != null) return b;
            Thread.sleep(200);
        }
        return null;
    }

    public static void main(String[] args) throws Exception {
        int rc;
        if (args.length == 0) rc = parent();
        else if (args[0].equals("service")) rc = service(args[1]);
        else if (args[0].equals("broker")) rc = broker(args);
        else if (args[0].equals("client")) rc = client(args);
        else if (args[0].equals("accounts")) rc = accounts();
        else if (args[0].equals("gms-write")) rc = gmsWrite(args);
        else if (args[0].equals("jobbroker")) rc = jobBroker(args);
        else if (args[0].equals("jobclient")) rc = jobClient(args);
        else if (args[0].equals("jobhost")) rc = jobHost(args);
        else if (args[0].equals("pkgevents")) rc = packageEvents(args);
        else if (args[0].equals("pkgbroker")) rc = packageBroker(args);
        else if (args[0].equals("app")) rc = app(args);
        else rc = 2;
        System.out.flush();
        System.exit(rc);
    }

    // ---- the transport ---------------------------------------------------------------

    static final int ECHO = 1, ONEWAY = 2, COUNT = 3, CALLBACK = 4, SAME = 5, FD = 6, DIE = 7, RESOLVER = 8,
            HOME = 9, MESSENGER = 10;

    /** A Handler on a new thread running its own Looper. */
    static Handler looperThread(String name, Handler.Callback cb) throws InterruptedException {
        final Handler[] h = new Handler[1];
        CountDownLatch ready = new CountDownLatch(1);
        Thread t = new Thread(() -> {
            Looper.prepare();
            h[0] = new Handler(Looper.myLooper(), cb);
            ready.countDown();
            Looper.loop();
        }, name);
        t.setDaemon(true);
        t.start();
        ready.await(20, TimeUnit.SECONDS);
        return h[0];
    }

    /** The child's own object, handed out twice per SAME call. */
    static final Binder CHILD_OBJECT = new Binder();

    static int service(String name) throws Exception {
        final List<Integer> seen = java.util.Collections.synchronizedList(new ArrayList<>());
        Binder svc = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                try {
                    switch (code) {
                        case ECHO:
                            reply.writeString(data.readString() + "!");
                            reply.writeInt(data.readInt() + 1);
                            reply.writeInt(Binder.getCallingPid());
                            reply.writeInt(Binder.getCallingUid());
                            return true;
                        case ONEWAY:
                            seen.add(data.readInt());
                            return true;
                        case COUNT: {
                            boolean inOrder = true;
                            synchronized (seen) {
                                for (int i = 0; i < seen.size(); i++) inOrder &= seen.get(i) == i;
                            }
                            reply.writeInt(seen.size());
                            reply.writeInt(inOrder ? 1 : 0);
                            return true;
                        }
                        case CALLBACK: {
                            IBinder cb = data.readStrongBinder();
                            Parcel d = Parcel.obtain(), r = Parcel.obtain();
                            d.writeInt(7);
                            cb.transact(10, d, r, 0);   // into the caller, while it waits on us
                            reply.writeInt(r.readInt());
                            reply.writeStrongBinder(cb);  // and back home
                            return true;
                        }
                        case SAME:
                            reply.writeStrongBinder(CHILD_OBJECT);
                            reply.writeStrongBinder(CHILD_OBJECT);
                            return true;
                        case MESSENGER: {
                            // As Google Play services' GCM registrar: the uid that
                            // sent the Message and the creator uid of the
                            // PendingIntent ("app") it carries, answered on replyTo.
                            Handler h = looperThread("child-messenger", msg -> {
                                try {
                                    Bundle b = msg.getData();
                                    b.setClassLoader(PendingIntent.class.getClassLoader());
                                    PendingIntent app = b.getParcelable("app");
                                    Message out = Message.obtain();
                                    out.what = msg.what;
                                    out.arg1 = msg.sendingUid;
                                    out.arg2 = app != null ? app.getCreatorUid() : -2;
                                    msg.replyTo.send(out);
                                } catch (Exception e) {
                                    e.printStackTrace();
                                }
                                return true;
                            });
                            reply.writeStrongBinder(new Messenger(h).getBinder());
                            return true;
                        }
                        case HOME:
                            reply.writeInt(data.readStrongBinder() == CHILD_OBJECT ? 1 : 0);
                            return true;
                        case FD: {
                            ParcelFileDescriptor in = data.readFileDescriptor();
                            String text;
                            try (FileInputStream s = new FileInputStream(in.getFileDescriptor())) {
                                text = new String(s.readAllBytes(), StandardCharsets.UTF_8);
                            }
                            reply.writeString(text);
                            File mine = scratch("child.txt");
                            Files.writeString(mine.toPath(), "written by the child");
                            ParcelFileDescriptor out = ParcelFileDescriptor.open(mine,
                                    ParcelFileDescriptor.MODE_READ_ONLY);
                            reply.writeFileDescriptor(out.getFileDescriptor());
                            return true;
                        }
                        case RESOLVER:
                            final int uid = data.readInt();
                            MacApkBinderTransport.setUidResolver(pid -> uid);
                            return true;
                        case DIE:
                            Runtime.getRuntime().halt(0);
                            return true;
                        default:
                            return false;
                    }
                } catch (Exception e) {
                    throw new RuntimeException(e);
                }
            }
        };
        MacApkBinderTransport.publish(name, svc);
        Thread.sleep(Long.MAX_VALUE);
        return 0;
    }

    static int parent() throws Exception {
        System.out.println("Win.apk cross-process Binder (named pipes)");
        String name = "winapk-test-" + myPid() + "-svc";
        Process child = new ProcessBuilder(self("service", name)).inheritIO().start();
        IBinder svc = waitFor(name);
        check("look up the published service", svc != null, "");
        if (svc == null) { child.destroy(); return 1; }

        Parcel d = Parcel.obtain(), r = Parcel.obtain();
        d.writeString("hello");
        d.writeInt(41);
        boolean ok = svc.transact(ECHO, d, r, 0);
        String s = r.readString();
        int n = r.readInt(), pid = r.readInt(), uid = r.readInt();
        check("transact and reply", ok && "hello!".equals(s) && n == 42, s + " " + n);
        check("the calling pid is the caller's, from the OS", pid == myPid(), pid + " vs " + myPid());
        check("no resolver: the calling uid is nobody's (9999), not one the caller sent", uid == 9999, "" + uid);

        d = Parcel.obtain(); r = Parcel.obtain();
        d.writeInt(12345);
        svc.transact(RESOLVER, d, r, 0);
        d = Parcel.obtain(); r = Parcel.obtain();
        d.writeString("x"); d.writeInt(0);
        svc.transact(ECHO, d, r, 0);
        r.readString(); r.readInt(); r.readInt();
        uid = r.readInt();
        check("the calling uid is the receiver's answer for that pid", uid == 12345, "" + uid);

        for (int i = 0; i < 50; i++) {
            d = Parcel.obtain();
            d.writeInt(i);
            svc.transact(ONEWAY, d, null, IBinder.FLAG_ONEWAY);
        }
        d = Parcel.obtain(); r = Parcel.obtain();
        svc.transact(COUNT, d, r, 0);
        int count = r.readInt(), inOrder = r.readInt();
        check("50 oneway calls arrive, in order", count == 50 && inOrder == 1, count + " in order " + inOrder);

        final int[] cbPid = { -1 };
        Binder cb = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                cbPid[0] = Binder.getCallingPid();
                int x = data.readInt();
                reply.writeInt(x * x);
                return true;
            }
        };
        d = Parcel.obtain(); r = Parcel.obtain();
        d.writeStrongBinder(cb);
        svc.transact(CALLBACK, d, r, 0);
        int sq = r.readInt();
        IBinder back = r.readStrongBinder();
        check("a callback into the caller while it waits (nested call)", sq == 49, "" + sq);
        check("the callback's calling pid is the child's", cbPid[0] == child.pid(), cbPid[0] + " vs " + child.pid());
        check("our own binder comes home as the same object", back == cb, String.valueOf(back));

        d = Parcel.obtain(); r = Parcel.obtain();
        svc.transact(SAME, d, r, 0);
        IBinder a = r.readStrongBinder(), b = r.readStrongBinder();
        d = Parcel.obtain(); r = Parcel.obtain();
        svc.transact(SAME, d, r, 0);
        IBinder c = r.readStrongBinder();
        check("one proxy per remote object, within and across calls", a != null && a == b && a == c,
                a + " " + b + " " + c);
        check("a remote object is not taken for a local one", !(a instanceof Binder), String.valueOf(a));
        d = Parcel.obtain(); r = Parcel.obtain();
        d.writeStrongBinder(a);
        svc.transact(HOME, d, r, 0);
        check("a proxy sent back is the owner's own object there", r.readInt() == 1, "");

        // The child's resolver answers 12345 for us (RESOLVER above); ours is unset,
        // so the child's reply comes from nobody's uid (9999).
        d = Parcel.obtain(); r = Parcel.obtain();
        svc.transact(MESSENGER, d, r, 0);
        IBinder mb = r.readStrongBinder();
        final int[] got = { -1, -1, -1, -1 };
        CountDownLatch answered = new CountDownLatch(1);
        Handler mine = looperThread("parent-messenger", msg -> {
            got[0] = msg.what; got[1] = msg.arg1; got[2] = msg.arg2; got[3] = msg.sendingUid;
            answered.countDown();
            return true;
        });
        Message m = Message.obtain();
        m.what = 77;
        m.replyTo = new Messenger(mine);
        Bundle data = new Bundle();
        data.putParcelable("app", PendingIntent.getBroadcast(null, 0, new Intent("com.example.NOTHING"),
                PendingIntent.FLAG_IMMUTABLE));
        m.setData(data);
        boolean sent = false;
        if (mb != null) { new Messenger(mb).send(m); sent = true; }
        boolean replied = sent && answered.await(20, TimeUnit.SECONDS);
        check("a Message to a Messenger in another process is answered on its replyTo",
                replied && got[0] == 77, got[0] + "");
        check("the receiver sees the sender's uid in Message.sendingUid", got[1] == 12345, "" + got[1]);
        check("a PendingIntent in the Message keeps its creator's uid", got[2] == android.os.Process.myUid(),
                got[2] + " vs " + android.os.Process.myUid());
        check("the reply carries its own sender's uid", got[3] == 9999, "" + got[3]);

        File f = scratch("parent.txt");
        Files.writeString(f.toPath(), "written by the parent \u05e9\u05dc\u05d5\u05dd");
        ParcelFileDescriptor pfd = ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
        d = Parcel.obtain(); r = Parcel.obtain();
        d.writeFileDescriptor(pfd.getFileDescriptor());
        svc.transact(FD, d, r, 0);
        String readThere = r.readString();
        check("an fd crosses and is read in the other process", "written by the parent \u05e9\u05dc\u05d5\u05dd".equals(readThere),
                String.valueOf(readThere));
        ParcelFileDescriptor fromChild = r.readFileDescriptor();
        String readHere = null;
        if (fromChild != null) {
            try (FileInputStream in = new FileInputStream(fromChild.getFileDescriptor())) {
                readHere = new String(in.readAllBytes(), StandardCharsets.UTF_8);
            }
        }
        check("an fd in a reply arrives as a new fd here", "written by the child".equals(readHere),
                String.valueOf(readHere));

        CountDownLatch died = new CountDownLatch(1);
        svc.linkToDeath(() -> { died.countDown(); }, 0);
        d = Parcel.obtain();
        svc.transact(DIE, d, null, IBinder.FLAG_ONEWAY);
        boolean heard = died.await(20, TimeUnit.SECONDS);
        child.waitFor(20, TimeUnit.SECONDS);
        check("linkToDeath hears the far process exit", heard, "");
        check("isBinderAlive is false after", !svc.isBinderAlive(), "");
        boolean deadThrown = false;
        try {
            svc.transact(ECHO, Parcel.obtain(), Parcel.obtain(), 0);
        } catch (DeadObjectException e) {
            deadThrown = true;
        }
        check("a call to a dead process throws DeadObjectException", deadThrown, "");

        brokerChecks();
        packageEventChecks();
        jobChecks();
        googleAccountChecks();
        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        return failures == 0 ? 0 : 1;
    }

    // ---- the broker ------------------------------------------------------------------

    static final String GOOD = "com.example.good", VICTIM = "com.example.victim";

    static String manifest(String pkg) {
        return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            + "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"" + pkg + "\">\n"
            + "  <application>\n"
            + "    <service android:name=\"" + pkg + ".Exported\" android:exported=\"true\">\n"
            + "      <intent-filter><action android:name=\"com.example.BIND\"/></intent-filter>\n"
            + "    </service>\n"
            + "    <service android:name=\"" + pkg + ".Private\" android:exported=\"false\"/>\n"
            + "  </application>\n"
            + "</manifest>\n";
    }

    static void brokerChecks() throws Exception {
        File dir = scratch("broker");
        File root = new File(dir, "android");
        root.mkdirs();
        File good = new File(dir, "good.apk"), victim = new File(dir, "victim.apk");
        Files.writeString(good.toPath(), "not a real apk");
        Files.writeString(victim.toPath(), "not a real apk");
        Files.writeString(new File(dir, "good.xml").toPath(), manifest(GOOD));
        Files.writeString(new File(dir, "victim.xml").toPath(), manifest(VICTIM));
        String name = "winapk-test-" + myPid() + "-system";

        ProcessBuilder bp = new ProcessBuilder(self("broker", name, dir.getPath())).inheritIO();
        bp.environment().put("WINAPK_TEST_ANDROID_ROOT", root.getPath());
        Process broker = bp.start();
        IBinder sys = waitFor(name);
        check("the broker is published", sys != null, "");
        if (sys == null) { broker.destroy(); return; }

        // The parent is no installed app (its command line names no APK).
        Parcel d = Parcel.obtain(), r = Parcel.obtain();
        d.writeInterfaceToken(SystemServer.DESCRIPTOR);
        d.writeString(GOOD);
        d.writeTypedObject(new Intent().setComponent(new ComponentName(VICTIM, VICTIM + ".Exported")), 0);
        sys.transact(SystemServer.TX_RESOLVE, d, r, 0);
        boolean refused = false;
        try { r.readException(); } catch (SecurityException e) { refused = true; }
        check("a process that is no installed app is refused, whatever package it names", refused, "");

        ProcessBuilder cp = new ProcessBuilder(self("client", name, "--apk", good.getPath())).inheritIO();
        cp.environment().put("WINAPK_TEST_ANDROID_ROOT", root.getPath());
        Process client = cp.start();
        boolean done = client.waitFor(120, TimeUnit.SECONDS);
        int rc = done ? client.exitValue() : -1;
        check("the client's broker checks (above) passed", rc == 0, "exit " + rc);
        broker.destroy();
    }

    static int broker(String[] args) throws Exception {
        String name = args[1];
        File dir = new File(args[2]);
        SystemServer s = new SystemServer();
        java.lang.reflect.Field f = SystemServer.class.getDeclaredField("packages");
        f.setAccessible(true);
        @SuppressWarnings("unchecked")
        java.util.Map<String, SystemServer.PackageRecord> packages =
                (java.util.Map<String, SystemServer.PackageRecord>) f.get(s);
        packages.put(GOOD, new SystemServer.PackageRecord(GOOD, new File(dir, "good.apk").getPath(),
                Manifest.parse(new File(dir, "good.xml"), null)));
        packages.put(VICTIM, new SystemServer.PackageRecord(VICTIM, new File(dir, "victim.apk").getPath(),
                Manifest.parse(new File(dir, "victim.xml"), null)));
        MacApkBinderTransport.setUidResolver(s::uidOfPid);
        MacApkBinderTransport.publish(name, s.new Stub());
        Thread.sleep(Long.MAX_VALUE);
        return 0;
    }

    static boolean refused(IBinder sys, int code, Parcel d) throws Exception {
        Parcel r = Parcel.obtain();
        sys.transact(code, d, r, 0);
        try { r.readException(); } catch (SecurityException e) { return true; }
        return false;
    }

    static int resolve(IBinder sys, String caller, String pkg, String cls) throws Exception {
        Parcel d = Parcel.obtain(), r = Parcel.obtain();
        d.writeInterfaceToken(SystemServer.DESCRIPTOR);
        d.writeString(caller);
        d.writeTypedObject(new Intent().setComponent(new ComponentName(pkg, cls)), 0);
        sys.transact(SystemServer.TX_RESOLVE, d, r, 0);
        r.readException();
        return r.readInt();
    }

    static int client(String[] args) throws Exception {
        System.out.println("broker checks (client pid " + myPid() + ", --apk " + args[3] + ")");
        IBinder sys = waitFor(args[1]);
        if (sys == null) { System.out.println("  [FAIL] no broker"); return 1; }

        Parcel d = Parcel.obtain();
        d.writeInterfaceToken(SystemServer.DESCRIPTOR);
        d.writeString(VICTIM);
        d.writeStrongBinder(new Binder());
        d.writeString(new File(new File(args[3]).getParentFile(), "victim.apk").getPath());
        d.writeString(VICTIM);
        check("registering as another package is refused", refused(sys, SystemServer.TX_REGISTER, d), "");

        d = Parcel.obtain();
        d.writeInterfaceToken(SystemServer.DESCRIPTOR);
        d.writeString(VICTIM);
        d.writeTypedObject(new Intent().setComponent(new ComponentName(VICTIM, VICTIM + ".Private")), 0);
        check("calling as another package is refused", refused(sys, SystemServer.TX_RESOLVE, d), "");

        d = Parcel.obtain();
        Parcel r = Parcel.obtain();
        d.writeInterfaceToken(SystemServer.DESCRIPTOR);
        d.writeString(GOOD);
        d.writeStrongBinder(new Binder());
        d.writeString("C:\\somewhere\\else.apk");   // ignored: the broker uses the installed APK
        d.writeString(GOOD);
        sys.transact(SystemServer.TX_REGISTER, d, r, 0);
        boolean registered;
        int brokerPid = -1;
        try { r.readException(); brokerPid = r.readInt(); registered = true; }
        catch (SecurityException e) { registered = false; }
        check("registering as the package its command line names is accepted", registered && brokerPid > 0,
                "broker pid " + brokerPid);

        check("a non-exported service of another package does not resolve for us",
                resolve(sys, GOOD, VICTIM, VICTIM + ".Private") == SystemServer.RESOLVE_NOT_EXPORTED, "");
        check("an exported one does",
                resolve(sys, GOOD, VICTIM, VICTIM + ".Exported") == SystemServer.RESOLVE_OK, "");
        check("our own non-exported service does",
                resolve(sys, GOOD, GOOD, GOOD + ".Private") == SystemServer.RESOLVE_OK, "");

        d = Parcel.obtain();
        r = Parcel.obtain();
        d.writeInterfaceToken(SystemServer.DESCRIPTOR);
        d.writeInt(myPid());
        sys.transact(SystemServer.TX_UID_OF_PID, d, r, 0);
        r.readException();
        int uid = r.readInt();
        int expected = AppUids.uidFor(GOOD, null);
        check("our uid is our package's", uid >= AppUids.FIRST && uid == expected, uid + " vs " + expected);
        return failures == 0 ? 0 : 1;
    }

    // ---- package changes: the launcher's commit ---------------------------------------

    static final String LISTENER = "com.example.listener", ADDED = "com.example.added", FAKE = "com.example.fake";

    /** The parent: a copy of this program in inst\bin (the runtime) and one as inst\Win.apk.exe (the
     *  launcher above it); the checks run in a process of the bin copy, which may talk to a broker
     *  started from that copy. */
    static void packageEventChecks() throws Exception {
        File dir = scratch("pkg");
        File bin = new File(dir, "inst/bin");
        bin.mkdirs();
        java.nio.file.Path exe = java.nio.file.Paths.get(ProcessHandle.current().info().command().orElseThrow());
        File runtime = new File(bin, "winapk-binder-test.exe"), launcher = new File(dir, "inst/Win.apk.exe");
        Files.copy(exe, runtime.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        Files.copy(exe, launcher.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        runtime.deleteOnExit();
        launcher.deleteOnExit();
        List<String> cmd = new ArrayList<>(List.of(runtime.getPath(), System.getProperty("java.class.path"),
                BinderTransportTest.class.getName(), "pkgevents", dir.getPath(), launcher.getPath()));
        ProcessBuilder pb = new ProcessBuilder(cmd).inheritIO();
        pb.environment().put("WINAPK_TEST_ANDROID_ROOT", new File(dir, "android").getPath());
        Process p = pb.start();
        boolean done = p.waitFor(400, TimeUnit.SECONDS);
        if (!done) p.destroyForcibly();
        int rc = done ? p.exitValue() : -1;
        check("the package change checks (above) passed", rc == 0, "exit " + rc);
    }

    /** A binary AndroidManifest.xml as aapt2 writes one (UTF-16 string pool, no resource map):
     *  `<manifest package><uses-sdk android:targetSdkVersion/><application>`, one exported receiver
     *  per action, `</application></manifest>`. */
    static byte[] axml(String pkg, int targetSdk, String... receiverActions) {
        final String ns = "http://schemas.android.com/apk/res/android";
        List<String> pool = new ArrayList<>();
        java.util.function.ToIntFunction<String> idx = v -> {
            int i = pool.indexOf(v);
            if (i < 0) { pool.add(v); i = pool.size() - 1; }
            return i;
        };
        java.io.ByteArrayOutputStream body = new java.io.ByteArrayOutputStream();
        java.util.function.BiConsumer<java.io.ByteArrayOutputStream, Integer> i32 = (o, v) -> {
            o.write(v); o.write(v >>> 8); o.write(v >>> 16); o.write(v >>> 24);
        };
        java.util.function.BiConsumer<java.io.ByteArrayOutputStream, Integer> u16 = (o, v) -> {
            o.write(v); o.write(v >>> 8);
        };
        int android = idx.applyAsInt("android"), uri = idx.applyAsInt(ns);
        // START_NAMESPACE
        u16.accept(body, 0x0100); u16.accept(body, 16); i32.accept(body, 24);
        i32.accept(body, 1); i32.accept(body, -1); i32.accept(body, android); i32.accept(body, uri);
        java.util.function.BiConsumer<String, String[][]> start = (tag, attrs) -> {
            u16.accept(body, 0x0102); u16.accept(body, 16); i32.accept(body, 16 + 20 + 20 * attrs.length);
            i32.accept(body, 1); i32.accept(body, -1);
            i32.accept(body, -1); i32.accept(body, idx.applyAsInt(tag));
            u16.accept(body, 20); u16.accept(body, 20); u16.accept(body, attrs.length);
            u16.accept(body, 0); u16.accept(body, 0); u16.accept(body, 0);
            for (String[] a : attrs) {   // {name, value, "a" when in the android namespace}
                int v = idx.applyAsInt(a[1]);
                i32.accept(body, a.length > 2 ? uri : -1); i32.accept(body, idx.applyAsInt(a[0]));
                i32.accept(body, v);
                u16.accept(body, 8); body.write(0); body.write(0x03); i32.accept(body, v);
            }
        };
        java.util.function.Consumer<String> end = tag -> {
            u16.accept(body, 0x0103); u16.accept(body, 16); i32.accept(body, 24);
            i32.accept(body, 1); i32.accept(body, -1); i32.accept(body, -1); i32.accept(body, idx.applyAsInt(tag));
        };
        start.accept("manifest", new String[][] { { "package", pkg } });
        start.accept("uses-sdk", new String[][] { { "minSdkVersion", "21", "a" },
                                                   { "targetSdkVersion", Integer.toString(targetSdk), "a" } });
        end.accept("uses-sdk");
        start.accept("application", new String[0][]);
        int n = 0;
        for (String action : receiverActions) {
            start.accept("receiver", new String[][] { { "name", pkg + ".Receiver" + (n++), "a" },
                                                      { "exported", "true", "a" } });
            start.accept("intent-filter", new String[0][]);
            start.accept("action", new String[][] { { "name", action, "a" } });
            end.accept("action");
            end.accept("intent-filter");
            end.accept("receiver");
        }
        end.accept("application");
        end.accept("manifest");
        // END_NAMESPACE
        u16.accept(body, 0x0101); u16.accept(body, 16); i32.accept(body, 24);
        i32.accept(body, 1); i32.accept(body, -1); i32.accept(body, android); i32.accept(body, uri);

        java.io.ByteArrayOutputStream strings = new java.io.ByteArrayOutputStream();
        java.io.ByteArrayOutputStream offsets = new java.io.ByteArrayOutputStream();
        for (String v : pool) {
            i32.accept(offsets, strings.size());
            u16.accept(strings, v.length());
            for (char c : v.toCharArray()) u16.accept(strings, (int) c);
            u16.accept(strings, 0);
        }
        while (strings.size() % 4 != 0) strings.write(0);
        int poolSize = 28 + offsets.size() + strings.size();
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        u16.accept(out, 0x0003); u16.accept(out, 8); i32.accept(out, 8 + poolSize + body.size());
        u16.accept(out, 0x0001); u16.accept(out, 28); i32.accept(out, poolSize);
        i32.accept(out, pool.size()); i32.accept(out, 0); i32.accept(out, 0);
        i32.accept(out, 28 + offsets.size()); i32.accept(out, 0);
        out.writeBytes(offsets.toByteArray());
        out.writeBytes(strings.toByteArray());
        out.writeBytes(body.toByteArray());
        return out.toByteArray();
    }

    /** An APK holding only that manifest. */
    static File apkFile(File dir, String name, byte[] manifest) throws Exception {
        File f = new File(dir, name);
        try (java.util.zip.ZipOutputStream z = new java.util.zip.ZipOutputStream(new java.io.FileOutputStream(f))) {
            z.putNextEntry(new java.util.zip.ZipEntry("AndroidManifest.xml"));
            z.write(manifest);
            z.closeEntry();
        }
        return f;
    }

    static String exe() { return ProcessHandle.current().info().command().orElseThrow(); }

    /** One copy of this program on `args`, with the scenario's android root. */
    static Process start(File dir, String... args) throws Exception {
        ProcessBuilder pb = new ProcessBuilder(self(args)).inheritIO();
        pb.environment().put("WINAPK_TEST_ANDROID_ROOT", new File(dir, "android").getPath());
        pb.environment().put("MACAPK_BROKER_ROOT", new File(dir, "broker-root").getPath());
        pb.environment().put("MACAPK_GMS_DIR", new File(dir, "gms").getPath());
        pb.environment().remove("MACAPK_GMS_PRESENT");
        return pb.start();
    }

    /** `exe --notify ...`: the launcher's call (or anyone else's), its exit code. */
    static int notify(String exe, String name, int op, String pkg, File apk, boolean dataRemoved) throws Exception {
        Process p = new ProcessBuilder(exe, "--notify", name, Integer.toString(op), pkg, apk.getPath(),
                dataRemoved ? "1" : "0", exe()).inheritIO().start();
        if (!p.waitFor(60, TimeUnit.SECONDS)) { p.destroyForcibly(); return -1; }
        return p.exitValue();
    }

    /** The events an "app" child wrote, one line each: action, data, registered-only, replacing,
     *  data removed, uid. */
    static List<String[]> events(File f) {
        List<String[]> out = new ArrayList<>();
        try {
            for (String l : Files.readAllLines(f.toPath(), StandardCharsets.UTF_8)) out.add(l.split("\t", -1));
        } catch (Exception ignored) {}
        return out;
    }

    static String[] waitEvent(File f, String action, String data, int skip) throws InterruptedException {
        long deadline = System.currentTimeMillis() + 30_000;
        while (System.currentTimeMillis() < deadline) {
            int seen = 0;
            for (String[] e : events(f)) {
                if (e[0].equals(action) && e[1].equals(data) && seen++ == skip) return e;
            }
            Thread.sleep(200);
        }
        return null;
    }

    static int count(File f, String action, String data) {
        int n = 0;
        for (String[] e : events(f)) if (e[0].equals(action) && (data == null || e[1].equals(data))) n++;
        return n;
    }

    static int packageEvents(String[] args) throws Exception {
        File dir = new File(args[1]);
        String launcher = args[2];
        System.out.println("package change checks (pid " + myPid() + ", the launcher " + launcher + ")");
        String name = "winapk-test-" + myPid() + "-pkg";
        File listenerApk = apkFile(dir, "listener.apk",
                axml(LISTENER, 34, Intent.ACTION_PACKAGE_ADDED, Intent.ACTION_PACKAGE_FULLY_REMOVED));
        File addedApk = apkFile(dir, "added.apk", axml(ADDED, 34));
        File listenerEvents = new File(dir, "listener.events"), addedEvents = new File(dir, "added.events");
        Files.writeString(listenerEvents.toPath(), "");
        Files.writeString(addedEvents.toPath(), "");

        Process broker = start(dir, "pkgbroker", name, listenerApk.getPath());
        IBinder sys = waitFor(name);
        check("the broker is published", sys != null, "");
        List<Process> apps = new ArrayList<>();
        try {
            if (sys == null) return 1;
            apps.add(start(dir, "app", name, LISTENER, listenerEvents.getPath(), "-", "--apk", listenerApk.getPath()));
            check("an app the broker knows attaches", waitEvent(listenerEvents, "registered", "", 0) != null, "");

            Process early = start(dir, "app", name, ADDED, addedEvents.getPath(), "-", "--apk", addedApk.getPath());
            boolean ended = early.waitFor(120, TimeUnit.SECONDS);
            check("a package not installed yet is refused on attach", ended && early.exitValue() == 3,
                    ended ? "exit " + early.exitValue() : "still running");

            // Anyone but the launcher: this runtime by Binder, and this program by the launcher's own code.
            Parcel d = Parcel.obtain();
            d.writeInterfaceToken(SystemServer.DESCRIPTOR);
            d.writeInt(SystemServer.PACKAGE_ADDED);
            d.writeString(ADDED);
            d.writeString(addedApk.getPath());
            d.writeInt(0);
            check("a package change from a runtime is refused", refused(sys, SystemServer.TX_PACKAGE_CHANGED, d), "");
            int rc = notify(exe(), name, SystemServer.PACKAGE_ADDED, ADDED, addedApk, false);
            check("a package change from a program that is not the launcher is refused", rc == 1, "exit " + rc);
            check("so the package is still unknown", !hasPackage(sys, ADDED), "");

            rc = notify(launcher, name, SystemServer.PACKAGE_ADDED, ADDED, addedApk, false);
            check("the launcher's install commit is taken in", rc == 0, "exit " + rc);
            check("the broker knows the added package at once", hasPackage(sys, ADDED), "");
            String[] e = waitEvent(listenerEvents, Intent.ACTION_PACKAGE_ADDED, "package:" + ADDED, 0);
            check("a running app's dynamic receiver gets PACKAGE_ADDED", e != null, "");
            int uid = AppUids.uidFor(ADDED, null);
            if (e != null) {
                check("PACKAGE_ADDED of a new package: no EXTRA_REPLACING, its uid",
                        e[3].equals("false") && e[5].equals(Integer.toString(uid)), String.join(" ", e));
                check("an app targeting API 34 gets it for its registered receivers only", e[2].equals("1"), e[2]);
            }

            apps.add(start(dir, "app", name, ADDED, addedEvents.getPath(), "spoof", "--apk", addedApk.getPath()));
            check("the added package's process is accepted on attach",
                    waitEvent(addedEvents, "registered", "", 0) != null, "");

            rc = notify(launcher, name, SystemServer.PACKAGE_REPLACED, ADDED, addedApk, false);
            check("the launcher's update commit is taken in", rc == 0, "exit " + rc);
            e = waitEvent(listenerEvents, Intent.ACTION_PACKAGE_ADDED, "package:" + ADDED, 1);
            check("an update sends PACKAGE_ADDED with EXTRA_REPLACING", e != null && e[3].equals("true"),
                    e == null ? "none" : String.join(" ", e));
            e = waitEvent(listenerEvents, Intent.ACTION_PACKAGE_REPLACED, "package:" + ADDED, 0);
            check("and PACKAGE_REPLACED", e != null && e[3].equals("true"), e == null ? "none" : String.join(" ", e));
            e = waitEvent(addedEvents, Intent.ACTION_MY_PACKAGE_REPLACED, "", 0);
            check("the updated package gets MY_PACKAGE_REPLACED", e != null, "");

            rc = notify(launcher, name, SystemServer.PACKAGE_REMOVED, ADDED, addedApk, true);
            check("the launcher's removal commit is taken in", rc == 0, "exit " + rc);
            check("the broker forgets the removed package", !hasPackage(sys, ADDED), "");
            e = waitEvent(listenerEvents, Intent.ACTION_PACKAGE_REMOVED, "package:" + ADDED, 0);
            check("a running app gets PACKAGE_REMOVED with EXTRA_DATA_REMOVED", e != null && e[4].equals("true"),
                    e == null ? "none" : String.join(" ", e));
            e = waitEvent(listenerEvents, Intent.ACTION_PACKAGE_FULLY_REMOVED, "package:" + ADDED, 0);
            check("and PACKAGE_FULLY_REMOVED, to its manifest receivers too (an exempt broadcast)",
                    e != null && e[2].equals("0"), e == null ? "none" : String.join(" ", e));
            check("MY_PACKAGE_REPLACED went to the updated package alone",
                    count(listenerEvents, Intent.ACTION_MY_PACKAGE_REPLACED, null) == 0, "");
            check("the removed package does not hear its own removal",
                    count(addedEvents, Intent.ACTION_PACKAGE_REMOVED, null) == 0, "");
            check("an app's own PACKAGE_ADDED broadcast reaches nobody",
                    count(listenerEvents, Intent.ACTION_PACKAGE_ADDED, "package:" + FAKE) == 0, "");

            Process late = start(dir, "app", name, ADDED, addedEvents.getPath(), "-", "--apk", addedApk.getPath());
            ended = late.waitFor(120, TimeUnit.SECONDS);
            check("a removed package is refused on attach again", ended && late.exitValue() == 3,
                    ended ? "exit " + late.exitValue() : "still running");
        } finally {
            for (Process p : apps) p.destroyForcibly();
            broker.destroyForcibly();
        }
        return failures == 0 ? 0 : 1;
    }

    static boolean hasPackage(IBinder sys, String pkg) throws Exception {
        Parcel d = Parcel.obtain(), r = Parcel.obtain();
        d.writeInterfaceToken(SystemServer.DESCRIPTOR);
        d.writeString(pkg);
        sys.transact(SystemServer.TX_HAS_PACKAGE, d, r, 0);
        r.readException();
        return r.readInt() != 0;
    }

    static int packageBroker(String[] args) throws Exception {
        SystemServer s = new SystemServer();
        s.addPackage(args[2]);
        MacApkBinderTransport.setUidResolver(s::uidOfPid);
        MacApkBinderTransport.publish(args[1], s.new Stub());
        Thread.sleep(Long.MAX_VALUE);
        return 0;
    }

    /** An app's process as the broker sees one: it registers an application thread that writes down
     *  every broadcast scheduled on it (args: name, package, events file, "spoof" to send a
     *  PACKAGE_ADDED of its own, --apk <apk>). Exit 3 when the registration is refused. */
    static int app(String[] args) throws Exception {
        String name = args[1], pkg = args[2];
        File events = new File(args[3]);
        IBinder sys = waitFor(name);
        if (sys == null) return 1;
        Binder thread = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                if (code != SystemServer.TX_SCHEDULE_BROADCAST) return false;
                data.enforceInterface(SystemServer.THREAD_DESCRIPTOR);
                Intent i = data.readTypedObject(Intent.CREATOR);
                String line = i.getAction() + "\t" + (i.getData() == null ? "" : i.getData().toString()) + "\t"
                        + ((i.getFlags() & Intent.FLAG_RECEIVER_REGISTERED_ONLY) != 0 ? "1" : "0") + "\t"
                        + i.getBooleanExtra(Intent.EXTRA_REPLACING, false) + "\t"
                        + i.getBooleanExtra(Intent.EXTRA_DATA_REMOVED, false) + "\t"
                        + i.getIntExtra(Intent.EXTRA_UID, -1) + "\n";
                synchronized (BinderTransportTest.class) {
                    try {
                        Files.writeString(events.toPath(), line, StandardCharsets.UTF_8,
                                java.nio.file.StandardOpenOption.APPEND);
                    } catch (Exception e) {
                        throw new RuntimeException(e);
                    }
                }
                return true;
            }
        };
        Parcel d = Parcel.obtain(), r = Parcel.obtain();
        d.writeInterfaceToken(SystemServer.DESCRIPTOR);
        d.writeString(pkg);
        d.writeStrongBinder(thread);
        d.writeString(args[6]);
        d.writeString(pkg);
        sys.transact(SystemServer.TX_REGISTER, d, r, 0);
        try { r.readException(); } catch (SecurityException e) { return 3; }
        synchronized (BinderTransportTest.class) {
            Files.writeString(events.toPath(), "registered\t\n", StandardCharsets.UTF_8,
                    java.nio.file.StandardOpenOption.APPEND);
        }
        if (args[4].equals("spoof")) {
            d = Parcel.obtain();
            d.writeInterfaceToken(SystemServer.DESCRIPTOR);
            d.writeString(pkg);
            d.writeTypedObject(new Intent(Intent.ACTION_PACKAGE_ADDED, android.net.Uri.parse("package:" + FAKE)), 0);
            d.writeString(pkg);
            sys.transact(SystemServer.TX_BROADCAST, d, null, IBinder.FLAG_ONEWAY);
        }
        Thread.sleep(Long.MAX_VALUE);
        return 0;
    }

    // ---- JobScheduler through the broker ---------------------------------------------

    /** A job of a package whose process is gone runs when it is due: the "jobclient"
     *  child reports a job to the broker and exits; the broker keeps it (BrokerJobs),
     *  and once it is due starts the package's process ("jobhost", the stand-in for the
     *  runtime the broker would start) and hands it the job (JOB_RUN). */
    static void jobChecks() throws Exception {
        File dir = scratch("jobs");
        File good = new File(dir, "good.apk");
        Files.writeString(good.toPath(), "not a real apk");
        Files.writeString(new File(dir, "good.xml").toPath(), manifest(GOOD));
        File marker = new File(dir, "job-ran.txt");
        marker.delete();
        String name = "winapk-test-" + myPid() + "-jobs";

        Process broker = new ProcessBuilder(self("jobbroker", name, dir.getPath(), marker.getPath()))
                .inheritIO().start();
        IBinder sys = waitFor(name);
        check("the jobs broker is published", sys != null, "");
        if (sys == null) { broker.destroy(); return; }

        Process client = new ProcessBuilder(self("jobclient", name, "--apk", good.getPath())).inheritIO().start();
        boolean done = client.waitFor(120, TimeUnit.SECONDS);
        int rc = done ? client.exitValue() : -1;
        check("the client reported its job and exited", rc == 0, "exit " + rc);

        long deadline = System.currentTimeMillis() + 120_000;
        while (!marker.isFile() && System.currentTimeMillis() < deadline) Thread.sleep(200);
        String ran = marker.isFile() ? Files.readString(marker.toPath()).trim() : "";
        check("a job of a package with no process left runs when due: the broker starts the package for it",
                ran.startsWith("op=" + SystemServer.JOB_RUN + " id=" + JOB_ID + " "), ran);
        check("the job runs no earlier than it was due", ran.endsWith(" late=1"), ran);
        broker.destroy();
    }

    static final int JOB_ID = 4242;

    static int jobBroker(String[] args) throws Exception {
        System.setProperty("macapk.broker.jobsPollMs", "300");
        String name = args[1];
        File dir = new File(args[2]);
        String marker = args[3];
        SystemServer s = new SystemServer();
        java.lang.reflect.Field f = SystemServer.class.getDeclaredField("packages");
        f.setAccessible(true);
        @SuppressWarnings("unchecked")
        java.util.Map<String, SystemServer.PackageRecord> packages =
                (java.util.Map<String, SystemServer.PackageRecord>) f.get(s);
        packages.put(GOOD, new SystemServer.PackageRecord(GOOD, new File(dir, "good.apk").getPath(),
                Manifest.parse(new File(dir, "good.xml"), null)));
        s.jobsFile = new File(dir, "jobs.bin");
        // The runtime the broker would start for the package: a child of this test, told where to note the job.
        s.launchCommand = (pr, proc) -> self("jobhost", name, pr.pkg, marker);
        s.mayStartForJob = pkg -> true;
        s.conditionsOverride = new BrokerJobs.Conditions() {
            @Override public boolean networkUp() { return true; }
            @Override public boolean networkMetered() { return false; }
            @Override public boolean charging() { return true; }
            @Override public int batteryPercent() { return -1; }
            @Override public long freeStorageBytes() { return Long.MAX_VALUE; }
            @Override public boolean idle() { return true; }
        };
        s.bootDone = true;
        MacApkBinderTransport.setUidResolver(s::uidOfPid);
        MacApkBinderTransport.publish(name, s.new Stub());
        s.startJobs();
        Thread.sleep(Long.MAX_VALUE);
        return 0;
    }

    /** Registers as GOOD (its command line's --apk), reports one job due 1.5 s from now,
     *  and exits: the broker now holds the job. */
    static int jobClient(String[] args) throws Exception {
        IBinder sys = waitFor(args[1]);
        if (sys == null) return 1;
        Parcel d = Parcel.obtain(), r = Parcel.obtain();
        d.writeInterfaceToken(SystemServer.DESCRIPTOR);
        d.writeString(GOOD);
        d.writeStrongBinder(new Binder());
        d.writeString(args[3]);
        d.writeString(GOOD);
        sys.transact(SystemServer.TX_REGISTER, d, r, 0);
        r.readException();
        android.app.job.JobInfo job = new android.app.job.JobInfo.Builder(JOB_ID,
                new ComponentName(GOOD, GOOD + ".Private"))
                .setPersisted(true).setRequiredNetworkType(android.app.job.JobInfo.NETWORK_TYPE_UNMETERED).build();
        long due = System.currentTimeMillis() + 1500;
        Files.writeString(new File(System.getProperty("java.io.tmpdir"), "winapk-job-due-" + JOB_ID).toPath(),
                Long.toString(due));
        d = Parcel.obtain();
        d.writeInterfaceToken(SystemServer.DESCRIPTOR);
        d.writeString(GOOD);
        d.writeString(GOOD);
        d.writeInt(SystemClient.JOB_REPORT_PUT);
        d.writeInt(JOB_ID);
        d.writeLong(due);
        d.writeInt(0);
        d.writeTypedObject(job, 0);
        sys.transact(SystemServer.TX_JOB_REPORT, d, null, IBinder.FLAG_ONEWAY);
        Thread.sleep(500);   // the oneway report is in before this process goes
        return 0;
    }

    /** The package's process the broker started for the job: registers, and notes the
     *  job the broker hands its application thread. */
    static int jobHost(String[] args) throws Exception {
        IBinder sys = waitFor(args[1]);
        if (sys == null) return 1;
        String pkg = args[2];
        File marker = new File(args[3]);
        Binder thread = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                if (code != SystemServer.TX_SCHEDULE_JOB) return false;
                data.enforceInterface(SystemServer.THREAD_DESCRIPTOR);
                int op = data.readInt();
                android.app.job.JobInfo info = data.readTypedObject(android.app.job.JobInfo.CREATOR);
                long due = 0;
                try {
                    due = Long.parseLong(Files.readString(new File(System.getProperty("java.io.tmpdir"),
                            "winapk-job-due-" + JOB_ID).toPath()).trim());
                } catch (Exception ignored) {}
                boolean late = System.currentTimeMillis() >= due;
                try {
                    Files.writeString(marker.toPath(), "op=" + op + " id=" + (info != null ? info.getId() : -1)
                            + " late=" + (late ? 1 : 0));
                } catch (Exception e) {
                    throw new IllegalStateException(e);
                }
                reply.writeNoException();
                reply.writeInt(android.app.job.JobScheduler.RESULT_SUCCESS);
                return true;
            }
        };
        Parcel d = Parcel.obtain(), r = Parcel.obtain();
        d.writeInterfaceToken(SystemServer.DESCRIPTOR);
        d.writeString(pkg);
        d.writeStrongBinder(thread);
        d.writeString("");
        d.writeString(pkg);
        sys.transact(SystemServer.TX_REGISTER, d, r, 0);
        r.readException();
        Thread.sleep(60_000);   // alive while the broker hands over the job; the broker ends us
        return 0;
    }

    // ---- Google accounts -------------------------------------------------------------

    static final String GOOGLE = "com.google";

    static void googleAccountChecks() throws Exception {
        File dir = scratch("accounts");
        File root = new File(dir, "android");
        root.mkdirs();
        ProcessBuilder pb = new ProcessBuilder(self("accounts")).inheritIO();
        pb.environment().put("WINAPK_TEST_ANDROID_ROOT", root.getPath());
        pb.environment().put("MACAPK_GMS_DIR", new File(dir, "gms").getPath());
        pb.environment().remove("MACAPK_GMS_PRESENT");
        Process p = pb.start();
        boolean done = p.waitFor(180, TimeUnit.SECONDS);
        if (!done) p.destroyForcibly();
        int rc = done ? p.exitValue() : -1;
        check("the Google account checks (above) passed", rc == 0, "exit " + rc);
    }

    /** Another process writing GmsStore, as a sign-in in an app or in Settings does. */
    static boolean gmsWriter(String op, String email) throws Exception {
        ProcessBuilder pb = new ProcessBuilder(self("gms-write", op, email)).inheritIO();
        Process p = pb.start();
        return p.waitFor(60, TimeUnit.SECONDS) && p.exitValue() == 0;
    }

    static int gmsWrite(String[] args) {
        String email = args[2];
        if (args[1].equals("add")) {
            com.macapk.gms.GmsStore.putAccount(email, "master-" + email);
            com.macapk.gms.GmsStore.putAccountData(email, "accountId", "4242" + email.length());
        } else {
            com.macapk.gms.GmsStore.removeAccount(email);
        }
        return 0;
    }

    static int accounts() throws Exception {
        System.out.println("Google account checks (pid " + myPid() + ")");
        final String a = "first@example.com", b = "second@example.com", c = "third@example.com";
        final String other = "kept@example.com";
        check("first account written", gmsWriter("add", a), "");
        File storeFile = new File(new File(System.getenv("MACAPK_GMS_DIR")).getParentFile(), "accounts_ce.properties");
        com.macapk.accounts.AccountStore store = com.macapk.accounts.AccountStore.at(storeFile);
        // A com.google row the service did not mirror (added the way Play services' own
        // sign-in would add one): a sign-out in Win.apk must leave it alone.
        store.add(other, GOOGLE, "pw", null);
        AccountsService svc = new AccountsService(new SystemServer(), storeFile);
        check("an account signed in before the service started is mirrored", store.exists(a, GOOGLE), "");
        check("its master token is the row's password", ("master-" + a).equals(store.password(a, GOOGLE)),
                String.valueOf(store.password(a, GOOGLE) != null));
        check("its gaia id is GoogleUserId", ("4242" + a.length()).equals(store.userData(a, GOOGLE, "GoogleUserId")),
                String.valueOf(store.userData(a, GOOGLE, "GoogleUserId")));
        check("a second sync changes nothing", !svc.syncGoogleAccounts(), "");

        check("second account written by another process", gmsWriter("add", b), "");
        boolean changed = svc.syncGoogleAccounts();
        check("an account signed in by another process after the start is mirrored",
                changed && store.exists(b, GOOGLE), "changed=" + changed);
        check("the first account is still there", store.exists(a, GOOGLE), "");

        check("first account signed out by another process", gmsWriter("remove", a), "");
        changed = svc.syncGoogleAccounts();
        check("an account signed out elsewhere leaves the table", changed && !store.exists(a, GOOGLE),
                "changed=" + changed);
        check("an account Win.apk did not mirror stays", store.exists(other, GOOGLE), "");
        check("the other account stays", store.exists(b, GOOGLE), "");

        // Nobody asks: the service follows the store's file by itself.
        check("third account written by another process", gmsWriter("add", c), "");
        long deadline = System.currentTimeMillis() + 20_000;
        while (!store.exists(c, GOOGLE) && System.currentTimeMillis() < deadline) Thread.sleep(200);
        check("a sign-in elsewhere is mirrored without a query", store.exists(c, GOOGLE), "");
        return failures == 0 ? 0 : 1;
    }
}
