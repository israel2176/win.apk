// android.system.OsConstants — symbolic POSIX errno + mode bits. AOSP:
//   https://cs.android.com/android/platform/superproject/main/+/main:libcore/luni/src/main/java/android/system/OsConstants.java
// We expose only the constants F-Droid (and a handful of other APKs)
// actually reference; this matches the standard Linux values and is
// host-independent.
package android.system;

public final class OsConstants {
    private OsConstants() {}

    // errno
    public static final int EPERM   = 1;
    public static final int ENOENT  = 2;
    public static final int EIO     = 5;
    public static final int EACCES  = 13;
    public static final int EEXIST  = 17;
    public static final int ENOTDIR = 20;
    public static final int EISDIR  = 21;
    public static final int EINVAL  = 22;
    public static final int ENOSPC  = 28;

    // open(2) flags. AOSP's OsConstants exposes the *bionic* O_* values (its
    // native initConstants() reads them straight from bionic's fcntl.h), so an
    // app reads these and passes them to Os.open, which translates them to
    // Darwin's before open(2) (see runtime/src/native_os.cpp NativeOs_open).
    // Values: aosp-mirror/platform_bionic libc/kernel/uapi/asm-generic/fcntl.h
    // (aarch64 uses the asm-generic set). Instagram's InstagramAppShell
    // .attachBaseContext reads O_RDONLY very early; without the field the
    // NoSuchFieldError aborts attachBaseContext, so SoLoader/NativeLoader.init
    // never runs and the entire app DI (IgSessionManager, IgExecutor) cascades.
    public static final int O_ACCMODE   = 00000003;
    public static final int O_RDONLY    = 00000000;
    public static final int O_WRONLY    = 00000001;
    public static final int O_RDWR      = 00000002;
    public static final int O_CREAT     = 00000100;
    public static final int O_EXCL      = 00000200;
    public static final int O_NOCTTY    = 00000400;
    public static final int O_TRUNC     = 00001000;
    public static final int O_APPEND    = 00002000;
    public static final int O_NONBLOCK  = 00004000;
    public static final int O_DSYNC     = 00010000;
    public static final int O_DIRECT    = 00040000;
    public static final int O_LARGEFILE = 00100000;
    public static final int O_DIRECTORY = 00200000;
    public static final int O_NOFOLLOW  = 00400000;
    public static final int O_CLOEXEC   = 02000000;
    public static final int O_SYNC      = 04010000; // __O_SYNC | O_DSYNC
    public static final int O_PATH      = 010000000;

    // st_mode masks
    public static final int S_IFMT   = 0170000;
    public static final int S_IFDIR  = 0040000;
    public static final int S_IFCHR  = 0020000;
    public static final int S_IFBLK  = 0060000;
    public static final int S_IFREG  = 0100000;
    public static final int S_IFIFO  = 0010000;
    public static final int S_IFLNK  = 0120000;
    public static final int S_IFSOCK = 0140000;
    // File-mode PERMISSION bits (Linux sys/stat.h; AOSP OsConstants exposes
    // them). The S_IF* type bits above were present but these were not, so an
    // app reading e.g. OsConstants.S_IXUSR for an Os.chmod mode threw
    // NoSuchFieldError (Google Play services does this during attachBaseContext).
    public static final int S_ISUID = 0004000;
    public static final int S_ISGID = 0002000;
    public static final int S_ISVTX = 0001000;
    public static final int S_IRWXU = 0000700;
    public static final int S_IRUSR = 0000400;
    public static final int S_IWUSR = 0000200;
    public static final int S_IXUSR = 0000100;
    public static final int S_IRWXG = 0000070;
    public static final int S_IRGRP = 0000040;
    public static final int S_IWGRP = 0000020;
    public static final int S_IXGRP = 0000010;
    public static final int S_IRWXO = 0000007;
    public static final int S_IROTH = 0000004;
    public static final int S_IWOTH = 0000002;
    public static final int S_IXOTH = 0000001;

    // mode helpers used by F-Droid's StorageUtils
    public static boolean S_ISDIR (int mode) { return (mode & S_IFMT) == S_IFDIR;  }
    public static boolean S_ISCHR (int mode) { return (mode & S_IFMT) == S_IFCHR;  }
    public static boolean S_ISBLK (int mode) { return (mode & S_IFMT) == S_IFBLK;  }
    public static boolean S_ISREG (int mode) { return (mode & S_IFMT) == S_IFREG;  }
    public static boolean S_ISFIFO(int mode) { return (mode & S_IFMT) == S_IFIFO;  }
    public static boolean S_ISLNK (int mode) { return (mode & S_IFMT) == S_IFLNK;  }
    public static boolean S_ISSOCK(int mode) { return (mode & S_IFMT) == S_IFSOCK; }

    public static final int SEEK_SET = 0;
    public static final int SEEK_CUR = 1;
    public static final int SEEK_END = 2;

    // prctl(2) options (Linux asm-generic/prctl.h). AOSP's OsConstants exposes
    // these and Os.prctl wraps the syscall. Google Play services' Application
    // reads PR_SET_DUMPABLE in its constructor (Os.prctl(PR_SET_DUMPABLE, 1,...)
    // to enable native stack-dump capture); the value must exist or the field
    // access throws NoSuchFieldError (an Error, so its catch(Exception) does not
    // save it). Values are the stable Linux constants.
    public static final int PR_SET_DUMPABLE = 4;
    public static final int PR_GET_DUMPABLE = 3;

    public static final int _SC_CLK_TCK = 6;
    public static final int _SC_PAGE_SIZE = 40;
    public static final int _SC_PAGESIZE = 39;
    public static final int _SC_NPROCESSORS_CONF = 96;
    public static final int _SC_NPROCESSORS_ONLN = 97;
    public static final int ENOSYS = 38;
    public static final int EOPNOTSUPP = 95;
    public static final int EBADF = 9;

    // mmap(2) protections and the one signal apps name (Os.kill(pid, SIGKILL)):
    // Linux's values, as every constant here.
    public static final int PROT_NONE = 0;
    public static final int PROT_READ = 1;
    public static final int PROT_WRITE = 2;
    public static final int PROT_EXEC = 4;
    public static final int SIGKILL = 9;

    /** AOSP errnoName(int): the name of an errno value ("ENOENT"), or null for
     *  one it has no constant for. AOSP's list, with Linux's numbers; where two
     *  names share a number (EOPNOTSUPP/ENOTSUP, EWOULDBLOCK/EAGAIN) it is the
     *  one AOSP's chain of ifs reaches first. */
    public static String errnoName(int errno) {
        switch (errno) {
            case 1: return "EPERM";
            case 2: return "ENOENT";
            case 3: return "ESRCH";
            case 4: return "EINTR";
            case 5: return "EIO";
            case 6: return "ENXIO";
            case 7: return "E2BIG";
            case 8: return "ENOEXEC";
            case 9: return "EBADF";
            case 10: return "ECHILD";
            case 11: return "EAGAIN";
            case 12: return "ENOMEM";
            case 13: return "EACCES";
            case 14: return "EFAULT";
            case 16: return "EBUSY";
            case 17: return "EEXIST";
            case 18: return "EXDEV";
            case 19: return "ENODEV";
            case 20: return "ENOTDIR";
            case 21: return "EISDIR";
            case 22: return "EINVAL";
            case 23: return "ENFILE";
            case 24: return "EMFILE";
            case 25: return "ENOTTY";
            case 26: return "ETXTBSY";
            case 27: return "EFBIG";
            case 28: return "ENOSPC";
            case 29: return "ESPIPE";
            case 30: return "EROFS";
            case 31: return "EMLINK";
            case 32: return "EPIPE";
            case 33: return "EDOM";
            case 34: return "ERANGE";
            case 35: return "EDEADLK";
            case 36: return "ENAMETOOLONG";
            case 37: return "ENOLCK";
            case 38: return "ENOSYS";
            case 39: return "ENOTEMPTY";
            case 40: return "ELOOP";
            case 42: return "ENOMSG";
            case 43: return "EIDRM";
            case 60: return "ENOSTR";
            case 61: return "ENODATA";
            case 62: return "ETIME";
            case 63: return "ENOSR";
            case 64: return "ENONET";
            case 67: return "ENOLINK";
            case 71: return "EPROTO";
            case 72: return "EMULTIHOP";
            case 74: return "EBADMSG";
            case 75: return "EOVERFLOW";
            case 84: return "EILSEQ";
            case 88: return "ENOTSOCK";
            case 89: return "EDESTADDRREQ";
            case 90: return "EMSGSIZE";
            case 91: return "EPROTOTYPE";
            case 92: return "ENOPROTOOPT";
            case 93: return "EPROTONOSUPPORT";
            case 95: return "ENOTSUP";
            case 97: return "EAFNOSUPPORT";
            case 98: return "EADDRINUSE";
            case 99: return "EADDRNOTAVAIL";
            case 100: return "ENETDOWN";
            case 101: return "ENETUNREACH";
            case 102: return "ENETRESET";
            case 103: return "ECONNABORTED";
            case 104: return "ECONNRESET";
            case 105: return "ENOBUFS";
            case 106: return "EISCONN";
            case 107: return "ENOTCONN";
            case 110: return "ETIMEDOUT";
            case 111: return "ECONNREFUSED";
            case 113: return "EHOSTUNREACH";
            case 114: return "EALREADY";
            case 115: return "EINPROGRESS";
            case 116: return "ESTALE";
            case 122: return "EDQUOT";
            case 125: return "ECANCELED";
            default: return null;
        }
    }
}



