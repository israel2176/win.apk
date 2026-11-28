package libcore.io;

import static android.system.OsConstants.ENOSYS;

import android.system.ErrnoException;

/**
 * Win.apk's libcore.io.Linux: the Os that {@code Libcore.rawOs} and {@code Libcore.os} (through
 * BlockGuardOs) wrap. AOSP's is all natives (libcore_io_Linux.cpp), which HotSpot has no library
 * for; android-all's class came into aosp-resources.jar with the classes that name it, and every
 * call threw UnsatisfiedLinkError (AliExpress' LinuxTaskTracker reads {@code Libcore.os.sysconf}
 * by reflection during its first activity's onCreate). This one, compiled over android-all's in
 * build_aosp_res.sh, answers each call as android.system.Os does: Win.apk's Os already answers
 * them as a phone does (its natives are the host's). What Win.apk's Os has not got fails with
 * ENOSYS, the errno a kernel gives for a system call it does not have, which apps already handle
 * as an ErrnoException; the few calls that cannot throw one answer as bionic does with nothing
 * to report (no tty, no fdsan tag, no interface by that name).
 *
 * The method list is libcore.io.Os of android-all 14; the bodies were generated from it and
 * android.system.Os's statics, so a call Win.apk's Os learns later is one line here.
 */
public final class Linux implements Os {
    /** The process's file mode creation mask, as umask(2) keeps it: an app's process starts with 077. */
    private static int umask = 077;

    Linux() {}

    @Override public java.io.FileDescriptor accept(java.io.FileDescriptor a0, java.net.SocketAddress a1) throws android.system.ErrnoException, java.net.SocketException {
        throw new ErrnoException("accept", ENOSYS);
    }

    @Override public boolean access(java.lang.String a0, int a1) throws android.system.ErrnoException {
        android.system.Os.stat(a0);  // throws ENOENT as access(2) does when the path is missing
        return true;
    }

    @Override public java.net.InetAddress[] android_getaddrinfo(java.lang.String a0, android.system.StructAddrinfo a1, int a2) throws android.system.GaiException {
        try {
            return java.net.InetAddress.getAllByName(a0);
        } catch (java.net.UnknownHostException e) {
            throw new android.system.GaiException("android_getaddrinfo", android.system.OsConstants.EAI_NODATA);
        }
    }

    @Override public void bind(java.io.FileDescriptor a0, java.net.InetAddress a1, int a2) throws android.system.ErrnoException, java.net.SocketException {
        throw new ErrnoException("bind", ENOSYS);
    }

    @Override public void bind(java.io.FileDescriptor a0, java.net.SocketAddress a1) throws android.system.ErrnoException, java.net.SocketException {
        throw new ErrnoException("bind", ENOSYS);
    }

    @Override public android.system.StructCapUserData[] capget(android.system.StructCapUserHeader a0) throws android.system.ErrnoException {
        throw new ErrnoException("capget", ENOSYS);
    }

    @Override public void capset(android.system.StructCapUserHeader a0, android.system.StructCapUserData[] a1) throws android.system.ErrnoException {
        throw new ErrnoException("capset", ENOSYS);
    }

    @Override public void chmod(java.lang.String a0, int a1) throws android.system.ErrnoException {
        android.system.Os.chmod(a0, a1);
    }

    @Override public void chown(java.lang.String a0, int a1, int a2) throws android.system.ErrnoException {
        throw new ErrnoException("chown", ENOSYS);
    }

    @Override public void close(java.io.FileDescriptor a0) throws android.system.ErrnoException {
        android.system.Os.close(a0);
    }

    @Override public void android_fdsan_exchange_owner_tag(java.io.FileDescriptor a0, long a1, long a2) {
        
    }

    @Override public long android_fdsan_get_owner_tag(java.io.FileDescriptor a0) {
        return 0;
    }

    @Override public java.lang.String android_fdsan_get_tag_type(long a0) {
        return "unowned";
    }

    @Override public long android_fdsan_get_tag_value(long a0) {
        return 0;
    }

    @Override public void connect(java.io.FileDescriptor a0, java.net.InetAddress a1, int a2) throws android.system.ErrnoException, java.net.SocketException {
        throw new ErrnoException("connect", ENOSYS);
    }

    @Override public void connect(java.io.FileDescriptor a0, java.net.SocketAddress a1) throws android.system.ErrnoException, java.net.SocketException {
        throw new ErrnoException("connect", ENOSYS);
    }

    @Override public java.io.FileDescriptor dup(java.io.FileDescriptor a0) throws android.system.ErrnoException {
        return android.system.Os.dup(a0);
    }

    @Override public java.io.FileDescriptor dup2(java.io.FileDescriptor a0, int a1) throws android.system.ErrnoException {
        return android.system.Os.dup2(a0, a1);
    }

    @Override public java.lang.String[] environ() {
        java.util.List<String> out = new java.util.ArrayList<>();
        for (java.util.Map.Entry<String, String> e : System.getenv().entrySet()) out.add(e.getKey() + "=" + e.getValue());
        return out.toArray(new String[0]);
    }

    @Override public void execv(java.lang.String a0, java.lang.String[] a1) throws android.system.ErrnoException {
        throw new ErrnoException("execv", ENOSYS);
    }

    @Override public void execve(java.lang.String a0, java.lang.String[] a1, java.lang.String[] a2) throws android.system.ErrnoException {
        throw new ErrnoException("execve", ENOSYS);
    }

    @Override public void fchmod(java.io.FileDescriptor a0, int a1) throws android.system.ErrnoException {
        android.system.Os.fchmod(a0, a1);
    }

    @Override public void fchown(java.io.FileDescriptor a0, int a1, int a2) throws android.system.ErrnoException {
        throw new ErrnoException("fchown", ENOSYS);
    }

    @Override public int fcntlInt(java.io.FileDescriptor a0, int a1, int a2) throws android.system.ErrnoException {
        throw new ErrnoException("fcntlInt", ENOSYS);
    }

    @Override public int fcntlVoid(java.io.FileDescriptor a0, int a1) throws android.system.ErrnoException {
        throw new ErrnoException("fcntlVoid", ENOSYS);
    }

    @Override public void fdatasync(java.io.FileDescriptor a0) throws android.system.ErrnoException {
        android.system.Os.fdatasync(a0);
    }

    @Override public android.system.StructStat fstat(java.io.FileDescriptor a0) throws android.system.ErrnoException {
        return android.system.Os.fstat(a0);
    }

    @Override public android.system.StructStatVfs fstatvfs(java.io.FileDescriptor a0) throws android.system.ErrnoException {
        return android.system.Os.fstatvfs(a0);
    }

    @Override public void fsync(java.io.FileDescriptor a0) throws android.system.ErrnoException {
        android.system.Os.fsync(a0);
    }

    @Override public void ftruncate(java.io.FileDescriptor a0, long a1) throws android.system.ErrnoException {
        android.system.Os.ftruncate(a0, a1);
    }

    @Override public java.lang.String gai_strerror(int a0) {
        return "getaddrinfo error " + a0;
    }

    @Override public int getegid() {
        return android.system.Os.getegid();
    }

    @Override public int geteuid() {
        return android.system.Os.geteuid();
    }

    @Override public int getgid() {
        return android.system.Os.getgid();
    }

    @Override public java.lang.String getenv(java.lang.String a0) {
        return android.system.Os.getenv(a0);
    }

    @Override public java.lang.String getnameinfo(java.net.InetAddress a0, int a1) throws android.system.GaiException {
        return a0.getHostName();
    }

    @Override public java.net.SocketAddress getpeername(java.io.FileDescriptor a0) throws android.system.ErrnoException {
        throw new ErrnoException("getpeername", ENOSYS);
    }

    @Override public int getpgid(int a0) throws android.system.ErrnoException {
        throw new ErrnoException("getpgid", ENOSYS);
    }

    @Override public int getpid() {
        return android.system.Os.getpid();
    }

    @Override public int getppid() {
        return android.system.Os.getppid();
    }

    @Override public android.system.StructPasswd getpwnam(java.lang.String a0) throws android.system.ErrnoException {
        throw new ErrnoException("getpwnam", ENOSYS);
    }

    @Override public android.system.StructPasswd getpwuid(int a0) throws android.system.ErrnoException {
        throw new ErrnoException("getpwuid", ENOSYS);
    }

    @Override public android.system.StructRlimit getrlimit(int a0) throws android.system.ErrnoException {
        throw new ErrnoException("getrlimit", ENOSYS);
    }

    @Override public java.net.SocketAddress getsockname(java.io.FileDescriptor a0) throws android.system.ErrnoException {
        throw new ErrnoException("getsockname", ENOSYS);
    }

    @Override public int getsockoptByte(java.io.FileDescriptor a0, int a1, int a2) throws android.system.ErrnoException {
        throw new ErrnoException("getsockoptByte", ENOSYS);
    }

    @Override public java.net.InetAddress getsockoptInAddr(java.io.FileDescriptor a0, int a1, int a2) throws android.system.ErrnoException {
        throw new ErrnoException("getsockoptInAddr", ENOSYS);
    }

    @Override public int getsockoptInt(java.io.FileDescriptor a0, int a1, int a2) throws android.system.ErrnoException {
        throw new ErrnoException("getsockoptInt", ENOSYS);
    }

    @Override public android.system.StructLinger getsockoptLinger(java.io.FileDescriptor a0, int a1, int a2) throws android.system.ErrnoException {
        throw new ErrnoException("getsockoptLinger", ENOSYS);
    }

    @Override public android.system.StructTimeval getsockoptTimeval(java.io.FileDescriptor a0, int a1, int a2) throws android.system.ErrnoException {
        throw new ErrnoException("getsockoptTimeval", ENOSYS);
    }

    @Override public android.system.StructUcred getsockoptUcred(java.io.FileDescriptor a0, int a1, int a2) throws android.system.ErrnoException {
        throw new ErrnoException("getsockoptUcred", ENOSYS);
    }

    @Override public int gettid() {
        return android.system.Os.gettid();
    }

    @Override public int getuid() {
        return android.system.Os.getuid();
    }

    @Override public byte[] getxattr(java.lang.String a0, java.lang.String a1) throws android.system.ErrnoException {
        throw new ErrnoException("getxattr", ENOSYS);
    }

    @Override public android.system.StructIfaddrs[] getifaddrs() throws android.system.ErrnoException {
        throw new ErrnoException("getifaddrs", ENOSYS);
    }

    @Override public java.lang.String if_indextoname(int a0) {
        return null;
    }

    @Override public int if_nametoindex(java.lang.String a0) {
        return 0;
    }

    @Override public java.net.InetAddress inet_pton(int a0, java.lang.String a1) {
        try {
            if (a1 == null || a1.isEmpty() || !(a1.indexOf(':') >= 0 || Character.digit(a1.charAt(0), 16) >= 0)) return null;
            java.net.InetAddress a = java.net.InetAddress.getByName(a1);
            boolean v6 = a instanceof java.net.Inet6Address;
            return (a0 == android.system.OsConstants.AF_INET6) == v6 ? a : null;
        } catch (java.net.UnknownHostException e) {
            return null;
        }
    }

    @Override public int ioctlFlags(java.io.FileDescriptor a0, java.lang.String a1) throws android.system.ErrnoException {
        throw new ErrnoException("ioctlFlags", ENOSYS);
    }

    @Override public java.net.InetAddress ioctlInetAddress(java.io.FileDescriptor a0, int a1, java.lang.String a2) throws android.system.ErrnoException {
        throw new ErrnoException("ioctlInetAddress", ENOSYS);
    }

    @Override public int ioctlInt(java.io.FileDescriptor a0, int a1) throws android.system.ErrnoException {
        throw new ErrnoException("ioctlInt", ENOSYS);
    }

    @Override public int ioctlMTU(java.io.FileDescriptor a0, java.lang.String a1) throws android.system.ErrnoException {
        throw new ErrnoException("ioctlMTU", ENOSYS);
    }

    @Override public boolean isatty(java.io.FileDescriptor a0) {
        return false;
    }

    @Override public void kill(int a0, int a1) throws android.system.ErrnoException {
        throw new ErrnoException("kill", ENOSYS);
    }

    @Override public void lchown(java.lang.String a0, int a1, int a2) throws android.system.ErrnoException {
        throw new ErrnoException("lchown", ENOSYS);
    }

    @Override public void link(java.lang.String a0, java.lang.String a1) throws android.system.ErrnoException {
        throw new ErrnoException("link", ENOSYS);
    }

    @Override public void listen(java.io.FileDescriptor a0, int a1) throws android.system.ErrnoException {
        throw new ErrnoException("listen", ENOSYS);
    }

    @Override public java.lang.String[] listxattr(java.lang.String a0) throws android.system.ErrnoException {
        throw new ErrnoException("listxattr", ENOSYS);
    }

    @Override public long lseek(java.io.FileDescriptor a0, long a1, int a2) throws android.system.ErrnoException {
        return android.system.Os.lseek(a0, a1, a2);
    }

    @Override public android.system.StructStat lstat(java.lang.String a0) throws android.system.ErrnoException {
        return android.system.Os.lstat(a0);
    }

    @Override public java.io.FileDescriptor memfd_create(java.lang.String a0, int a1) throws android.system.ErrnoException {
        return android.system.Os.memfd_create(a0, a1);
    }

    @Override public void mincore(long a0, long a1, byte[] a2) throws android.system.ErrnoException {
        throw new ErrnoException("mincore", ENOSYS);
    }

    @Override public void mkdir(java.lang.String a0, int a1) throws android.system.ErrnoException {
        android.system.Os.mkdir(a0, a1);
    }

    @Override public void mkfifo(java.lang.String a0, int a1) throws android.system.ErrnoException {
        android.system.Os.mkfifo(a0, a1);
    }

    @Override public void mlock(long a0, long a1) throws android.system.ErrnoException {
        throw new ErrnoException("mlock", ENOSYS);
    }

    @Override public long mmap(long a0, long a1, int a2, int a3, java.io.FileDescriptor a4, long a5) throws android.system.ErrnoException {
        throw new ErrnoException("mmap", ENOSYS);
    }

    @Override public void msync(long a0, long a1, int a2) throws android.system.ErrnoException {
        throw new ErrnoException("msync", ENOSYS);
    }

    @Override public void munlock(long a0, long a1) throws android.system.ErrnoException {
        throw new ErrnoException("munlock", ENOSYS);
    }

    @Override public void munmap(long a0, long a1) throws android.system.ErrnoException {
        throw new ErrnoException("munmap", ENOSYS);
    }

    @Override public java.io.FileDescriptor open(java.lang.String a0, int a1, int a2) throws android.system.ErrnoException {
        return android.system.Os.open(a0, a1, a2);
    }

    @Override public java.io.FileDescriptor[] pipe2(int a0) throws android.system.ErrnoException {
        throw new ErrnoException("pipe2", ENOSYS);
    }

    @Override public int poll(android.system.StructPollfd[] a0, int a1) throws android.system.ErrnoException {
        throw new ErrnoException("poll", ENOSYS);
    }

    @Override public void posix_fallocate(java.io.FileDescriptor a0, long a1, long a2) throws android.system.ErrnoException {
        android.system.Os.posix_fallocate(a0, a1, a2);
    }

    @Override public int prctl(int a0, long a1, long a2, long a3, long a4) throws android.system.ErrnoException {
        return android.system.Os.prctl(a0, a1, a2, a3, a4);
    }

    @Override public int pread(java.io.FileDescriptor a0, java.nio.ByteBuffer a1, long a2) throws android.system.ErrnoException, java.io.InterruptedIOException {
        throw new ErrnoException("pread", ENOSYS);
    }

    @Override public int pread(java.io.FileDescriptor a0, byte[] a1, int a2, int a3, long a4) throws android.system.ErrnoException, java.io.InterruptedIOException {
        return android.system.Os.pread(a0, a1, a2, a3, a4);
    }

    @Override public int pwrite(java.io.FileDescriptor a0, java.nio.ByteBuffer a1, long a2) throws android.system.ErrnoException, java.io.InterruptedIOException {
        throw new ErrnoException("pwrite", ENOSYS);
    }

    @Override public int pwrite(java.io.FileDescriptor a0, byte[] a1, int a2, int a3, long a4) throws android.system.ErrnoException, java.io.InterruptedIOException {
        return android.system.Os.pwrite(a0, a1, a2, a3, a4);
    }

    @Override public int read(java.io.FileDescriptor a0, java.nio.ByteBuffer a1) throws android.system.ErrnoException, java.io.InterruptedIOException {
        return android.system.Os.read(a0, a1);
    }

    @Override public int read(java.io.FileDescriptor a0, byte[] a1, int a2, int a3) throws android.system.ErrnoException, java.io.InterruptedIOException {
        return android.system.Os.read(a0, a1, a2, a3);
    }

    @Override public java.lang.String readlink(java.lang.String a0) throws android.system.ErrnoException {
        return android.system.Os.readlink(a0);
    }

    @Override public java.lang.String realpath(java.lang.String a0) throws android.system.ErrnoException {
        throw new ErrnoException("realpath", ENOSYS);
    }

    @Override public int readv(java.io.FileDescriptor a0, java.lang.Object[] a1, int[] a2, int[] a3) throws android.system.ErrnoException, java.io.InterruptedIOException {
        throw new ErrnoException("readv", ENOSYS);
    }

    @Override public int recvfrom(java.io.FileDescriptor a0, java.nio.ByteBuffer a1, int a2, java.net.InetSocketAddress a3) throws android.system.ErrnoException, java.net.SocketException {
        throw new ErrnoException("recvfrom", ENOSYS);
    }

    @Override public int recvfrom(java.io.FileDescriptor a0, byte[] a1, int a2, int a3, int a4, java.net.InetSocketAddress a5) throws android.system.ErrnoException, java.net.SocketException {
        throw new ErrnoException("recvfrom", ENOSYS);
    }

    @Override public int recvmsg(java.io.FileDescriptor a0, android.system.StructMsghdr a1, int a2) throws android.system.ErrnoException, java.net.SocketException {
        throw new ErrnoException("recvmsg", ENOSYS);
    }

    @Override public void remove(java.lang.String a0) throws android.system.ErrnoException {
        android.system.Os.remove(a0);
    }

    @Override public void removexattr(java.lang.String a0, java.lang.String a1) throws android.system.ErrnoException {
        throw new ErrnoException("removexattr", ENOSYS);
    }

    @Override public void rename(java.lang.String a0, java.lang.String a1) throws android.system.ErrnoException {
        android.system.Os.rename(a0, a1);
    }

    @Override public int sendmsg(java.io.FileDescriptor a0, android.system.StructMsghdr a1, int a2) throws android.system.ErrnoException, java.net.SocketException {
        throw new ErrnoException("sendmsg", ENOSYS);
    }

    @Override public int sendto(java.io.FileDescriptor a0, java.nio.ByteBuffer a1, int a2, java.net.InetAddress a3, int a4) throws android.system.ErrnoException, java.net.SocketException {
        throw new ErrnoException("sendto", ENOSYS);
    }

    @Override public int sendto(java.io.FileDescriptor a0, byte[] a1, int a2, int a3, int a4, java.net.InetAddress a5, int a6) throws android.system.ErrnoException, java.net.SocketException {
        throw new ErrnoException("sendto", ENOSYS);
    }

    @Override public int sendto(java.io.FileDescriptor a0, byte[] a1, int a2, int a3, int a4, java.net.SocketAddress a5) throws android.system.ErrnoException, java.net.SocketException {
        throw new ErrnoException("sendto", ENOSYS);
    }

    @Override public long sendfile(java.io.FileDescriptor a0, java.io.FileDescriptor a1, android.system.Int64Ref a2, long a3) throws android.system.ErrnoException {
        throw new ErrnoException("sendfile", ENOSYS);
    }

    @Override public void setegid(int a0) throws android.system.ErrnoException {
        throw new ErrnoException("setegid", ENOSYS);
    }

    @Override public void setenv(java.lang.String a0, java.lang.String a1, boolean a2) throws android.system.ErrnoException {
        android.system.Os.setenv(a0, a1, a2);
    }

    @Override public void seteuid(int a0) throws android.system.ErrnoException {
        throw new ErrnoException("seteuid", ENOSYS);
    }

    @Override public void setgid(int a0) throws android.system.ErrnoException {
        throw new ErrnoException("setgid", ENOSYS);
    }

    @Override public void setpgid(int a0, int a1) throws android.system.ErrnoException {
        throw new ErrnoException("setpgid", ENOSYS);
    }

    @Override public void setregid(int a0, int a1) throws android.system.ErrnoException {
        throw new ErrnoException("setregid", ENOSYS);
    }

    @Override public void setreuid(int a0, int a1) throws android.system.ErrnoException {
        throw new ErrnoException("setreuid", ENOSYS);
    }

    @Override public int setsid() throws android.system.ErrnoException {
        throw new ErrnoException("setsid", ENOSYS);
    }

    @Override public void setsockoptByte(java.io.FileDescriptor a0, int a1, int a2, int a3) throws android.system.ErrnoException {
        throw new ErrnoException("setsockoptByte", ENOSYS);
    }

    @Override public void setsockoptIfreq(java.io.FileDescriptor a0, int a1, int a2, java.lang.String a3) throws android.system.ErrnoException {
        throw new ErrnoException("setsockoptIfreq", ENOSYS);
    }

    @Override public void setsockoptInt(java.io.FileDescriptor a0, int a1, int a2, int a3) throws android.system.ErrnoException {
        throw new ErrnoException("setsockoptInt", ENOSYS);
    }

    @Override public void setsockoptIpMreqn(java.io.FileDescriptor a0, int a1, int a2, int a3) throws android.system.ErrnoException {
        throw new ErrnoException("setsockoptIpMreqn", ENOSYS);
    }

    @Override public void setsockoptGroupReq(java.io.FileDescriptor a0, int a1, int a2, android.system.StructGroupReq a3) throws android.system.ErrnoException {
        throw new ErrnoException("setsockoptGroupReq", ENOSYS);
    }

    @Override public void setsockoptLinger(java.io.FileDescriptor a0, int a1, int a2, android.system.StructLinger a3) throws android.system.ErrnoException {
        throw new ErrnoException("setsockoptLinger", ENOSYS);
    }

    @Override public void setsockoptTimeval(java.io.FileDescriptor a0, int a1, int a2, android.system.StructTimeval a3) throws android.system.ErrnoException {
        throw new ErrnoException("setsockoptTimeval", ENOSYS);
    }

    @Override public void setuid(int a0) throws android.system.ErrnoException {
        throw new ErrnoException("setuid", ENOSYS);
    }

    @Override public void setxattr(java.lang.String a0, java.lang.String a1, byte[] a2, int a3) throws android.system.ErrnoException {
        throw new ErrnoException("setxattr", ENOSYS);
    }

    @Override public void shutdown(java.io.FileDescriptor a0, int a1) throws android.system.ErrnoException {
        throw new ErrnoException("shutdown", ENOSYS);
    }

    @Override public java.io.FileDescriptor socket(int a0, int a1, int a2) throws android.system.ErrnoException {
        throw new ErrnoException("socket", ENOSYS);
    }

    @Override public void socketpair(int a0, int a1, int a2, java.io.FileDescriptor a3, java.io.FileDescriptor a4) throws android.system.ErrnoException {
        throw new ErrnoException("socketpair", ENOSYS);
    }

    @Override public long splice(java.io.FileDescriptor a0, android.system.Int64Ref a1, java.io.FileDescriptor a2, android.system.Int64Ref a3, long a4, int a5) throws android.system.ErrnoException {
        throw new ErrnoException("splice", ENOSYS);
    }

    @Override public android.system.StructStat stat(java.lang.String a0) throws android.system.ErrnoException {
        return android.system.Os.stat(a0);
    }

    @Override public android.system.StructStatVfs statvfs(java.lang.String a0) throws android.system.ErrnoException {
        return android.system.Os.statvfs(a0);
    }

    @Override public java.lang.String strerror(int a0) {
        return "errno " + a0;
    }

    @Override public java.lang.String strsignal(int a0) {
        return "Signal " + a0;
    }

    @Override public void symlink(java.lang.String a0, java.lang.String a1) throws android.system.ErrnoException {
        android.system.Os.symlink(a0, a1);
    }

    @Override public long sysconf(int a0) {
        return android.system.Os.sysconf(a0);
    }

    @Override public void tcdrain(java.io.FileDescriptor a0) throws android.system.ErrnoException {
        throw new ErrnoException("tcdrain", ENOSYS);
    }

    @Override public void tcsendbreak(java.io.FileDescriptor a0, int a1) throws android.system.ErrnoException {
        throw new ErrnoException("tcsendbreak", ENOSYS);
    }

    @Override public int umask(int a0) {
        int old = umask;
        umask = a0 & 0777;
        return old;
    }

    @Override public android.system.StructUtsname uname() {
        return android.system.Os.uname();
    }

    @Override public void unlink(java.lang.String a0) throws android.system.ErrnoException {
        android.system.Os.unlink(a0);
    }

    @Override public void unsetenv(java.lang.String a0) throws android.system.ErrnoException {
        android.system.Os.unsetenv(a0);
    }

    @Override public int waitpid(int a0, android.system.Int32Ref a1, int a2) throws android.system.ErrnoException {
        throw new ErrnoException("waitpid", ENOSYS);
    }

    @Override public int write(java.io.FileDescriptor a0, java.nio.ByteBuffer a1) throws android.system.ErrnoException, java.io.InterruptedIOException {
        return android.system.Os.write(a0, a1);
    }

    @Override public int write(java.io.FileDescriptor a0, byte[] a1, int a2, int a3) throws android.system.ErrnoException, java.io.InterruptedIOException {
        return android.system.Os.write(a0, a1, a2, a3);
    }

    @Override public int writev(java.io.FileDescriptor a0, java.lang.Object[] a1, int[] a2, int[] a3) throws android.system.ErrnoException, java.io.InterruptedIOException {
        throw new ErrnoException("writev", ENOSYS);
    }
}
