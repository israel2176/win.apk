// arm64_patch_guest.c -- a library that rewrites its own code, as an inline
// hook (Dobby, ShadowHook, a game's anti-cheat) or a loader that decrypts
// its text in place does: mprotect, write, cache maintenance (IC IVAU), call.
// The next call must run the new code, whichever engine runs it; a
// translation made ahead of time from the file's bytes no longer describes
// the library (arm64_aot.h, "Code the guest rewrites"). test/arm64_test.cpp
// runs it once per engine, each on its own copy of the library.
//
// Built like arm64_bridge_guest.c (no NDK; libc bound to the Bionic shim).

typedef unsigned long size_t;
typedef unsigned long uint64_t;
typedef unsigned int uint32_t;
typedef unsigned long uintptr_t;

int mprotect(void*, size_t, int);

#define EXPORT __attribute__((visibility("default")))

// An inline hook: the first instruction of flow_patch_target is overwritten
// (mprotect, write, cache maintenance), and the next call must run the new
// code. Then the old instruction is put back. 1 * 100 + 42 when the patch
// is seen.
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl flow_patch_target\n"
    ".type flow_patch_target, %function\n"
    "flow_patch_target:\n"
    "    mov w0, #1\n"
    "    ret\n"
    ".size flow_patch_target, .-flow_patch_target\n");

uint64_t flow_patch_target(void);

static void write_code(volatile uint32_t* at, uint32_t insn) {
    const uintptr_t page = (uintptr_t)at & ~4095ul;
    mprotect((void*)page, 8192, 7 /* PROT_READ | PROT_WRITE | PROT_EXEC */);
    *at = insn;
    __asm__ volatile("dc cvau, %0\n dsb ish\n ic ivau, %0\n dsb ish\n isb" ::"r"(at) : "memory");
    mprotect((void*)page, 8192, 5 /* PROT_READ | PROT_EXEC */);
}

EXPORT uint64_t flow_patch(void) {
    volatile uint32_t* at = (volatile uint32_t*)&flow_patch_target;
    const uint32_t original = *at;
    const uint64_t before = flow_patch_target();
    write_code(at, 0x52800540);  // mov w0, #42
    const uint64_t after = flow_patch_target();
    write_code(at, original);
    return before * 100 + after;
}

// The same across threads: a second thread calls flow_patch_target2 over and
// over (so a JIT has compiled it on that thread), this one patches it, and
// the other thread must see the new code (Arm makes IC IVAU reach every
// core). Returns 1 when it did, 0 when it kept running the old code.
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl flow_patch_target2\n"
    ".type flow_patch_target2, %function\n"
    "flow_patch_target2:\n"
    "    mov w0, #1\n"
    "    ret\n"
    ".size flow_patch_target2, .-flow_patch_target2\n");

uint64_t flow_patch_target2(void);
typedef long pthread_t;
int pthread_create(pthread_t*, const void*, void* (*)(void*), void*);
int pthread_join(pthread_t, void**);

static volatile uint64_t g_calls, g_patched;

static void* call_until_patched(void* arg) {
    (void)arg;
    // Up to 20 million calls after the patch: far longer than any engine
    // needs to see it, and a fraction of a second.
    uint64_t after = 0;
    for (;;) {
        const uint64_t r = flow_patch_target2();
        __atomic_add_fetch(&g_calls, 1, __ATOMIC_RELAXED);
        if (r == 42) return (void*)1;
        if (__atomic_load_n(&g_patched, __ATOMIC_ACQUIRE) && ++after > 20000000) return (void*)0;
    }
}

EXPORT uint64_t flow_patch_threads(void) {
    volatile uint32_t* at = (volatile uint32_t*)&flow_patch_target2;
    const uint32_t original = *at;
    g_calls = 0;
    g_patched = 0;
    pthread_t t;
    if (pthread_create(&t, 0, call_until_patched, 0) != 0) return 2;
    while (__atomic_load_n(&g_calls, __ATOMIC_RELAXED) < 10000) {
    }
    write_code(at, 0x52800540);  // mov w0, #42
    __atomic_store_n(&g_patched, 1, __ATOMIC_RELEASE);
    void* seen = 0;
    pthread_join(t, &seen);
    write_code(at, original);
    return (uint64_t)seen;
}
