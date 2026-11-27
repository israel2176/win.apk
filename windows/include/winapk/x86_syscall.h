// x86_syscall.h -- raw Linux system calls in x86_64 guest code.
//
// An Android x86_64 library may enter the kernel itself, with the SYSCALL
// instruction (0f 05), instead of through libc: a self-unpacking library
// (UPX's loader stub maps, writes and protects its own pages that way), a
// protector that reads /proc without libc's functions, a runtime with its own
// syscall wrappers. On Windows the same instruction enters the Windows kernel,
// whose service numbers are not Linux's: Linux's mmap (9) is some unrelated NT
// service, called with arguments it does not expect.
//
// So every SYSCALL the loader can find in x86_64 guest code becomes UD2 (0f 0b,
// the same length), and a vectored handler answers the illegal instruction at
// one of those sites as the kernel would: rax = the call's result (-errno on
// failure), rcx = the next instruction, r11 = the flags, rip past it. The
// calls are the Bionic shim's functions under the kernel's numbering
// (linux_syscall).
//
// Where SYSCALL instructions are found:
//
//   * At load, in a library's executable segments, by decoding instructions
//     from every entry point the library names (DT_INIT, DT_INIT_ARRAY,
//     DT_FINI(_ARRAY), its defined function symbols in .dynsym and .symtab,
//     e_entry) and following direct jumps and calls. Only bytes decoded as a
//     SYSCALL instruction on such a path are changed; data that happens to hold
//     0f 05 is not.
//   * In code the guest writes at run time (a packer decrypting its .text, a
//     loader stub unpacking the next stage), once a loaded library has been
//     found to contain a SYSCALL (a process whose libraries have none is
//     never changed, and never pays for it): memory the guest asks to make
//     executable through mmap or mprotect is given everything it asked for but
//     execution, and when it first runs, the handler decodes from the
//     instruction that faulted, patches what it finds, and then makes the
//     pages it decoded executable. A later mprotect to executable after the
//     guest rewrote them starts that over.
//
// What differs from a device (docs/research/packers.md):
//   * A guest that reads its own code back sees 0f 0b where it wrote 0f 05.
//   * A SYSCALL reached only through an indirect jump into a page that was
//     already executable is not found.
//   * Windows writes the exception record below the stack pointer, where the
//     System V ABI lets a leaf function keep 128 bytes of locals (the red
//     zone); a leaf that keeps them there across a SYSCALL loses them.

#pragma once

#include <cstddef>
#include <cstdint>

namespace winapk::x86 {

// A raw Linux system call, number `nr` (rax) under x86_64's numbering,
// arguments rdi, rsi, rdx, r10, r8, r9. False if the shim cannot answer it;
// otherwise *result is what the kernel would return (-errno on failure).
bool linux_syscall(uint64_t nr, const uint64_t args[6], uint64_t* result);

// The numbers the shim's syscall(3) does not answer itself (misc.cpp), with
// libc's convention: -1 and errno on failure, ENOSYS for one it does not have.
int64_t other_syscall(int64_t nr, const int64_t args[6]);

struct CodeRange {
    uintptr_t lo, hi;  // [lo, hi), readable
};

// Decodes from each entry in `entries` that lies in one of `ranges`, following
// direct branches within them, and turns every SYSCALL met into UD2, which the
// handler then answers. The bytes must be writable. Returns the number of
// sites changed, and in *instructions (when given) how many instructions were
// decoded. When there is a site, installs the handler and turns on the
// run-time scan (exec_protection); with none, nothing is changed at all.
// Which addresses were decoded is kept as one bit per byte of the ranges'
// span, so a walk costs an eighth of the code's size in memory and time
// linear in it.
size_t patch_syscalls(const CodeRange* ranges, size_t nranges, const uintptr_t* entries, size_t nentries,
                      size_t* instructions = nullptr);

// The protection the shim gives [addr, addr + len) for a guest's mmap or
// mprotect asking for `prot` (Linux PROT_* bits): `prot` itself, or, once a
// library with a SYSCALL has been loaded, `prot` without PROT_EXEC (and with
// PROT_READ) for private memory, with the request recorded so that the pages
// become executable when first run, after their SYSCALL instructions are
// patched. A request without PROT_EXEC forgets an earlier one.
int exec_protection(void* addr, size_t len, int prot);

// [addr, addr + len) is no longer guest code (munmap, dlclose): its sites and
// requests are forgotten.
void forget(const void* addr, size_t len);

// How many sites patch_syscalls and the run-time scans have changed, and how
// many raw calls the handler has answered.
size_t sites_patched();
size_t calls_answered();

// The length of the x86_64 instruction at `p`, at most `avail` bytes read, or
// 0 if it is not one this decoder knows. Exposed for its test.
size_t instruction_length(const uint8_t* p, size_t avail);

}  // namespace winapk::x86
