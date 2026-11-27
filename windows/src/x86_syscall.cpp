// x86_syscall.cpp -- raw Linux system calls in x86_64 guest code (x86_syscall.h).

#include "winapk/x86_syscall.h"

#include "winapk/arm64_guest.h"
#include "winapk/guest_abi.h"
#include "winapk/mini_bionic.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_set>
#include <vector>

namespace winapk::x86 {
namespace {

constexpr uintptr_t kPage = 4096;
constexpr int kProtRead = 1, kProtWrite = 2, kProtExec = 4;
constexpr int kENOSYS = 38, kEFAULT = 14;

// ---- the calls ----------------------------------------------------------------------------
//
// x86_64's numbers (arch/x86/entry/syscalls/syscall_64.tbl) to the shim's
// functions. Where the libc function and the system call take the same
// arguments and differ only in how they report an error, the function is
// called by name; the rest are below in other_syscall. A function that returns
// ssize_t or off_t is "wide"; the others return an int, whose -1 must not
// reach the caller as 0xffffffff.

using Syscall = int64_t(WINAPK_GUEST_ABI*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

struct LibcCall {
    uint16_t nr;
    const char* name;
    bool wide;
};

constexpr LibcCall kLibcCalls[] = {
    {0, "read", true},            {1, "write", true},            {2, "open", false},
    {3, "close", false},          {4, "stat", false},            {5, "fstat", false},
    {6, "lstat", false},          {7, "poll", false},            {8, "lseek", true},
    {16, "ioctl", false},         {17, "pread64", true},         {18, "pwrite64", true},
    {19, "readv", true},          {20, "writev", true},          {21, "access", false},
    {22, "pipe", false},          {23, "select", false},         {25, "mremap", true},
    {26, "msync", false},         {27, "mincore", false},        {32, "dup", false},
    {33, "dup2", false},          {34, "pause", false},          {35, "nanosleep", false},
    {37, "alarm", false},         {40, "sendfile", true},        {41, "socket", false},
    {42, "connect", false},       {43, "accept", false},         {44, "sendto", true},
    {45, "recvfrom", true},       {46, "sendmsg", true},         {47, "recvmsg", true},
    {48, "shutdown", false},      {49, "bind", false},           {50, "listen", false},
    {51, "getsockname", false},   {52, "getpeername", false},    {53, "socketpair", false},
    {54, "setsockopt", false},    {55, "getsockopt", false},     {57, "fork", false},
    {58, "vfork", false},         {59, "execve", false},         {61, "wait4", false},
    {62, "kill", false},          {63, "uname", false},          {72, "fcntl", false},
    {73, "flock", false},         {74, "fsync", false},          {75, "fdatasync", false},
    {76, "truncate", false},      {77, "ftruncate", false},      {80, "chdir", false},
    {82, "rename", false},        {83, "mkdir", false},          {84, "rmdir", false},
    {85, "creat", false},         {86, "link", false},           {87, "unlink", false},
    {88, "symlink", false},       {89, "readlink", true},        {90, "chmod", false},
    {91, "fchmod", false},        {92, "chown", false},          {93, "fchown", false},
    {94, "lchown", false},        {95, "umask", false},          {96, "gettimeofday", false},
    {97, "getrlimit", false},     {98, "getrusage", false},      {99, "sysinfo", false},
    {100, "times", true},         {101, "ptrace", true},         {102, "getuid", false},
    {104, "getgid", false},       {107, "geteuid", false},       {108, "getegid", false},
    {109, "setpgid", false},      {110, "getppid", false},       {112, "setsid", false},
    {131, "sigaltstack", false},  {137, "statfs", false},        {138, "fstatfs", false},
    {141, "setpriority", false},  {149, "mlock", false},         {150, "munlock", false},
    {151, "mlockall", false},     {152, "munlockall", false},    {157, "prctl", false},
    {160, "setrlimit", false},    {188, "setxattr", false},      {189, "lsetxattr", false},
    {190, "fsetxattr", false},    {191, "getxattr", true},       {192, "lgetxattr", true},
    {193, "fgetxattr", true},     {194, "listxattr", true},      {195, "llistxattr", true},
    {196, "flistxattr", true},    {197, "removexattr", false},   {198, "lremovexattr", false},
    {199, "fremovexattr", false}, {203, "sched_setaffinity", false}, {213, "epoll_create", false},
    {229, "clock_getres", false}, {232, "epoll_wait", false},    {233, "epoll_ctl", false},
    {253, "inotify_add_watch", false}, {254, "inotify_rm_watch", false}, {257, "openat", false},
    {258, "mkdirat", false},      {260, "fchownat", false},      {262, "fstatat", false},
    {263, "unlinkat", false},     {264, "renameat", false},      {265, "linkat", false},
    {266, "symlinkat", false},    {267, "readlinkat", true},     {268, "fchmodat", false},
    {269, "faccessat", false},    {271, "ppoll", false},         {280, "utimensat", false},
    {281, "epoll_pwait", false},  {283, "timerfd_create", false},
    {285, "fallocate", false},    {286, "timerfd_settime", false}, {287, "timerfd_gettime", false},
    {288, "accept4", false},      {290, "eventfd", false},       {291, "epoll_create1", false},
    {292, "dup3", false},         {293, "pipe2", false},         {294, "inotify_init1", false},
    {299, "recvmmsg", false},     {302, "prlimit", false},       {307, "sendmmsg", false},
    {319, "memfd_create", false}, {325, "mlock2", false},
};

// The numbers the shim's own syscall() answers before it asks other_syscall (misc.cpp).
constexpr uint16_t kShimSyscalls[] = {9, 10, 11, 13, 14, 24, 28, 39, 186, 200, 202, 204, 218, 228, 234, 318, 324};

template <typename F>
F shim(const char* name) {
    return reinterpret_cast<F>(mini_bionic_resolve(name));
}

int* guest_errno() {
    static const auto fn = shim<int*(WINAPK_GUEST_ABI*)()>("__errno");
    return fn();
}

const LibcCall* libc_call(int64_t nr) {
    for (const auto& c : kLibcCalls) {
        if (c.nr == nr) return mini_bionic_resolve(c.name) ? &c : nullptr;
    }
    return nullptr;
}

bool special(int64_t nr) {
    return nr == 60 || nr == 79 || nr == 140 || nr == 230 || nr == 231 || nr == 284 || nr == 309;
}

bool known(int64_t nr) {
    for (uint16_t n : kShimSyscalls) {
        if (n == nr) return true;
    }
    return special(nr) || libc_call(nr) != nullptr;
}

}  // namespace

int64_t other_syscall(int64_t nr, const int64_t a[6]) {
    switch (nr) {
        case 60:  // exit: this thread only; exit_group (231) is the process
            shim<void(WINAPK_GUEST_ABI*)(void*)>("pthread_exit")(nullptr);
            return 0;
        case 231:
            shim<void(WINAPK_GUEST_ABI*)(int64_t)>("_exit")(a[0]);
            return 0;
        case 79: {  // getcwd: the kernel returns the length with the terminating NUL; libc returns the buffer
            char* buf = reinterpret_cast<char*>(a[0]);
            if (!buf) return *guest_errno() = kEFAULT, -1;  // libc would allocate; the kernel cannot
            const auto f = shim<char*(WINAPK_GUEST_ABI*)(char*, size_t)>("getcwd");
            return f(buf, static_cast<size_t>(a[1])) ? static_cast<int64_t>(std::strlen(buf) + 1) : -1;
        }
        case 140: {  // getpriority: the kernel returns 20 - nice (1..40), so that no answer looks like an error
            const auto f = shim<int(WINAPK_GUEST_ABI*)(int, int)>("getpriority");
            int* err = guest_errno();
            const int saved = *err;
            *err = 0;
            const int nice = f(static_cast<int>(a[0]), static_cast<int>(a[1]));
            if (nice == -1 && *err != 0) return -1;
            *err = saved;
            return 20 - nice;
        }
        case 230: {  // clock_nanosleep returns its error number (POSIX), not -1 and errno
            const auto f = shim<int(WINAPK_GUEST_ABI*)(int, int, const void*, void*)>("clock_nanosleep");
            const int err = f(static_cast<int>(a[0]), static_cast<int>(a[1]), reinterpret_cast<const void*>(a[2]),
                              reinterpret_cast<void*>(a[3]));
            if (err == 0) return 0;
            *guest_errno() = err;
            return -1;
        }
        case 284: {  // eventfd(initval): no flags argument (eventfd2, 290, has one); rsi is whatever it held
            const auto f = shim<int(WINAPK_GUEST_ABI*)(unsigned, int)>("eventfd");
            return f(static_cast<unsigned>(a[0]), 0);
        }
        case 309: {  // getcpu(cpu, node, cache)
            PROCESSOR_NUMBER pn{};
            GetCurrentProcessorNumberEx(&pn);
            if (auto* cpu = reinterpret_cast<uint32_t*>(a[0])) *cpu = static_cast<uint32_t>(pn.Group) * 64 + pn.Number;
            if (auto* node = reinterpret_cast<uint32_t*>(a[1])) {
                USHORT n = 0;
                *node = GetNumaProcessorNodeEx(&pn, &n) ? n : 0;
            }
            return 0;
        }
        default:
            break;
    }
    if (const LibcCall* c = libc_call(nr)) {
        const int64_t r = shim<Syscall>(c->name)(a[0], a[1], a[2], a[3], a[4], a[5]);
        return c->wide ? r : static_cast<int32_t>(r);
    }
    *guest_errno() = kENOSYS;
    return -1;
}

namespace {

std::atomic<size_t> g_sites_patched{0};
std::atomic<size_t> g_calls_answered{0};

// One line per number the shim does not have, the first time guest code makes it.
void report_unknown(uint64_t nr, uint64_t rip) {
    static std::mutex lock;
    static std::set<uint64_t> seen;
    std::lock_guard<std::mutex> hold(lock);
    if (!seen.insert(nr).second) return;
    std::fprintf(stderr, "[win.apk] raw system call %llu (x86_64) at 0x%llx: not implemented, -ENOSYS\n",
                 static_cast<unsigned long long>(nr), static_cast<unsigned long long>(rip));
    std::fflush(stderr);
}

}  // namespace

bool linux_syscall(uint64_t nr, const uint64_t args[6], uint64_t* result) {
    const auto n = static_cast<int64_t>(nr);
    if (!known(n)) return false;
    const int64_t a[6] = {static_cast<int64_t>(args[0]), static_cast<int64_t>(args[1]), static_cast<int64_t>(args[2]),
                          static_cast<int64_t>(args[3]), static_cast<int64_t>(args[4]), static_cast<int64_t>(args[5])};
    using ShimSyscall = int64_t(WINAPK_GUEST_ABI*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
    static const auto shim_syscall = shim<ShimSyscall>("syscall");
    // The kernel never touches errno, which is libc's: the caller's value is kept.
    int* err = guest_errno();
    const int saved = *err;
    *err = 0;
    const int64_t r = shim_syscall(n, a[0], a[1], a[2], a[3], a[4], a[5]);
    // The kernel's convention: -errno on failure. mmap's MAP_FAILED is -1 too.
    *result = static_cast<uint64_t>(r == -1 && *err != 0 ? -static_cast<int64_t>(*err) : r);
    *err = saved;
    return true;
}

// ---- decoding --------------------------------------------------------------------------------
//
// Enough of the x86_64 encoding to know each instruction's length and where it
// can go next: legacy prefixes, REX, the one-, two- and three-byte opcode maps,
// VEX and EVEX, ModRM, SIB, displacements and immediates (Intel SDM vol. 2,
// appendix A). An opcode that is invalid in 64-bit mode ends the path: what
// follows is not code this decoder can vouch for.

namespace {

enum class Flow { kNext, kStop, kJump, kBranch, kCall };

struct Insn {
    size_t len = 0;
    Flow flow = Flow::kNext;
    int64_t rel = 0;  // for kJump, kBranch, kCall: target = end of the instruction + rel
    bool syscall = false;
};

bool decode(const uint8_t* p, size_t avail, Insn* d) {
    const size_t limit = std::min<size_t>(avail, 15);
    size_t i = 0;
    bool opsize16 = false, addr32 = false, rexw = false;
    for (;; ++i) {
        if (i >= limit) return false;
        const uint8_t b = p[i];
        if (b == 0x66) opsize16 = true;
        else if (b == 0x67) addr32 = true;
        else if (b == 0xf0 || b == 0xf2 || b == 0xf3 || b == 0x2e || b == 0x36 || b == 0x3e || b == 0x26 ||
                 b == 0x64 || b == 0x65) {
        } else if ((b & 0xf0) == 0x40) {
            // REX counts only right before the opcode; one before another prefix is ignored.
            if (i + 1 < limit && ((p[i + 1] & 0xf0) != 0x40)) {
                const uint8_t n = p[i + 1];
                const bool prefix = n == 0x66 || n == 0x67 || n == 0xf0 || n == 0xf2 || n == 0xf3 || n == 0x2e ||
                                    n == 0x36 || n == 0x3e || n == 0x26 || n == 0x64 || n == 0x65;
                if (!prefix) {
                    rexw = b & 8;
                    ++i;
                    break;
                }
            }
        } else {
            break;
        }
    }
    if (i >= limit) return false;
    uint8_t op = p[i++];
    int map = 0;
    bool modrm = false, vex = false;
    size_t imm = 0;
    const size_t z = opsize16 ? 2 : 4;
    if (op == 0xc4 || op == 0xc5 || op == 0x62) {
        // VEX (c4: three bytes, c5: two) and EVEX (62: four), all valid only in 64-bit mode here.
        vex = true;
        if (op == 0xc5) {
            if (i + 1 > limit) return false;
            map = 1;
            i += 1;
        } else if (op == 0xc4) {
            if (i + 2 > limit) return false;
            map = p[i] & 0x1f;
            i += 2;
        } else {
            if (i + 3 > limit) return false;
            map = p[i] & 7;
            i += 3;
        }
        if (map < 1 || map > 7 || map == 4) return false;
        if (i >= limit) return false;
        op = p[i++];
        modrm = !(map == 1 && op == 0x77);  // vzeroupper / vzeroall
        if (map == 3) imm = 1;
        if (map == 1 && ((op >= 0x70 && op <= 0x73) || op == 0xc2 || op == 0xc4 || op == 0xc5 || op == 0xc6)) imm = 1;
    } else if (op == 0x0f) {
        if (i >= limit) return false;
        op = p[i++];
        map = 1;
        if (op == 0x38 || op == 0x3a) {
            map = op == 0x38 ? 2 : 3;
            if (i >= limit) return false;
            op = p[i++];
        }
    }

    d->flow = Flow::kNext;
    d->syscall = false;
    size_t rel = 0;
    if (vex) {
        // set above
    } else if (map == 0) {
        switch (op) {
            case 0x06: case 0x07: case 0x0e: case 0x16: case 0x17: case 0x1e: case 0x1f: case 0x27: case 0x2f:
            case 0x37: case 0x3f: case 0x60: case 0x61: case 0x82: case 0x9a: case 0xce: case 0xd4: case 0xd5:
            case 0xd6: case 0xea:
                return false;
            default: break;
        }
        if (op < 0x40) {
            const int low = op & 7;
            if (low < 4) modrm = true;
            else if (low == 4) imm = 1;
            else if (low == 5) imm = z;
        } else if (op >= 0x50 && op <= 0x5f) {
        } else if (op == 0x63) {
            modrm = true;
        } else if (op == 0x68) {
            imm = z;
        } else if (op == 0x69) {
            modrm = true, imm = z;
        } else if (op == 0x6a) {
            imm = 1;
        } else if (op == 0x6b) {
            modrm = true, imm = 1;
        } else if (op >= 0x6c && op <= 0x6f) {
        } else if (op >= 0x70 && op <= 0x7f) {
            rel = 1, d->flow = Flow::kBranch;
        } else if (op == 0x80 || op == 0x83 || op == 0xc0 || op == 0xc1 || op == 0xc6) {
            modrm = true, imm = 1;
        } else if (op == 0x81 || op == 0xc7) {
            modrm = true, imm = z;
        } else if (op >= 0x84 && op <= 0x8f) {
            modrm = true;
        } else if (op >= 0x90 && op <= 0x9f) {
        } else if (op >= 0xa0 && op <= 0xa3) {
            imm = addr32 ? 4 : 8;  // moffs
        } else if (op == 0xa8) {
            imm = 1;
        } else if (op == 0xa9) {
            imm = z;
        } else if (op >= 0xa4 && op <= 0xaf) {
        } else if (op >= 0xb0 && op <= 0xb7) {
            imm = 1;
        } else if (op >= 0xb8 && op <= 0xbf) {
            imm = rexw ? 8 : z;
        } else if (op == 0xc2 || op == 0xca) {
            imm = 2, d->flow = Flow::kStop;
        } else if (op == 0xc3 || op == 0xcb || op == 0xcc || op == 0xcf || op == 0xf4) {
            d->flow = Flow::kStop;
        } else if (op == 0xc8) {
            imm = 3;
        } else if (op == 0xcd || (op >= 0xe4 && op <= 0xe7)) {
            imm = 1;
        } else if (op == 0xc9 || op == 0xd7 || (op >= 0xec && op <= 0xef) || op == 0xf1 || op == 0xf5 ||
                   (op >= 0xf8 && op <= 0xfd)) {
        } else if ((op >= 0xd0 && op <= 0xd3) || (op >= 0xd8 && op <= 0xdf) || op == 0xfe || op == 0xff ||
                   op == 0xf6 || op == 0xf7) {
            modrm = true;
        } else if (op >= 0xe0 && op <= 0xe3) {
            rel = 1, d->flow = Flow::kBranch;
        } else if (op == 0xe8) {
            rel = 4, d->flow = Flow::kCall;
        } else if (op == 0xe9) {
            rel = 4, d->flow = Flow::kJump;
        } else if (op == 0xeb) {
            rel = 1, d->flow = Flow::kJump;
        } else {
            return false;
        }
    } else if (map == 1) {
        if ((op >= 0x24 && op <= 0x27) || op == 0x36 || op == 0x39 || (op >= 0x3b && op <= 0x3f) || op == 0xa6 ||
            op == 0xa7 || op == 0x04 || op == 0x0a || op == 0x0c) {
            return false;
        }
        if (op == 0x05) {
            d->syscall = true;
        } else if (op == 0x0b) {
            d->flow = Flow::kStop;  // ud2
        } else if (op == 0x0f) {
            modrm = true, imm = 1;  // 3DNow!, its opcode after the operand
        } else if ((op >= 0x06 && op <= 0x09) || op == 0x0e || (op >= 0x30 && op <= 0x37) || op == 0x77 ||
                   (op >= 0xa0 && op <= 0xa2) || (op >= 0xa8 && op <= 0xaa) || (op >= 0xc8 && op <= 0xcf)) {
        } else if ((op >= 0x70 && op <= 0x73) || op == 0xa4 || op == 0xac || op == 0xba || op == 0xc2 ||
                   (op >= 0xc4 && op <= 0xc6)) {
            modrm = true, imm = 1;
        } else if (op >= 0x80 && op <= 0x8f) {
            rel = 4, d->flow = Flow::kBranch;
        } else {
            modrm = true;
        }
    } else if (map == 2) {
        modrm = true;
    } else {
        modrm = true, imm = 1;
    }

    if (modrm) {
        if (i >= limit) return false;
        const uint8_t m = p[i++];
        const int mod = m >> 6, rm = m & 7, reg = (m >> 3) & 7;
        if (mod != 3) {
            size_t disp = mod == 1 ? 1 : mod == 2 ? 4 : 0;
            if (rm == 4) {
                if (i >= limit) return false;
                const uint8_t sib = p[i++];
                if ((sib & 7) == 5 && mod == 0) disp = 4;
            } else if (rm == 5 && mod == 0) {
                disp = 4;  // rip-relative
            }
            i += disp;
        }
        if (!vex && map == 0) {
            if (op == 0xf6 && reg < 2) imm = 1;
            if (op == 0xf7 && reg < 2) imm = z;
            if (op == 0xff && (reg == 4 || reg == 5)) d->flow = Flow::kStop;  // jmp through a register or memory
        }
    }
    i += imm;
    if (rel) {
        if (i + rel > limit) return false;
        if (rel == 1) {
            d->rel = static_cast<int8_t>(p[i]);
        } else {
            int32_t v;
            std::memcpy(&v, p + i, 4);
            d->rel = v;
        }
        i += rel;
    }
    if (i > limit) return false;
    d->len = i;
    return true;
}

// Every instruction reachable from the entries through fall-through and direct
// jumps, calls and branches, inside the ranges. Collects the SYSCALL sites and
// the pages decoded. Which addresses have been decoded is one bit per byte of
// the span the ranges cover (an eighth of the code's size): each byte starts
// at most one decode, so the walk ends without a budget, in time linear in the
// code.
class Walk {
public:
    Walk(const CodeRange* ranges, size_t n) : ranges_(ranges, ranges + n) {
        if (ranges_.empty()) return;
        base_ = ranges_[0].lo;
        uintptr_t top = ranges_[0].hi;
        for (const CodeRange& r : ranges_) {
            base_ = std::min(base_, r.lo);
            top = std::max(top, r.hi);
        }
        seen_.assign((top - base_ + 63) / 64, 0);
        pages_.assign((top - (base_ & ~(kPage - 1)) + kPage - 1) / kPage, false);
    }

    void from(uintptr_t entry) {
        work_.push_back(entry);
        while (!work_.empty()) {
            uintptr_t pc = work_.back();
            work_.pop_back();
            for (;;) {
                const CodeRange* r = range_of(pc);
                if (!r || !first_visit(pc)) break;
                Insn in;
                if (!decode(reinterpret_cast<const uint8_t*>(pc), r->hi - pc, &in)) break;
                ++instructions_;
                pages_[page_index(pc)] = true;
                pages_[page_index(pc + in.len - 1)] = true;
                if (in.syscall) sites_.push_back(pc);
                const uintptr_t next = pc + in.len;
                const uintptr_t target = next + static_cast<uintptr_t>(in.rel);
                if (in.flow == Flow::kStop) break;
                if (in.flow == Flow::kJump) {
                    pc = target;
                    continue;
                }
                if (in.flow == Flow::kBranch || in.flow == Flow::kCall) work_.push_back(target);
                pc = next;
            }
        }
    }

    const std::vector<uintptr_t>& sites() const { return sites_; }
    size_t instructions() const { return instructions_; }
    // The pages that hold a decoded byte.
    std::vector<uintptr_t> pages() const {
        std::vector<uintptr_t> out;
        for (size_t i = 0; i < pages_.size(); ++i) {
            if (pages_[i]) out.push_back((base_ & ~(kPage - 1)) + i * kPage);
        }
        return out;
    }

private:
    const CodeRange* range_of(uintptr_t pc) const {
        for (const CodeRange& r : ranges_) {
            if (pc >= r.lo && pc < r.hi) return &r;
        }
        return nullptr;
    }
    // pc lies in a range, so in the bitmap.
    bool first_visit(uintptr_t pc) {
        const uintptr_t i = pc - base_;
        uint64_t& word = seen_[i / 64];
        const uint64_t bit = uint64_t{1} << (i % 64);
        if (word & bit) return false;
        word |= bit;
        return true;
    }
    // An instruction's last byte lies in its range too (decode reads no further).
    size_t page_index(uintptr_t a) const { return (a - (base_ & ~(kPage - 1))) / kPage; }

    std::vector<CodeRange> ranges_;
    uintptr_t base_ = 0;
    std::vector<uint64_t> seen_;
    std::vector<bool> pages_;
    std::vector<uintptr_t> work_;
    std::vector<uintptr_t> sites_;
    size_t instructions_ = 0;
};

// ---- the sites and the deferred pages ----------------------------------------------------

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_set<uintptr_t> g_sites;   // SYSCALLs made UD2, under g_lock
std::map<uintptr_t, int> g_pending;      // page -> the PROT_* asked for, executable once run, under g_lock
std::atomic<bool> g_active{false};       // x86_64 guest code has been loaded

DWORD win_exec_protect(int prot) {
    return (prot & kProtWrite) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
}

bool executable(DWORD protect) {
    return protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
}

// Under g_lock (exclusive). `writable`: the bytes may be written as they are.
size_t patch_sites(const std::vector<uintptr_t>& sites, bool writable) {
    size_t n = 0;
    for (uintptr_t s : sites) {
        auto* b = reinterpret_cast<uint8_t*>(s);
        if (b[0] != 0x0f || b[1] != 0x05) continue;
        if (writable) {
            b[1] = 0x0b;
        } else {
            DWORD old = 0, ignored;
            if (!VirtualProtect(b, 2, PAGE_READWRITE, &old)) continue;
            b[1] = 0x0b;
            VirtualProtect(b, 2, old, &ignored);
        }
        g_sites.insert(s);
        ++n;
    }
    g_sites_patched.fetch_add(n, std::memory_order_relaxed);
    return n;
}

// A guest page asked to be executable is first run: patch what can be reached
// from here, then let the pages decoded run.
bool on_execute(uintptr_t rip) {
    const uintptr_t page = rip & ~(kPage - 1);
    AcquireSRWLockExclusive(&g_lock);
    if (!g_pending.count(page)) {
        ReleaseSRWLockExclusive(&g_lock);
        // Another thread may have opened it while this one waited.
        MEMORY_BASIC_INFORMATION mbi{};
        return VirtualQuery(reinterpret_cast<void*>(rip), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
               executable(mbi.Protect) && !(mbi.Protect & PAGE_GUARD);
    }
    if (arm64::is_guest_code(reinterpret_cast<const void*>(rip))) {
        // arm64 code is never run by the host: the arm64 handler takes the fault.
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    uintptr_t lo = page, hi = page + kPage;
    while (g_pending.count(lo - kPage)) lo -= kPage;
    while (g_pending.count(hi)) hi += kPage;
    const CodeRange run{lo, hi};
    Walk walk(&run, 1);
    walk.from(rip);
    patch_sites(walk.sites(), false);
    std::vector<uintptr_t> open = walk.pages();
    open.push_back(page);
    for (uintptr_t p : open) {
        auto it = g_pending.find(p);
        if (it == g_pending.end()) continue;
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(p), kPage, win_exec_protect(it->second), &old);
        g_pending.erase(it);
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(lo), hi - lo);
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

bool is_site(uintptr_t rip) {
    AcquireSRWLockShared(&g_lock);
    const bool yes = g_sites.count(rip) != 0;
    ReleaseSRWLockShared(&g_lock);
    return yes;
}

// First in the vectored chain, so that neither the guest's signal emulation
// nor the crash report sees a fault that is only a system call or a first run.
LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    CONTEXT* c = ep->ContextRecord;
    if (r->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
        const auto* ip = reinterpret_cast<const uint8_t*>(c->Rip);
        if (!is_site(c->Rip) || ip[0] != 0x0f || ip[1] != 0x0b) return EXCEPTION_CONTINUE_SEARCH;
        const uint64_t args[6] = {c->Rdi, c->Rsi, c->Rdx, c->R10, c->R8, c->R9};
        uint64_t result;
        if (!linux_syscall(c->Rax, args, &result)) {
            report_unknown(c->Rax, c->Rip);
            result = static_cast<uint64_t>(-kENOSYS);
        }
        g_calls_answered.fetch_add(1, std::memory_order_relaxed);
        // What SYSCALL leaves: the result in rax, the return address in rcx, the flags in r11.
        c->Rax = result;
        c->Rcx = c->Rip + 2;
        c->R11 = c->EFlags;
        c->Rip += 2;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 &&
        r->ExceptionInformation[0] == 8 /* EXCEPTION_EXECUTE_FAULT */ && g_active.load(std::memory_order_relaxed)) {
        return on_execute(c->Rip) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void activate() {
    static std::once_flag once;
    std::call_once(once, [] {
        AddVectoredExceptionHandler(1, on_exception);
        g_active.store(true);
    });
}

}  // namespace

size_t instruction_length(const uint8_t* p, size_t avail) {
    Insn in;
    return decode(p, avail, &in) ? in.len : 0;
}

size_t patch_syscalls(const CodeRange* ranges, size_t nranges, const uintptr_t* entries, size_t nentries,
                      size_t* instructions) {
    activate();
    Walk walk(ranges, nranges);
    for (size_t i = 0; i < nentries; ++i) walk.from(entries[i]);
    if (instructions) *instructions = walk.instructions();
    AcquireSRWLockExclusive(&g_lock);
    const size_t n = patch_sites(walk.sites(), true);
    ReleaseSRWLockExclusive(&g_lock);
    return n;
}

int exec_protection(void* addr, size_t len, int prot) {
    if (!g_active.load(std::memory_order_relaxed) || len == 0) return prot;
    const uintptr_t lo = reinterpret_cast<uintptr_t>(addr) & ~(kPage - 1);
    const uintptr_t hi = (reinterpret_cast<uintptr_t>(addr) + len + kPage - 1) & ~(kPage - 1);
    if (prot & kProtExec) {
        // Private memory only: a host module's pages (the arm64 bridge's own checks keep a guest
        // from changing those) and shared file views are left as asked.
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(addr, &mbi, sizeof mbi) || mbi.State == MEM_FREE || mbi.Type != MEM_PRIVATE) return prot;
    }
    AcquireSRWLockExclusive(&g_lock);
    for (uintptr_t p = lo; p < hi; p += kPage) {
        if (prot & kProtExec) g_pending[p] = prot;
        else g_pending.erase(p);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return (prot & kProtExec) ? ((prot & ~kProtExec) | kProtRead) : prot;
}

void forget(const void* addr, size_t len) {
    if (!g_active.load(std::memory_order_relaxed) || len == 0) return;
    const auto lo = reinterpret_cast<uintptr_t>(addr);
    const uintptr_t hi = lo + len;
    AcquireSRWLockExclusive(&g_lock);
    for (auto it = g_sites.begin(); it != g_sites.end();) {
        it = (*it >= lo && *it < hi) ? g_sites.erase(it) : std::next(it);
    }
    g_pending.erase(g_pending.lower_bound(lo & ~(kPage - 1)), g_pending.lower_bound(hi));
    ReleaseSRWLockExclusive(&g_lock);
}

size_t sites_patched() { return g_sites_patched.load(std::memory_order_relaxed); }
size_t calls_answered() { return g_calls_answered.load(std::memory_order_relaxed); }

}  // namespace winapk::x86
