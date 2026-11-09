// setjmp.cpp -- setjmp and longjmp for x86_64 guest code.
//
// libpng, libjpeg, FreeType's validators and Skia's codecs (in Flutter's
// engine) report errors with longjmp. Without these the stub returned 0 for
// setjmp and did nothing for longjmp, so an error path ran on into freed or
// half-built state: Pixel Wheels (libGDX, FreeType) and LocalSend (Flutter)
// died with access violations. An arm64 guest never reaches this file: its
// setjmp and longjmp are the bridge's intrinsics (arm64_guest.h), asked
// before the shim.
//
// Only the registers the System V ABI keeps across a call need saving (rbx,
// rbp, r12-r15, rsp and the return address); the rest are the caller's to
// lose. Bionic's x86_64 jmp_buf is 11 longs; the slots used are the
// runtime's own, since only these functions read them. sigsetjmp(buf, 1)
// also saves the thread's signal mask and siglongjmp restores it, as on
// Linux: a handler that longjmps out of a fault would otherwise leave the
// signal blocked for good (signal.cpp).

#include "bionic_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace winapk::bionic {

namespace {

// jmp_buf layout: [0] rbx, [1] rbp, [2] r12, [3] r13, [4] r14, [5] r15,
// [6] rsp after return, [7] return address, [8] a magic (0x5e7a3b1c0f2d4a69)
// that longjmp checks, [9] the signal mask, [10] 1 if [9] was saved.

// Microsoft x64 helpers the naked functions call: callee-saved rdi and rsi
// survive the call, and the caller's own registers are not saved yet (setjmp)
// or not restored yet (longjmp) when it happens.
extern "C" uint64_t winapk_sig_mask_get() { return signal_mask_of_current_thread(); }
extern "C" void winapk_sig_mask_set(uint64_t mask) { signal_set_mask_of_current_thread(mask); }
extern "C" void winapk_sig_stack_fixup(uint64_t rsp);  // signal.cpp
extern "C" __attribute__((naked, sysv_abi)) int g_setjmp(int64_t*) {
    __asm__ volatile(
        "movq %%rbx, 0(%%rdi)\n"
        "movq %%rbp, 8(%%rdi)\n"
        "movq %%r12, 16(%%rdi)\n"
        "movq %%r13, 24(%%rdi)\n"
        "movq %%r14, 32(%%rdi)\n"
        "movq %%r15, 40(%%rdi)\n"
        "leaq 8(%%rsp), %%rax\n"
        "movq %%rax, 48(%%rdi)\n"
        "movq (%%rsp), %%rax\n"
        "movq %%rax, 56(%%rdi)\n"
        "movabsq $0x5e7a3b1c0f2d4a69, %%rax\n"
        "movq %%rax, 64(%%rdi)\n"
        "movq $0, 80(%%rdi)\n"
        "xorl %%eax, %%eax\n"
        "ret\n" ::: "memory");
}

// sigsetjmp(buf, savesigs): the mask first (the call clobbers nothing setjmp
// saves), then setjmp, which clears the saved flag; set it after.
extern "C" __attribute__((naked, sysv_abi)) int g_sigsetjmp(int64_t*, int) {
    __asm__ volatile(
        "testl %%esi, %%esi\n"
        "jz g_setjmp\n"
        "subq $40, %%rsp\n"  // shadow space; rsp was 8 mod 16 at entry
        "call winapk_sig_mask_get\n"
        "addq $40, %%rsp\n"
        "movq %%rax, 72(%%rdi)\n"
        // The tail: what g_setjmp does, with the saved flag set.
        "movq %%rbx, 0(%%rdi)\n"
        "movq %%rbp, 8(%%rdi)\n"
        "movq %%r12, 16(%%rdi)\n"
        "movq %%r13, 24(%%rdi)\n"
        "movq %%r14, 32(%%rdi)\n"
        "movq %%r15, 40(%%rdi)\n"
        "leaq 8(%%rsp), %%rax\n"
        "movq %%rax, 48(%%rdi)\n"
        "movq (%%rsp), %%rax\n"
        "movq %%rax, 56(%%rdi)\n"
        "movabsq $0x5e7a3b1c0f2d4a69, %%rax\n"
        "movq %%rax, 64(%%rdi)\n"
        "movq $1, 80(%%rdi)\n"
        "xorl %%eax, %%eax\n"
        "ret\n" ::: "memory");
}

void longjmp_bad(int64_t* buf) {
    std::fprintf(stderr, "[win.apk] guest called longjmp on a jmp_buf (%p) that setjmp did not fill\n",
                 static_cast<void*>(buf));
    std::fflush(stderr);
    std::abort();
}

extern "C" __attribute__((naked, sysv_abi)) void g_longjmp(int64_t*, int) {
    __asm__ volatile(
        "movabsq $0x5e7a3b1c0f2d4a69, %%rax\n"
        "cmpq %%rax, 64(%%rdi)\n"
        "jne 1f\n"
        // Out of a signal handler's alternate stack: the TEB's stack bounds back first (signal.cpp).
        "pushq %%rsi\n"
        "subq $32, %%rsp\n"  // shadow space; rsp was 8 mod 16 at entry, the push made it 0
        "movq 48(%%rdi), %%rcx\n"
        "call winapk_sig_stack_fixup\n"
        "addq $32, %%rsp\n"
        "popq %%rsi\n"
        "cmpq $1, 80(%%rdi)\n"  // siglongjmp after sigsetjmp(buf, 1): the mask back first
        "jne 3f\n"
        "pushq %%rsi\n"
        "subq $32, %%rsp\n"  // shadow space; rsp was 8 mod 16 at entry, the push made it 0
        "movq 72(%%rdi), %%rcx\n"
        "call winapk_sig_mask_set\n"
        "addq $32, %%rsp\n"
        "popq %%rsi\n"
        "3:\n"
        "movl %%esi, %%eax\n"
        "testl %%eax, %%eax\n"
        "jnz 2f\n"
        "movl $1, %%eax\n"
        "2:\n"
        "movq 0(%%rdi), %%rbx\n"
        "movq 8(%%rdi), %%rbp\n"
        "movq 16(%%rdi), %%r12\n"
        "movq 24(%%rdi), %%r13\n"
        "movq 32(%%rdi), %%r14\n"
        "movq 40(%%rdi), %%r15\n"
        "movq 48(%%rdi), %%rsp\n"
        "jmpq *56(%%rdi)\n"
        "1:\n"
        "subq $8, %%rsp\n"  // a call frame aligned as the ABI wants
        "call *%0\n"
        "ud2\n" ::"r"(&longjmp_bad)
        : "memory");
}

const ShimSymbol kSymbols[] = {
    WINAPK_SHIM("setjmp", g_setjmp),       WINAPK_SHIM("_setjmp", g_setjmp),
    WINAPK_SHIM("sigsetjmp", g_sigsetjmp), WINAPK_SHIM("longjmp", g_longjmp),
    WINAPK_SHIM("_longjmp", g_longjmp),    WINAPK_SHIM("siglongjmp", g_longjmp),
    WINAPK_SHIM("__longjmp_chk", g_longjmp),
};

}  // namespace

ShimTable setjmp_symbols() { return WINAPK_SHIM_TABLE(kSymbols); }

}  // namespace winapk::bionic
