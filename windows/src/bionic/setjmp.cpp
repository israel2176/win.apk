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
// runtime's own, since only these functions read them. The signal mask is not
// saved: the guest has no signals (signal.cpp).

#include "bionic_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace winapk::bionic {

namespace {

// jmp_buf layout: [0] rbx, [1] rbp, [2] r12, [3] r13, [4] r14, [5] r15,
// [6] rsp after return, [7] return address, [8] a magic (0x5e7a3b1c0f2d4a69)
// that longjmp checks.
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
        "xorl %%eax, %%eax\n"
        "ret\n" ::: "memory");
}

// sigsetjmp(buf, savesigs): the same, the mask being nothing to save.
extern "C" __attribute__((naked, sysv_abi)) int g_sigsetjmp(int64_t*, int) {
    __asm__ volatile("jmp g_setjmp\n");
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
