// fs_tls_patch.cpp -- see fs_tls_patch.h for why the FS prefix becomes GS.

#include "winapk/fs_tls_patch.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include <intrin.h>
#include <windows.h>

namespace winapk {

namespace {

constexpr uint8_t kPrefixFs = 0x64;
constexpr uint8_t kPrefixGs = 0x65;
constexpr uint32_t kStackGuardOffset = 0x28;  // TLS_SLOT_STACK_GUARD * 8

// REX.W, optionally REX.R (destination register r8-r15). REX.X/REX.B must be
// clear: with the SIB byte below they would change the addressing mode.
bool is_rex_w(uint8_t b) { return b == 0x48 || b == 0x4c; }

// Read-only uses of the guard: mov r64, m64 / sub / xor / cmp (both operand
// orders). A write (89 /r) is never patched; only Bionic's libc init writes
// the guard, and that code does not run here.
bool is_guard_read_opcode(uint8_t op) {
    return op == 0x8b || op == 0x2b || op == 0x33 || op == 0x3b || op == 0x39;
}

// prefix, REX, opcode, ModRM, SIB, disp32
constexpr size_t kInsnLen = 9;

// Whether the 9 bytes at `p` are a stack-guard read, whatever its segment
// prefix (p[0]) is.
bool is_guard_read_after_prefix(const uint8_t* p) {
    if (!is_rex_w(p[1]) || !is_guard_read_opcode(p[2])) return false;
    // ModRM mod=00 rm=100 (SIB follows); SIB 0x25 = no base, no index,
    // disp32: an absolute segment-relative address.
    if ((p[3] & 0xc7) != 0x04 || p[4] != 0x25) return false;
    uint32_t disp;
    std::memcpy(&disp, p + 5, sizeof(disp));
    return disp == kStackGuardOffset;
}

}  // namespace

size_t patch_fs_stack_guard_sites(uint8_t* code, size_t len) {
    size_t patched = 0;
    for (size_t i = 0; i + kInsnLen <= len; ++i) {
        if (code[i] != kPrefixFs || !is_guard_read_after_prefix(code + i)) continue;
        code[i] = kPrefixGs;
        ++patched;
        i += kInsnLen - 1;
    }
    return patched;
}

namespace {

SRWLOCK g_fixup_lock = SRWLOCK_INIT;
std::atomic<size_t> g_fixed_at_run_time{0};

// A guard load the scan never saw, faulting on FS's zero base: give it the GS
// prefix and run it again. Everything else is left to the next handler. The
// bytes are read in order and each one matched says the instruction goes on,
// so none is read past the end of the instruction the CPU has just fetched.
LONG CALLBACK fix_unpatched_guard_load(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (rec->ExceptionInformation[0] != 0) return EXCEPTION_CONTINUE_SEARCH;  // a read
    auto* insn = reinterpret_cast<uint8_t*>(info->ContextRecord->Rip);
    if (insn[0] != kPrefixFs && insn[0] != kPrefixGs) return EXCEPTION_CONTINUE_SEARCH;
    if (!is_guard_read_after_prefix(insn)) return EXCEPTION_CONTINUE_SEARCH;
    AcquireSRWLockExclusive(&g_fixup_lock);
    bool resume = insn[0] == kPrefixGs;  // another thread got here first
    if (insn[0] == kPrefixFs) {
        // The page is usually back to read-execute by now (a decryptor
        // restores it): open it for the one byte, then put it back.
        DWORD old = 0;
        if (VirtualProtect(insn, 1, PAGE_EXECUTE_READWRITE, &old)) {
            insn[0] = kPrefixGs;
            DWORD ignored;
            VirtualProtect(insn, 1, old, &ignored);
            FlushInstructionCache(GetCurrentProcess(), insn, kInsnLen);
            g_fixed_at_run_time.fetch_add(1, std::memory_order_relaxed);
            resume = true;
        }
    }
    ReleaseSRWLockExclusive(&g_fixup_lock);
    return resume ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install_fs_stack_guard_fixup() {
    static std::once_flag once;
    // First in the chain: the guest's SIGSEGV emulation and the crash report
    // must not see a fault that is only a load the scan missed.
    std::call_once(once, [] { AddVectoredExceptionHandler(1, fix_unpatched_guard_load); });
}

size_t fs_stack_guard_sites_fixed_at_run_time() { return g_fixed_at_run_time.load(std::memory_order_relaxed); }

void set_guest_stack_guard_for_current_thread(uint64_t value) {
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    tib->ArbitraryUserPointer = reinterpret_cast<PVOID>(value);
}

uint64_t read_guest_stack_guard_slot() {
    return __readgsqword(kStackGuardOffset);
}

}  // namespace winapk
