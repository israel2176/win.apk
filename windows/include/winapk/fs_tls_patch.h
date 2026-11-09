// fs_tls_patch.h -- the Bionic x86_64 stack guard on Windows.
//
// Every Android x86_64 function built with a stack protector loads the guard
// from Bionic's TLS block through the FS segment:
//
//     mov rax, qword ptr fs:[0x28]      64 48 8b 04 25 28 00 00 00
//     ...
//     sub rax, qword ptr fs:[0x28]      64 48 2b 04 25 28 00 00 00
//
// 0x28 is TLS_SLOT_STACK_GUARD (slot 5 x 8 bytes). Bionic makes FS point at
// that block with arch_prctl(ARCH_SET_FS); 64-bit Windows offers user mode no
// way to set the FS base, so these loads would read through a zero base.
//
// Windows keeps the TEB at the GS base instead, and the TEB starts with an
// NT_TIB, whose layout on x64 is:
//
//     0x00 ExceptionList   0x08 StackBase   0x10 StackLimit   0x18 SubSystemTib
//     0x20 FiberData       0x28 ArbitraryUserPointer          0x30 Self
//
// So swapping the one-byte FS prefix (0x64) for the GS prefix (0x65) keeps the
// instruction length and points the load at NT_TIB.ArbitraryUserPointer, a
// per-thread field user code may use. The canary only has to stay constant on
// a thread between a function's prologue and epilogue, which holds even on a
// thread whose slot was never initialized.
//
// Code the guest writes at run time never went through the load-time scan:
// packers and anti-tamper SDKs (IBM Trusteer's libtaz, for one) ship their
// .text encrypted and decrypt it from their first DT_INIT_ARRAY entry, so the
// scan saw only ciphertext. An unpatched guard load reads through FS's zero
// base and faults on address 0x28; a vectored handler rewrites the prefix of
// the faulting instruction then and resumes it (install_fs_stack_guard_fixup).
//
// PoC SCOPE. The scanner matches the exact 9-byte stack-guard encodings, not
// arbitrary FS accesses. It does not decode instructions, so a data byte run
// that happens to match would be patched as well. Win.apk phase 3 replaces this
// with a decoder-driven rewrite (Zydis) that also covers %fs:0 (__get_tls)
// and ELF TLS accesses, and moves the guard into a slot Win.apk owns.

#pragma once

#include <cstddef>
#include <cstdint>

namespace winapk {

// Rewrites the stack-guard loads in [code, code + len). Returns sites patched.
size_t patch_fs_stack_guard_sites(uint8_t* code, size_t len);

// Installs, once per process, the vectored handler that patches a stack-guard
// load at the moment it faults: one the load-time scan could not see because
// the guest wrote or decrypted that code after loading.
void install_fs_stack_guard_fixup();

// How many guard loads the handler has patched so far.
size_t fs_stack_guard_sites_fixed_at_run_time();

// Sets the value guest code reads as its stack guard on the calling thread.
void set_guest_stack_guard_for_current_thread(uint64_t value);

// Reads gs:[0x28] directly: what a patched guest load sees on this thread.
uint64_t read_guest_stack_guard_slot();

// Commits up to 1 MB of the calling thread's stack below the part already in
// use, once per thread, before guest code runs on it. Code built for Linux
// does not probe its stack page by page as Windows code must (__chkstk): a
// frame larger than a page writes past the guard page into memory that is
// only reserved, and Windows cannot even deliver that fault, since it has no
// stack to deliver it on, so the process ends with 0xC0000005 and not a line
// in the log (Pixel Wheels in FreeType). On Linux the whole stack mapping is
// usable. The guard region goes to the bottom of the committed part, so the
// stack still grows, and overflows, the Windows way below it. Returns how far
// StackLimit moved down (0: already done on this thread, or nothing to do; a
// layout it does not know is said once in the log).
size_t commit_guest_stack_for_current_thread();

}  // namespace winapk
