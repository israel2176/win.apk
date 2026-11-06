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
