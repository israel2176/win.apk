// ProcessCrashesTest -- the system broker's crash record (com.macapk.loader.ProcessCrashes) and what the
// broker does with it: a process that crashes twice within AOSP's MIN_CRASH_INTERVAL is bad, and the
// broker refuses to start it again for anyone but the user, as ActivityManagerService's AppErrors and
// ProcessList do, instead of booting a JVM for every bind of a process that crashes as it starts.
//
//   java -cp android-framework.jar;macapk-loader.jar;aosp-app.jar;<classes> com.macapk.loader.ProcessCrashesTest <scratch dir>
//
// Checks: which exit codes are crashes (app_exit.h's rule); one crash is not bad, a second within the
// interval is, a second after it is not; the user's start and a package change clear it; and with a
// broker whose "runtime" is a child of this test that crashes at once: the first two starts are made
// and fail, the third is refused with no process started, a user's attach clears it, and a request while
// the process starts starts no second copy.
// Prints PASS/FAIL per check and "ALL PASSED" at the end; exits 1 on a failure.
package com.macapk.loader;

import java.io.File;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;

public class ProcessCrashesTest {
    static int failures;

    static void check(String name, boolean ok) {
        System.out.println((ok ? "PASS " : "FAIL ") + name);
        if (!ok) failures++;
    }

    static final String PKG = "com.example.crashy";

    public static void main(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("crash")) {
            Runtime.getRuntime().halt(3);   // abort(): a crash on Windows and elsewhere
        }
        if (args.length > 0 && args[0].equals("slow")) {
            Thread.sleep(3_000);
            Runtime.getRuntime().halt(3);
        }
        File dir = new File(args.length > 0 ? args[0] : System.getProperty("java.io.tmpdir"), "process-crashes");
        dir.mkdirs();
        policy();
        broker(dir);
        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.out.flush();
        System.exit(failures == 0 ? 0 : 1);
    }

    static void policy() {
        check("the runtime's crash handler's exit is a crash", ProcessCrashes.crashExit(0x1000000A));
        check("abort() is a crash", ProcessCrashes.crashExit(3));
        check("an access violation is a crash", ProcessCrashes.crashExit(0xC0000005));
        check("a DLL that would not load is not the app's crash", !ProcessCrashes.crashExit(0xC0000135));
        check("a clean exit is not a crash", !ProcessCrashes.crashExit(0));
        check("being stopped (TerminateProcess 1) is not a crash", !ProcessCrashes.crashExit(1));
        check("another copy running is not a crash", !ProcessCrashes.crashExit(SystemServer.ALREADY_RUNNING_EXIT));

        ProcessCrashes c = new ProcessCrashes();
        long t = 1_000_000L;
        check("one crash does not make a process bad", !c.crashed(PKG, PKG, t) && !c.isBad(PKG, PKG));
        check("a second crash within the interval does", c.crashed(PKG, PKG, t + 10_000) && c.isBad(PKG, PKG));
        check("another process of the package is not bad", !c.isBad(PKG, PKG + ":remote"));
        c.userStarted(PKG, PKG);
        check("the user's start clears it", !c.isBad(PKG, PKG));
        check("and its crash time: the next crash alone is not bad", !c.crashed(PKG, PKG, t + 20_000));
        check("a crash after the interval is not bad",
                !c.crashed(PKG, PKG, t + 20_000 + ProcessCrashes.MIN_CRASH_INTERVAL_MS) && !c.isBad(PKG, PKG));
        c.crashed(PKG, PKG + ":remote", t);
        c.crashed(PKG, PKG + ":remote", t + 1);
        check("a second process crashing twice is bad", c.isBad(PKG, PKG + ":remote"));
        c.forgetPackage(PKG);
        check("a package change clears every process of it", !c.isBad(PKG, PKG + ":remote"));
    }

    static List<String> self(String... args) {
        List<String> cmd = new ArrayList<>();
        cmd.add(ProcessHandle.current().info().command().orElseThrow());
        cmd.add("-cp");
        cmd.add(System.getProperty("java.class.path"));
        cmd.add(ProcessCrashesTest.class.getName());
        for (String a : args) cmd.add(a);
        return cmd;
    }

    static String manifest(String pkg) {
        return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            + "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"" + pkg + "\">\n"
            + "  <application>\n"
            + "    <service android:name=\"" + pkg + ".Svc\" android:exported=\"true\"/>\n"
            + "  </application>\n"
            + "</manifest>\n";
    }

    static void broker(File dir) throws Exception {
        File apk = new File(dir, "crashy.apk");
        Files.writeString(apk.toPath(), "not a real apk");
        File xml = new File(dir, "crashy.xml");
        Files.writeString(xml.toPath(), manifest(PKG));
        SystemServer s = new SystemServer();
        java.lang.reflect.Field f = SystemServer.class.getDeclaredField("packages");
        f.setAccessible(true);
        @SuppressWarnings("unchecked")
        java.util.Map<String, SystemServer.PackageRecord> packages =
                (java.util.Map<String, SystemServer.PackageRecord>) f.get(s);
        packages.put(PKG, new SystemServer.PackageRecord(PKG, apk.getPath(), Manifest.parse(xml, null)));
        AtomicInteger launches = new AtomicInteger();
        String[] mode = { "crash" };
        s.launchCommand = (pr, proc) -> { launches.incrementAndGet(); return self(mode[0]); };

        for (int i = 1; i <= 2; i++) {
            String why = start(s);
            check("start " + i + " is made and fails: the process crashed before attaching (" + why + ")",
                    why.contains("exited (3) before attaching") && launches.get() == i);
        }
        // onExit's crash record runs on the exit's own thread: let it land.
        long deadline = System.currentTimeMillis() + 10_000;
        while (!s.crashes.isBad(PKG, PKG) && System.currentTimeMillis() < deadline) Thread.sleep(50);
        String why = start(s);
        check("a third start is refused with no process started: it is a bad process (" + why + ")",
                why.contains("bad process") && launches.get() == 2);

        s.crashes.userStarted(PKG, PKG);
        mode[0] = "slow";
        int before = launches.get();
        Thread a = new Thread(() -> start(s));
        a.start();
        Thread.sleep(1_000);
        check("after the user's start it starts again", launches.get() - before == 1);
        Thread b = new Thread(() -> start(s));
        b.start();
        Thread.sleep(1_000);
        check("a second request while it starts starts no second copy (" + (launches.get() - before) + ")",
                launches.get() - before == 1);
        a.join();
        b.join();
    }

    /** ensureRunning's failure, or "" when it started. */
    static String start(SystemServer s) {
        try {
            s.ensureRunning(PKG);
            return "";
        } catch (Throwable t) {
            Throwable c = t instanceof java.util.concurrent.ExecutionException && t.getCause() != null
                    ? t.getCause() : t;
            return String.valueOf(c.getMessage());
        }
    }
}
