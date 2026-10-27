// arm64_abi.cpp -- the Bionic shim's functions whose structs arm64 lays out
// differently from x86_64 (arm64_guest.h, abi_override).
//
// The shim is written against Android's x86_64 headers, which is what the
// x86_64 path needs. Most of bionic's LP64 structs are the same on arm64, but
// not all: struct stat (x86_64: 144 bytes, st_nlink before st_mode; arm64:
// 128, asm-generic order) and struct epoll_event (packed to 12 bytes on
// x86_64, 16 on arm64). Passing an arm64 caller's struct to the x86_64
// function writes past the end of it -- a stack object in the caller's
// frame, so saved registers and the stack protector's canary go first. An
// arm64 library's imports of these names bind to the wrappers here, which
// call the shim with an x86_64 struct and convert. (On the Mac the shim is
// built for arm64 and has no such gap.)

#include "winapk/arm64_guest.h"

#include "winapk/guest_abi.h"
#include "winapk/mini_bionic.h"

#include <cstdint>
#include <cstring>
#include <vector>

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

}  // namespace

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
        {"epoll_ctl", reinterpret_cast<void*>(&a64_epoll_ctl)},
        {"epoll_wait", reinterpret_cast<void*>(&a64_epoll_wait)},
        {"epoll_pwait", reinterpret_cast<void*>(&a64_epoll_pwait)},
    };
    for (const auto& o : kOverrides) {
        if (std::strcmp(o.name, name) == 0) return mini_bionic_resolve(name) ? o.fn : nullptr;
    }
    return nullptr;
}

}  // namespace winapk::arm64
