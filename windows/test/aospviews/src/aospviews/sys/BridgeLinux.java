package aospviews.sys;

import libcore.io.Linux;

/** The few of libcore.io.Linux's 134 natives (libcore's Os: the system calls) that AOSP's app-side
 *  framework is seen to call while inflating and laying views out: who the process is. Win.apk's
 *  libcore (on the Bionic shim and the host) has all of them; the rest throw here, by name. */
public final class BridgeLinux {
    private BridgeLinux() {}

    public static final int UID = 10123;  // an app uid (Process.FIRST_APPLICATION_UID + 123), user 0

    public static int getuid(Linux l) { return UID; }
    public static int geteuid(Linux l) { return UID; }
    public static int getgid(Linux l) { return UID; }
    public static int getegid(Linux l) { return UID; }
    public static int getpid(Linux l) { return (int) ProcessHandle.current().pid(); }
    public static int getppid(Linux l) { return ProcessHandle.current().parent().map(p -> (int) p.pid()).orElse(1); }
    public static int gettid(Linux l) { return (int) Thread.currentThread().threadId(); }
    public static String getenv(Linux l, String name) { return System.getenv(name); }
}
