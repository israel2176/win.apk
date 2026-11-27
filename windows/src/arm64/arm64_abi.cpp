// arm64_abi.cpp -- the Bionic shim's functions whose structs arm64 lays out
// differently from x86_64 (arm64_guest.h, abi_override).
//
// The shim is written against Android's x86_64 headers, which is what the
// x86_64 path needs. Most of bionic's LP64 structs are the same on arm64, but
// not all: struct stat (x86_64: 144 bytes, st_nlink before st_mode; arm64:
// 128, asm-generic order) and struct epoll_event (packed to 12 bytes on
// x86_64, 16 on arm64); open(2)'s flags differ too (O_DIRECTORY is
// x86_64's O_DIRECT). Passing an arm64 caller's struct to the x86_64
// function writes past the end of it -- a stack object in the caller's
// frame, so saved registers and the stack protector's canary go first. An
// arm64 library's imports of these names bind to the wrappers here, which
// call the shim with an x86_64 struct and convert. (On the Mac the shim is
// built for arm64 and has no such gap.)

#include "winapk/arm64_guest.h"

#include "winapk/guest_abi.h"
#include "winapk/host_code.h"
#include "winapk/linker.h"
#include "winapk/mini_bionic.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <windows.h>

namespace winapk::arm64 {

namespace {

// Bionic x86_64 (the shim's GuestStat, src/bionic/stdio.cpp).
struct X86Stat {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid;
    int32_t pad0;
    uint64_t st_rdev;
    int64_t st_size, st_blksize, st_blocks;
    int64_t st_atime_sec, st_atime_nsec, st_mtime_sec, st_mtime_nsec, st_ctime_sec, st_ctime_nsec;
    int64_t unused[3];
};
static_assert(sizeof(X86Stat) == 144, "Bionic's struct stat on x86_64");

// Bionic arm64 (asm-generic/stat.h's layout).
struct Arm64Stat {
    uint64_t st_dev, st_ino;
    uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev, pad1;
    int64_t st_size;
    int32_t st_blksize, pad2;
    int64_t st_blocks;
    int64_t st_atime_sec, st_atime_nsec, st_mtime_sec, st_mtime_nsec, st_ctime_sec, st_ctime_nsec;
    uint32_t unused4, unused5;
};
static_assert(sizeof(Arm64Stat) == 128, "Bionic's struct stat on arm64");

void to_arm64(const X86Stat& x, Arm64Stat* a) {
    std::memset(a, 0, sizeof(*a));
    a->st_dev = x.st_dev;
    a->st_ino = x.st_ino;
    a->st_mode = x.st_mode;
    a->st_nlink = static_cast<uint32_t>(x.st_nlink);
    a->st_uid = x.st_uid;
    a->st_gid = x.st_gid;
    a->st_rdev = x.st_rdev;
    a->st_size = x.st_size;
    a->st_blksize = static_cast<int32_t>(x.st_blksize);
    a->st_blocks = x.st_blocks;
    a->st_atime_sec = x.st_atime_sec;
    a->st_atime_nsec = x.st_atime_nsec;
    a->st_mtime_sec = x.st_mtime_sec;
    a->st_mtime_nsec = x.st_mtime_nsec;
    a->st_ctime_sec = x.st_ctime_sec;
    a->st_ctime_nsec = x.st_ctime_nsec;
}

#pragma pack(push, 4)
struct X86EpollEvent {  // packed on x86_64
    uint32_t events;
    uint64_t data;
};
#pragma pack(pop)
static_assert(sizeof(X86EpollEvent) == 12, "x86_64's packed struct epoll_event");

struct Arm64EpollEvent {
    uint32_t events;
    uint64_t data;
};
static_assert(sizeof(Arm64EpollEvent) == 16, "arm64's struct epoll_event");

template <typename Fn> Fn shim(const char* name) {
    return reinterpret_cast<Fn>(mini_bionic_resolve(name));
}

using PathStat = int(WINAPK_GUEST_ABI*)(const char*, X86Stat*);
using FdStat = int(WINAPK_GUEST_ABI*)(int, X86Stat*);
using AtStat = int(WINAPK_GUEST_ABI*)(int, const char*, X86Stat*, int);

WINAPK_GUEST_ABI int a64_stat(const char* path, Arm64Stat* out) {
    static const auto fn = shim<PathStat>("stat");
    X86Stat x;
    const int r = fn(path, &x);
    if (r == 0 && out) to_arm64(x, out);
    return r;
}
WINAPK_GUEST_ABI int a64_lstat(const char* path, Arm64Stat* out) {
    static const auto fn = shim<PathStat>("lstat");
    X86Stat x;
    const int r = fn(path, &x);
    if (r == 0 && out) to_arm64(x, out);
    return r;
}
WINAPK_GUEST_ABI int a64_fstat(int fd, Arm64Stat* out) {
    static const auto fn = shim<FdStat>("fstat");
    X86Stat x;
    const int r = fn(fd, &x);
    if (r == 0 && out) to_arm64(x, out);
    return r;
}
WINAPK_GUEST_ABI int a64_fstatat(int dirfd, const char* path, Arm64Stat* out, int flags) {
    static const auto fn = shim<AtStat>("fstatat");
    X86Stat x;
    const int r = fn(dirfd, path, &x, flags);
    if (r == 0 && out) to_arm64(x, out);
    return r;
}

// fts: each entry's fts_statp is a struct stat, filled in the caller's layout.
void fill_arm64_stat(const void* x86, void* out) {
    to_arm64(*static_cast<const X86Stat*>(x86), static_cast<Arm64Stat*>(out));
}
WINAPK_GUEST_ABI void* a64_fts_open(char* const* argv, int options, void* compar) {
    return fts_open_with_stat(argv, options, compar ? host_callable(compar) : nullptr, &fill_arm64_stat);
}

using EpollCtl = int(WINAPK_GUEST_ABI*)(int, int, int, X86EpollEvent*);
using EpollWait = int(WINAPK_GUEST_ABI*)(int, X86EpollEvent*, int, int);
using EpollPwait = int(WINAPK_GUEST_ABI*)(int, X86EpollEvent*, int, int, const void*);

WINAPK_GUEST_ABI int a64_epoll_ctl(int epfd, int op, int fd, Arm64EpollEvent* ev) {
    static const auto fn = shim<EpollCtl>("epoll_ctl");
    X86EpollEvent x{};
    if (ev) x = {ev->events, ev->data};
    return fn(epfd, op, fd, ev ? &x : nullptr);
}

int copy_events(int n, const std::vector<X86EpollEvent>& x, Arm64EpollEvent* out) {
    for (int i = 0; i < n; ++i) out[i] = {x[static_cast<size_t>(i)].events, x[static_cast<size_t>(i)].data};
    return n;
}

WINAPK_GUEST_ABI int a64_epoll_wait(int epfd, Arm64EpollEvent* events, int max, int timeout) {
    static const auto fn = shim<EpollWait>("epoll_wait");
    std::vector<X86EpollEvent> x(max > 0 ? static_cast<size_t>(max) : 1);
    const int n = fn(epfd, x.data(), max, timeout);
    return n > 0 ? copy_events(n, x, events) : n;
}

WINAPK_GUEST_ABI int a64_epoll_pwait(int epfd, Arm64EpollEvent* events, int max, int timeout, const void* mask) {
    static const auto fn = shim<EpollPwait>("epoll_pwait");
    std::vector<X86EpollEvent> x(max > 0 ? static_cast<size_t>(max) : 1);
    const int n = fn(epfd, x.data(), max, timeout, mask);
    return n > 0 ? copy_events(n, x, events) : n;
}

// ---- code the guest writes itself ----------------------------------------------
//
// Memory an arm64 caller maps or protects executable holds arm64 code it is
// about to write and run: a JIT of its own (Mono, LuaJIT, V8), or a
// trampoline (Facebook's force_dlopen writes one over a string gai_strerror
// returned, so a call it makes appears to come from libc). The bridge runs
// only registered ranges as guest code, and would call anything else as a
// host function, so such ranges are registered here, and dropped again when
// unmapped or made non-executable. (An x86_64 caller's code runs natively and
// needs none of this, which is why it is only for arm64 imports.)
//
// Never the host's own code (host_code.h): a library that hooks libc on
// Android mprotects libc's text writable and executable, and here that text is
// the shim's x86_64 code in winapk-runtime.exe (or a Windows DLL, or a
// translation's .aot.dll). Registered, the next call into any host function on
// the page would be interpreted as arm64; written, the host's code is gone.
// mprotect and MAP_FIXED over it fail with EACCES instead (asking for
// PROT_READ | PROT_EXEC, what it already is, succeeds and changes nothing),
// and track_exec, which raw calls and mremap reach too, never registers it. An
// image's data pages are not code and stay open (the trampoline above is in
// the exe's .rdata).

constexpr int kProtRead = 1, kProtWrite = 2, kProtExec = 4;
constexpr int kMapFixed = 0x10;
constexpr int kEacces = 13;
constexpr uint64_t kPage = 4096;

// The first address of [lo, hi) that is a host image's code (host_code.h), or 0.
uint64_t host_code_in(uint64_t lo, uint64_t hi) {
    for (uint64_t p = lo; p < hi;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) break;
        const uint64_t region_end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (mbi.State != MEM_FREE && mbi.Type == MEM_IMAGE) {
            const auto* base = static_cast<const uint8_t*>(mbi.AllocationBase);
            // The headers' region, as far as it may be read (a guest may have protected it: then nothing of
            // the image is known, and pe_code_in counts it as code).
            MEMORY_BASIC_INFORMATION head;
            size_t n = 0;
            if (VirtualQuery(base, &head, sizeof head) && head.State == MEM_COMMIT &&
                !(head.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && head.Protect != PAGE_EXECUTE) {
                n = head.RegionSize;
            }
            const uint64_t b = reinterpret_cast<uint64_t>(base);
            const uint64_t end = region_end < hi ? region_end : hi;
            uint64_t at;
            if (pe_code_in(base, n, p - b, end - b, &at)) return b + at;
        }
        p = region_end;
    }
    return 0;
}

// Refuses an arm64 caller's change to host code: EACCES, and one line in the log per process. `what`
// finishes "a guest library's ...".
int refuse_host_code(const char* what, uint64_t at) {
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) {
        char path[MAX_PATH] = "?";
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof mbi);
        GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), path, sizeof path);
        const char* name = std::strrchr(path, '\\');
        name = name ? name + 1 : path;
        std::fprintf(stderr,
                     "[win.apk] arm64: a guest library's %s host code, at %s+0x%llx: refused "
                     "(EACCES). On Android this is how a library patches or hooks libc's code; here libc is "
                     "Win.apk's x86_64 code, which arm64 code can neither patch nor run as its own\n",
                     what, name,
                     static_cast<unsigned long long>(at - reinterpret_cast<uint64_t>(mbi.AllocationBase)));
        std::fflush(stderr);
    }
    *shim<int*(WINAPK_GUEST_ABI*)()>("__errno")() = kEacces;
    return -1;
}

uint64_t page_down(const void* addr) { return reinterpret_cast<uint64_t>(addr) & ~(kPage - 1); }
uint64_t page_up(const void* addr, size_t len) {
    return (reinterpret_cast<uint64_t>(addr) + len + kPage - 1) & ~(kPage - 1);
}

// Only ranges registered here are ever dropped: an app that makes a page of
// a loaded library's own code writable for a moment must not unregister the
// library.
std::mutex g_exec_lock;
std::set<uint64_t> g_exec_ranges;  // starts of the ranges track_exec registered

void track_exec(void* addr, size_t len, int prot) {
    if (!addr || !len) return;
    const uint64_t lo = page_down(addr);
    const uint64_t hi = page_up(addr, len);
    void* start = reinterpret_cast<void*>(lo);
    std::lock_guard<std::mutex> hold(g_exec_lock);
    if (prot & kProtExec) {
        if (is_guest_code(start) || host_code_in(lo, hi)) return;
        register_code(start, hi - lo);
        g_exec_ranges.insert(lo);
    } else if (g_exec_ranges.erase(lo)) {
        unregister_code(start);
    }
}

using Mmap = void*(WINAPK_GUEST_ABI*)(void*, size_t, int, int, int, int64_t);
using Mprotect = int(WINAPK_GUEST_ABI*)(void*, size_t, int);
using Munmap = int(WINAPK_GUEST_ABI*)(void*, size_t);

WINAPK_GUEST_ABI void* a64_mmap(void* addr, size_t len, int prot, int flags, int fd, int64_t off) {
    static const auto fn = shim<Mmap>("mmap");
    if ((flags & kMapFixed) && addr && len) {
        if (const uint64_t at = host_code_in(page_down(addr), page_up(addr, len))) {
            refuse_host_code("mmap(MAP_FIXED) would map over", at);
            return reinterpret_cast<void*>(-1);
        }
    }
    void* p = fn(addr, len, prot, flags, fd, off);
    if (p != reinterpret_cast<void*>(-1) && (prot & kProtExec)) track_exec(p, len, prot);
    return p;
}

WINAPK_GUEST_ABI int a64_mprotect(void* addr, size_t len, int prot) {
    static const auto fn = shim<Mprotect>("mprotect");
    if (addr && len && !(reinterpret_cast<uint64_t>(addr) & (kPage - 1))) {  // a misaligned one is EINVAL
        if (const uint64_t at = host_code_in(page_down(addr), page_up(addr, len))) {
            if (prot == (kProtRead | kProtExec)) return 0;  // what host code is already
            const bool w = prot & kProtWrite, x = prot & kProtExec;
            return refuse_host_code(w && x ? "mprotect would make writable and executable"
                                    : w    ? "mprotect would make writable"
                                    : x    ? "mprotect would make executable"
                                           : "mprotect would make non-executable",
                                    at);
        }
    }
    const int r = fn(addr, len, prot);
    if (r == 0) track_exec(addr, len, prot);
    return r;
}

WINAPK_GUEST_ABI int a64_munmap(void* addr, size_t len) {
    static const auto fn = shim<Munmap>("munmap");
    const int r = fn(addr, len);
    if (r == 0) track_exec(addr, len, 0);
    return r;
}

// ---- system call numbers --------------------------------------------------------
//
// The shim's syscall() takes x86_64's numbers (futex 202, gettid 186...), and
// arm64 uses asm-generic's (futex 98, gettid 178...): an arm64 library's
// syscall(SYS_futex, ...) named another call. Both its syscall() import and
// its raw SVCs come here, are renumbered for the shim, and the calls the
// shim's syscall() does not take go to its libc functions, or to the shim's
// raw entry points where the kernel's interface differs from libc's
// (rt_sigaction's struct, set_tid_address, getpriority's 20 - nice, getcwd's
// length, clock_nanosleep's error number). A call nothing here answers is
// ENOSYS, as from a kernel without it. The numbers are asm-generic's
// (include/uapi/asm-generic/unistd.h, which arch/arm64 uses), and each raw
// call returns as the kernel does, -errno in x0, with the results its man
// page's "C library/kernel differences" give where libc's differ.

using Syscall = int64_t(WINAPK_GUEST_ABI*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
using Fn3 = int64_t(WINAPK_GUEST_ABI*)(int64_t, int64_t, int64_t);
using Fn4 = int64_t(WINAPK_GUEST_ABI*)(int64_t, int64_t, int64_t, int64_t);

// Calls answered by the shim's libc function of the same name, whose arguments
// arm64 lays out as x86_64 does (the shim's functions take x86_64's structs,
// so only calls whose structs are the same on both are here). Protection and
// runtime libraries make these as raw SVCs so that nothing hooking libc sees
// them (Leumi's libe689.so maps its memory with SVC 222). `wide`: the function
// returns a long (ssize_t); the others return an int, whose upper half of rax
// the System V ABI leaves undefined, so only the low half is the result
// (read whole, -1 came back as 4294967295 and its errno was lost).
struct LibcCall {
    uint64_t nr;
    const char* name;
    bool wide;
};
constexpr LibcCall kLibcCalls[] = {
    {5, "setxattr", false},        {6, "lsetxattr", false},        {7, "fsetxattr", false},
    {8, "getxattr", true},         {9, "lgetxattr", true},         {10, "fgetxattr", true},
    {11, "listxattr", true},       {12, "llistxattr", true},       {13, "flistxattr", true},
    {14, "removexattr", false},    {15, "lremovexattr", false},    {16, "fremovexattr", false},
    {19, "eventfd", false},        {20, "epoll_create1", false},   {23, "dup", false},
    {24, "dup3", false},           {25, "fcntl", false},           {26, "inotify_init1", false},
    {27, "inotify_add_watch", false}, {28, "inotify_rm_watch", false}, {29, "ioctl", false},
    {32, "flock", false},          {34, "mkdirat", false},         {35, "unlinkat", false},
    {38, "renameat", false},       {46, "ftruncate", false},       {59, "pipe2", false},
    {65, "readv", true},           {66, "writev", true},           {67, "pread64", true},
    {68, "pwrite64", true},        {78, "readlinkat", true},       {82, "fsync", false},
    {83, "fdatasync", false},      {85, "timerfd_create", false},  {86, "timerfd_settime", false},
    {87, "timerfd_gettime", false}, {101, "nanosleep", false},     {114, "clock_getres", false},
    {117, "ptrace", true},         {132, "sigaltstack", false},    {140, "setpriority", false},
    {160, "uname", false},         {163, "getrlimit", false},      {165, "getrusage", false},
    {167, "prctl", false},         {169, "gettimeofday", false},   {173, "getppid", false},
    {174, "getuid", false},        {175, "geteuid", false},        {176, "getgid", false},
    {177, "getegid", false},       {179, "sysinfo", false},        {198, "socket", false},
    {203, "connect", false},       {227, "msync", false},          {228, "mlock", false},
    {229, "munlock", false},       {230, "mlockall", false},       {231, "munlockall", false},
    {232, "mincore", false},       {261, "prlimit64", false},      {279, "memfd_create", false},
    {284, "mlock2", false},
};

const LibcCall* libc_call(uint64_t nr) {
    for (const auto& c : kLibcCalls) {
        if (c.nr == nr) return mini_bionic_resolve(c.name) ? &c : nullptr;
    }
    return nullptr;
}

bool known_syscall(uint64_t nr) {
    switch (nr) {
        case 98: case 178: case 172: case 124: case 278: case 113: case 233: case 283:   // via the shim's syscall()
        case 123:
        case 56: case 57: case 62: case 63: case 64: case 79: case 80: case 93: case 94:  // via libc functions
        case 96: case 134: case 135:  // set_tid_address, rt_sigaction, rt_sigprocmask
        case 17: case 115: case 141: case 168:  // getcwd, clock_nanosleep, getpriority, getcpu
        case 21: case 22:  // epoll_ctl, epoll_pwait (arm64's struct epoll_event)
        case 48: case 129: case 130: case 131:  // faccessat; kill, tkill, tgkill
        case 222: case 215: case 226: case 216:  // mmap, munmap, mprotect, mremap
            return true;
        default:
            return libc_call(nr) != nullptr;
    }
}

// open(2)'s flags where arm64's differ from x86_64's, which the shim takes: O_DIRECTORY, O_NOFOLLOW,
// O_DIRECT, O_LARGEFILE (arch/arm64/include/uapi/asm/fcntl.h against asm-generic's).
int x86_open_flags(int arm64_flags) {
    constexpr int kA64Directory = 040000, kA64Nofollow = 0100000, kA64Direct = 0200000, kA64Largefile = 0400000;
    constexpr int kX86Directory = 0200000, kX86Nofollow = 0400000, kX86Direct = 040000, kX86Largefile = 0100000;
    int f = arm64_flags & ~(kA64Directory | kA64Nofollow | kA64Direct | kA64Largefile);
    if (arm64_flags & kA64Directory) f |= kX86Directory;
    if (arm64_flags & kA64Nofollow) f |= kX86Nofollow;
    if (arm64_flags & kA64Direct) f |= kX86Direct;
    if (arm64_flags & kA64Largefile) f |= kX86Largefile;
    return f;
}

using Open = int(WINAPK_GUEST_ABI*)(const char*, int, unsigned);
using Openat = int(WINAPK_GUEST_ABI*)(int, const char*, int, unsigned);

// An arm64 library's open/openat imports bind here (abi_override): the same calls with x86_64's flags.
// /proc/self/auxv is the vector arm64 code is given (its CPU, not the host's).
WINAPK_GUEST_ABI int a64_open(const char* path, int flags, unsigned mode) {
    if (is_proc_auxv_path(path)) return open_proc_auxv(true);
    static const auto fn = shim<Open>("open");
    return fn(path, x86_open_flags(flags), mode);
}
WINAPK_GUEST_ABI int a64_open_2(const char* path, int flags) { return a64_open(path, flags, 0); }
WINAPK_GUEST_ABI int a64_openat(int dirfd, const char* path, int flags, unsigned mode) {
    if (is_proc_auxv_path(path)) return open_proc_auxv(true);  // an absolute path: dirfd does not count
    static const auto fn = shim<Openat>("openat");
    return fn(dirfd, path, x86_open_flags(flags), mode);
}

// fopen of /proc/self/auxv likewise; every other path is the shim's.
using Fopen = void*(WINAPK_GUEST_ABI*)(const char*, const char*);
WINAPK_GUEST_ABI void* a64_fopen(const char* path, const char* mode) {
    if (is_proc_auxv_path(path)) {
        const int fd = open_proc_auxv(true);
        if (fd < 0) return nullptr;
        static const auto fdopen_fn = shim<void*(WINAPK_GUEST_ABI*)(int, const char*)>("fdopen");
        void* f = fdopen_fn(fd, "r");
        if (!f) shim<int(WINAPK_GUEST_ABI*)(int)>("close")(fd);
        return f;
    }
    static const auto fn = shim<Fopen>("fopen");
    return fn(path, mode);
}

// getauxval: the vector arm64 code is given (winapk::process_auxv(true)): AT_HWCAP and AT_HWCAP2 the
// features it is told of (kHwcap, kHwcap2), AT_PLATFORM "aarch64"; 0 and ENOENT for a type it lacks.
WINAPK_GUEST_ABI uint64_t a64_getauxval(uint64_t type) {
    static const std::string auxv = process_auxv(true);
    uint64_t v = 0;
    if (auxv_value(auxv, type, &v)) return v;
    *shim<int*(WINAPK_GUEST_ABI*)()>("__errno")() = 2;  // ENOENT
    return 0;
}
WINAPK_GUEST_ABI int a64_openat_2(int dirfd, const char* path, int flags) { return a64_openat(dirfd, path, flags, 0); }

// mremap: the shim's, and an executable range the bridge runs as guest code follows it to where it moved.
using Mremap = void*(WINAPK_GUEST_ABI*)(void*, size_t, size_t, int, void*);
WINAPK_GUEST_ABI void* a64_mremap(void* old_addr, size_t old_len, size_t new_len, int flags, void* new_addr) {
    static const auto fn = shim<Mremap>("mremap");
    void* p = fn(old_addr, old_len, new_len, flags, new_addr);
    if (p == reinterpret_cast<void*>(-1)) return p;
    bool was_exec;
    {
        std::lock_guard<std::mutex> hold(g_exec_lock);
        was_exec = g_exec_ranges.count(reinterpret_cast<uint64_t>(old_addr) & ~(kPage - 1)) != 0;
    }
    if (was_exec && p != old_addr) {
        if (!(flags & 4)) track_exec(old_addr, old_len, 0);  // moved away (not MREMAP_DONTUNMAP)
        track_exec(p, new_len, kProtExec);
    } else if (was_exec && new_len != old_len) {
        track_exec(old_addr, old_len, 0);
        track_exec(p, new_len, kProtExec);
    }
    return p;
}

// getcpu(cpu, node, cache): the processor the thread runs on now, its NUMA node; the cache argument is
// unused since Linux 2.6.24.
int64_t a64_getcpu(uint32_t* cpu, uint32_t* node) {
    PROCESSOR_NUMBER pn{};
    GetCurrentProcessorNumberEx(&pn);
    if (cpu) *cpu = static_cast<uint32_t>(pn.Group) * 64 + pn.Number;
    if (node) {
        USHORT n = 0;
        *node = GetNumaProcessorNodeEx(&pn, &n) ? n : 0;
    }
    return 0;
}

// libc's convention: -1 and errno on failure.
int64_t do_syscall(uint64_t nr, const uint64_t a[6]) {
    static const auto shim_syscall = shim<Syscall>("syscall");
    auto via = [&](int64_t x86) {
        return shim_syscall(x86, static_cast<int64_t>(a[0]), static_cast<int64_t>(a[1]), static_cast<int64_t>(a[2]),
                            static_cast<int64_t>(a[3]), static_cast<int64_t>(a[4]));
    };
    auto fn3 = [&](const char* name) {
        const auto f = shim<Fn3>(name);
        return f(static_cast<int64_t>(a[0]), static_cast<int64_t>(a[1]), static_cast<int64_t>(a[2]));
    };
    switch (nr) {
        case 98: return via(202);   // futex
        case 178: return via(186);  // gettid
        case 172: return via(39);   // getpid
        case 124: return via(24);   // sched_yield
        case 123: return via(204);  // sched_getaffinity
        case 278: return via(318);  // getrandom
        case 113: return via(228);  // clock_gettime
        case 233: return via(28);   // madvise
        case 283: return via(324);  // membarrier
        case 96: return set_tid_address(reinterpret_cast<uint32_t*>(a[0]));  // cleared and woken at thread end
        case 134:  // rt_sigaction(sig, act, old, sigsetsize): the kernel's struct sigaction
            return raw_rt_sigaction(static_cast<int>(a[0]), reinterpret_cast<const void*>(a[1]),
                                    reinterpret_cast<void*>(a[2]), static_cast<size_t>(a[3]));
        case 135:  // rt_sigprocmask(how, set, old, sigsetsize)
            return raw_rt_sigprocmask(static_cast<int>(a[0]), reinterpret_cast<const uint64_t*>(a[1]),
                                      reinterpret_cast<uint64_t*>(a[2]), static_cast<size_t>(a[3]));
        case 17: {  // getcwd: the kernel returns the length, with the terminating NUL; libc returns the buffer
            const auto f = shim<char*(WINAPK_GUEST_ABI*)(char*, size_t)>("getcwd");
            char* buf = reinterpret_cast<char*>(a[0]);
            if (!buf) return *shim<int*(WINAPK_GUEST_ABI*)()>("__errno")() = 14, -1;  // EFAULT (libc would allocate)
            return f(buf, static_cast<size_t>(a[1])) ? static_cast<int64_t>(std::strlen(buf) + 1) : -1;
        }
        case 115: {  // clock_nanosleep returns its error number (POSIX), not -1 and errno
            const auto f = shim<int(WINAPK_GUEST_ABI*)(int, int, const void*, void*)>("clock_nanosleep");
            const int err = f(static_cast<int>(a[0]), static_cast<int>(a[1]), reinterpret_cast<const void*>(a[2]),
                              reinterpret_cast<void*>(a[3]));
            if (err == 0) return 0;
            *shim<int*(WINAPK_GUEST_ABI*)()>("__errno")() = err;
            return -1;
        }
        case 141: {  // getpriority: the kernel returns 20 - nice (1..40), so that no answer looks like an error
            const auto f = shim<int(WINAPK_GUEST_ABI*)(int, int)>("getpriority");
            int* err = shim<int*(WINAPK_GUEST_ABI*)()>("__errno")();
            const int saved = *err;
            *err = 0;
            const int nice = f(static_cast<int>(a[0]), static_cast<int>(a[1]));
            if (nice == -1 && *err != 0) return -1;
            *err = saved;
            return 20 - nice;
        }
        case 168: return a64_getcpu(reinterpret_cast<uint32_t*>(a[0]), reinterpret_cast<uint32_t*>(a[1]));
        case 21:
            return a64_epoll_ctl(static_cast<int>(a[0]), static_cast<int>(a[1]), static_cast<int>(a[2]),
                                 reinterpret_cast<Arm64EpollEvent*>(a[3]));
        case 22:  // epoll_pwait(epfd, events, max, timeout, sigmask, sigsetsize)
            return a64_epoll_pwait(static_cast<int>(a[0]), reinterpret_cast<Arm64EpollEvent*>(a[1]),
                                   static_cast<int>(a[2]), static_cast<int>(a[3]), reinterpret_cast<const void*>(a[4]));
        case 48: {  // faccessat
            const auto f = shim<int(WINAPK_GUEST_ABI*)(int, const char*, int, int)>("faccessat");
            return f(static_cast<int>(a[0]), reinterpret_cast<const char*>(a[1]), static_cast<int>(a[2]),
                     static_cast<int>(a[3]));
        }
        case 129: {  // kill
            const auto f = shim<int(WINAPK_GUEST_ABI*)(int, int)>("kill");
            return f(static_cast<int>(a[0]), static_cast<int>(a[1]));
        }
        // tkill(tid, sig), tgkill(tgid, tid, sig): the threads are this
        // process's own, and no signal is delivered on Windows (sigaction
        // records handlers, it does not install them); signal 0, the check
        // that a thread is alive, is the common use and is answered yes.
        case 130: case 131: return 0;
        case 56:  // openat(dirfd, path, flags, mode): relative to a directory descriptor too
            return a64_openat(static_cast<int>(a[0]), reinterpret_cast<const char*>(a[1]), static_cast<int>(a[2]),
                              static_cast<unsigned>(a[3]));
        case 57: return shim<int(WINAPK_GUEST_ABI*)(int)>("close")(static_cast<int>(a[0]));
        case 62: return fn3("lseek");
        case 63: return fn3("read");
        case 64: return fn3("write");
        case 79:
            return a64_fstatat(static_cast<int>(a[0]), reinterpret_cast<const char*>(a[1]),
                               reinterpret_cast<Arm64Stat*>(a[2]), static_cast<int>(a[3]));
        case 80: return a64_fstat(static_cast<int>(a[0]), reinterpret_cast<Arm64Stat*>(a[1]));
        case 93: case 94:
            shim<void(WINAPK_GUEST_ABI*)(int64_t)>("_exit")(static_cast<int64_t>(a[0]));
            return 0;
        // Memory, through the wrappers the mmap/mprotect/munmap imports bind to, so that an executable
        // mapping made by raw call is registered as guest code too.
        case 222:
            return reinterpret_cast<int64_t>(a64_mmap(reinterpret_cast<void*>(a[0]), static_cast<size_t>(a[1]),
                                                      static_cast<int>(a[2]), static_cast<int>(a[3]),
                                                      static_cast<int>(a[4]), static_cast<int64_t>(a[5])));
        case 215: return a64_munmap(reinterpret_cast<void*>(a[0]), static_cast<size_t>(a[1]));
        case 226:
            return a64_mprotect(reinterpret_cast<void*>(a[0]), static_cast<size_t>(a[1]), static_cast<int>(a[2]));
        case 216:  // mremap(old, old_len, new_len, flags, new)
            return reinterpret_cast<int64_t>(a64_mremap(reinterpret_cast<void*>(a[0]), static_cast<size_t>(a[1]),
                                                        static_cast<size_t>(a[2]), static_cast<int>(a[3]),
                                                        reinterpret_cast<void*>(a[4])));
        default:
            if (const LibcCall* c = libc_call(nr)) {
                const int64_t r = shim<Syscall>(c->name)(static_cast<int64_t>(a[0]), static_cast<int64_t>(a[1]),
                                                         static_cast<int64_t>(a[2]), static_cast<int64_t>(a[3]),
                                                         static_cast<int64_t>(a[4]), static_cast<int64_t>(a[5]));
                return c->wide ? r : static_cast<int32_t>(r);
            }
            *shim<int*(WINAPK_GUEST_ABI*)()>("__errno")() = 38;  // ENOSYS (known_syscall said yes: not reached)
            return -1;
    }
}

// a5 (mmap's offset) is the guest's x6, which the bridge passes on as the first stack argument; a caller
// with fewer arguments leaves whatever x6 held, which only the calls that take six read.
WINAPK_GUEST_ABI int64_t a64_syscall(int64_t nr, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                                     int64_t a5) {
    const uint64_t a[6] = {static_cast<uint64_t>(a0), static_cast<uint64_t>(a1), static_cast<uint64_t>(a2),
                           static_cast<uint64_t>(a3), static_cast<uint64_t>(a4), static_cast<uint64_t>(a5)};
    if (!known_syscall(static_cast<uint64_t>(nr))) {
        *shim<int*(WINAPK_GUEST_ABI*)()>("__errno")() = 38;  // ENOSYS
        return -1;
    }
    return do_syscall(static_cast<uint64_t>(nr), a);
}

}  // namespace

bool linux_syscall(uint64_t nr, const uint64_t args[6], uint64_t* result) {
    if (!known_syscall(nr)) return false;
    const int64_t r = do_syscall(nr, args);
    // The kernel's convention: -errno on failure.
    *result = static_cast<uint64_t>(r == -1 ? -static_cast<int64_t>(*shim<int*(WINAPK_GUEST_ABI*)()>("__errno")()) : r);
    return true;
}

void* abi_override(const char* name) {
    static const struct {
        const char* name;
        void* fn;
    } kOverrides[] = {
        {"stat", reinterpret_cast<void*>(&a64_stat)},
        {"stat64", reinterpret_cast<void*>(&a64_stat)},
        {"lstat", reinterpret_cast<void*>(&a64_lstat)},
        {"lstat64", reinterpret_cast<void*>(&a64_lstat)},
        {"fstat", reinterpret_cast<void*>(&a64_fstat)},
        {"fstat64", reinterpret_cast<void*>(&a64_fstat)},
        {"fstatat", reinterpret_cast<void*>(&a64_fstatat)},
        {"fstatat64", reinterpret_cast<void*>(&a64_fstatat)},
        {"fts_open", reinterpret_cast<void*>(&a64_fts_open)},
        {"epoll_ctl", reinterpret_cast<void*>(&a64_epoll_ctl)},
        {"epoll_wait", reinterpret_cast<void*>(&a64_epoll_wait)},
        {"epoll_pwait", reinterpret_cast<void*>(&a64_epoll_pwait)},
        {"syscall", reinterpret_cast<void*>(&a64_syscall)},
        {"mmap", reinterpret_cast<void*>(&a64_mmap)},
        {"mmap64", reinterpret_cast<void*>(&a64_mmap)},
        {"mprotect", reinterpret_cast<void*>(&a64_mprotect)},
        {"munmap", reinterpret_cast<void*>(&a64_munmap)},
        {"mremap", reinterpret_cast<void*>(&a64_mremap)},
        {"open", reinterpret_cast<void*>(&a64_open)},
        {"open64", reinterpret_cast<void*>(&a64_open)},
        {"__open_2", reinterpret_cast<void*>(&a64_open_2)},
        {"openat", reinterpret_cast<void*>(&a64_openat)},
        {"openat64", reinterpret_cast<void*>(&a64_openat)},
        {"__openat_2", reinterpret_cast<void*>(&a64_openat_2)},
        {"fopen", reinterpret_cast<void*>(&a64_fopen)},
        {"fopen64", reinterpret_cast<void*>(&a64_fopen)},
        {"getauxval", reinterpret_cast<void*>(&a64_getauxval)},
    };
    for (const auto& o : kOverrides) {
        if (std::strcmp(o.name, name) == 0) return mini_bionic_resolve(name) ? o.fn : nullptr;
    }
    return nullptr;
}

}  // namespace winapk::arm64
