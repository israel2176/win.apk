// arm64_guest.cpp -- the bridge between the arm64 interpreter and the Windows
// x64 host. See arm64_guest.h.

#include "winapk/arm64_guest.h"

#include "winapk/abi_thunk.h"
#include "winapk/arm64_cpu.h"
#include "winapk/guest_abi.h"
#include "winapk/linker.h"
#include "winapk/mini_bionic.h"

#include "arm64_jit.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace {

// ---- the two machine-code edges ---------------------------------------------
//
// winapk_arm64_call_sysv(fn, frame): calls the System V function `fn` with the
// registers and stack words in `frame`, and stores rax, rdx, xmm0 and xmm1
// back into it. It is itself System V, so the C++ caller (Microsoft x64)
// saves rsi, rdi and xmm6-xmm15 around it, as for any WINAPK_GUEST_ABI call.
//
// winapk_arm64_sysv_entry: a System V function body with the guest pc in r10.
// It spills the argument registers into a CallbackFrame and hands it to
// winapk_arm64_dispatch, which runs the guest and fills in the results.

struct SysvCallFrame {
    uint64_t gpr[6];         // rdi rsi rdx rcx r8 r9
    uint64_t fpr[8];         // xmm0..xmm7, low 64 bits
    const uint64_t* stack;   // words for the callee's stack arguments
    uint64_t nstack;
    uint64_t rax, rdx, xmm0, xmm1;
};
static_assert(offsetof(SysvCallFrame, fpr) == 48 && offsetof(SysvCallFrame, stack) == 112 &&
                  offsetof(SysvCallFrame, nstack) == 120 && offsetof(SysvCallFrame, rax) == 128 &&
                  offsetof(SysvCallFrame, rdx) == 136 && offsetof(SysvCallFrame, xmm0) == 144 &&
                  offsetof(SysvCallFrame, xmm1) == 152,
              "layout used by the assembly below");

struct CallbackFrame {
    uint64_t gpr[6];
    uint64_t fpr[8];
    const uint64_t* stack_args;  // the caller's stack arguments (just above the return address)
    uint64_t rax, rdx, xmm0;
};
static_assert(offsetof(CallbackFrame, stack_args) == 112 && offsetof(CallbackFrame, rax) == 120 &&
                  offsetof(CallbackFrame, rdx) == 128 && offsetof(CallbackFrame, xmm0) == 136 &&
                  sizeof(CallbackFrame) <= 160,
              "layout used by the assembly below");

}  // namespace

extern "C" WINAPK_GUEST_ABI void winapk_arm64_call_sysv(const void* fn, SysvCallFrame* frame);
extern "C" WINAPK_GUEST_ABI void winapk_arm64_sysv_entry();
extern "C" WINAPK_GUEST_ABI void winapk_arm64_dispatch(uint64_t pc, CallbackFrame* frame);

asm(R"(
    .text
    .p2align 4
    .globl winapk_arm64_call_sysv
winapk_arm64_call_sysv:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rbx
    pushq %r12
    movq %rsi, %r12
    movq %rdi, %r11
    movq 120(%r12), %rcx
    movq %rcx, %rax
    shlq $3, %rax
    subq %rax, %rsp
    andq $-16, %rsp
    movq 112(%r12), %rsi
    xorl %ebx, %ebx
1:
    cmpq %rcx, %rbx
    jae 2f
    movq (%rsi,%rbx,8), %rax
    movq %rax, (%rsp,%rbx,8)
    incq %rbx
    jmp 1b
2:
    movq 0(%r12), %rdi
    movq 8(%r12), %rsi
    movq 16(%r12), %rdx
    movq 24(%r12), %rcx
    movq 32(%r12), %r8
    movq 40(%r12), %r9
    movq 48(%r12), %xmm0
    movq 56(%r12), %xmm1
    movq 64(%r12), %xmm2
    movq 72(%r12), %xmm3
    movq 80(%r12), %xmm4
    movq 88(%r12), %xmm5
    movq 96(%r12), %xmm6
    movq 104(%r12), %xmm7
    movl $8, %eax
    callq *%r11
    movq %rax, 128(%r12)
    movq %rdx, 136(%r12)
    movq %xmm0, 144(%r12)
    movq %xmm1, 152(%r12)
    leaq -16(%rbp), %rsp
    popq %r12
    popq %rbx
    popq %rbp
    retq

    .p2align 4
    .globl winapk_arm64_sysv_entry
winapk_arm64_sysv_entry:
    pushq %rbp
    movq %rsp, %rbp
    subq $160, %rsp
    movq %rdi, 0(%rsp)
    movq %rsi, 8(%rsp)
    movq %rdx, 16(%rsp)
    movq %rcx, 24(%rsp)
    movq %r8, 32(%rsp)
    movq %r9, 40(%rsp)
    movq %xmm0, 48(%rsp)
    movq %xmm1, 56(%rsp)
    movq %xmm2, 64(%rsp)
    movq %xmm3, 72(%rsp)
    movq %xmm4, 80(%rsp)
    movq %xmm5, 88(%rsp)
    movq %xmm6, 96(%rsp)
    movq %xmm7, 104(%rsp)
    leaq 16(%rbp), %rax
    movq %rax, 112(%rsp)
    movq %r10, %rdi
    movq %rsp, %rsi
    callq winapk_arm64_dispatch
    movq 120(%rsp), %rax
    movq 128(%rsp), %rdx
    movq 136(%rsp), %xmm0
    leave
    retq
)");

namespace winapk::arm64 {

namespace {

constexpr size_t kGuestStack = 4 * 1024 * 1024;
// Reserved but never committed below each guest stack, so an overflow faults
// at once instead of writing into whatever is mapped next to it.
constexpr size_t kStackGuard = 64 * 1024;
// Never handed to guest code at the top of a guest stack: the host-call path
// copies a fixed number of words above sp, and a va_list conversion reads its
// overflow area, without knowing where the arguments end.
constexpr size_t kStackSlack = 1024;
// Guest stack words passed on to a host function after x6 and x7 (AAPCS64
// stack arguments become System V stack arguments in the same order).
constexpr size_t kGuestStackWords = 14;
// Caller stack words a callback passes on to the guest after x6 and x7.
constexpr size_t kCallbackStackWords = 8;

// run() stops when the guest returns here. It is never guest code and never
// called: only its address matters.
const uint8_t g_return_marker = 0;
uint64_t return_address() { return reinterpret_cast<uint64_t>(&g_return_marker); }

// ---- registered code ranges ---------------------------------------------------

struct Range {
    uint64_t lo, hi;
};
SRWLOCK g_ranges_lock = SRWLOCK_INIT;
std::vector<Range> g_ranges;

bool code_range(uint64_t pc, uint64_t* lo, uint64_t* hi) {
    AcquireSRWLockShared(&g_ranges_lock);
    bool found = false;
    for (const Range& r : g_ranges) {
        if (pc >= r.lo && pc < r.hi) {
            *lo = r.lo;
            *hi = r.hi;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_ranges_lock);
    return found;
}

// ---- per-thread guest state -----------------------------------------------------

struct ThreadState {
    Cpu cpu;
    uint8_t* stack = nullptr;
    uint64_t stack_top = 0;
    int depth = 0;  // nested runs on this thread
    std::unique_ptr<JitEngine> jit;  // created on first use; reads cpu.tpidr in place
    bool jit_tried = false;
    // bionic's TLS slots for arm64 start at index -1 (TLS_SLOT_BIONIC_TLS);
    // TPIDR_EL0 points at slot 0, the stack guard is slot 5.
    uint64_t tls[24] = {};

    ThreadState() {
        stack = static_cast<uint8_t*>(VirtualAlloc(nullptr, kStackGuard + kGuestStack, MEM_RESERVE, PAGE_NOACCESS));
        if (!stack || !VirtualAlloc(stack + kStackGuard, kGuestStack, MEM_COMMIT, PAGE_READWRITE)) {
            std::fprintf(stderr, "[win.apk] arm64: cannot allocate a guest stack\n");
            std::abort();
        }
        stack_top = (reinterpret_cast<uint64_t>(stack) + kStackGuard + kGuestStack - kStackSlack) & ~uint64_t{15};
        cpu.tpidr = reinterpret_cast<uint64_t>(&tls[1]);
        tls[1 + 5] = 0x5eed5eedc0ffee00ull ^ (reinterpret_cast<uint64_t>(this) << 8);
    }
    ~ThreadState() {
        jit.reset();
        if (stack) VirtualFree(stack, 0, MEM_RELEASE);
    }
};

// WINAPK_ARM64_ENGINE=interp runs everything through the interpreter, for
// comparing the two engines on an app.
Engine initial_engine() {
    char buf[16];
    const DWORD n = GetEnvironmentVariableA("WINAPK_ARM64_ENGINE", buf, sizeof buf);
    if (n > 0 && n < sizeof buf && std::strcmp(buf, "interp") == 0) return Engine::kInterpreter;
    return jit_built() ? Engine::kJit : Engine::kInterpreter;
}

std::atomic<Engine> g_engine{initial_engine()};

// Runs ts.cpu from its pc until it returns to its stop_pc.
void execute(ThreadState& ts) {
    if (g_engine.load(std::memory_order_relaxed) == Engine::kJit) {
        if (!ts.jit_tried) {
            ts.jit_tried = true;
            ts.jit = make_jit_engine(ts.cpu);
        }
        if (ts.jit) {
            ts.jit->run(ts.cpu);
            return;
        }
    }
    run(ts.cpu);
}

thread_local std::unique_ptr<ThreadState> t_state;

ThreadState& thread_state() {
    if (!t_state) t_state.reset(new ThreadState());
    return *t_state;
}

// ---- describing guest addresses in messages --------------------------------------

std::string where(uint64_t pc) {
    char buf[512];
    GuestLinker* linker = GuestLinker::process();
    GuestLinker::AddrInfo info;
    if (linker && linker->describe(reinterpret_cast<const void*>(pc), &info)) {
        const uint64_t off = pc - reinterpret_cast<uint64_t>(info.base);
        if (info.symbol) {
            std::snprintf(buf, sizeof buf, "%s+0x%llx (%s+0x%llx)", info.file, static_cast<unsigned long long>(off),
                          info.symbol,
                          static_cast<unsigned long long>(pc - reinterpret_cast<uint64_t>(info.symbol_addr)));
        } else {
            std::snprintf(buf, sizeof buf, "%s+0x%llx", info.file, static_cast<unsigned long long>(off));
        }
    } else {
        std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(pc));
    }
    return buf;
}

void fault(Cpu& c, uint32_t insn, const char* what) {
    std::fprintf(stderr, "[win.apk] arm64 guest stopped: %s, instruction 0x%08x at %s (lr %s)\n", what, insn,
                 where(c.pc).c_str(), where(c.x[30]).c_str());
    std::fflush(stderr);
    std::abort();
}

void svc(Cpu& c, uint32_t) {
    std::fprintf(stderr, "[win.apk] arm64 guest made a raw Linux system call %llu at %s; not supported\n",
                 static_cast<unsigned long long>(c.x[8]), where(c.pc - 4).c_str());
    std::fflush(stderr);
    std::abort();
}

// ---- va_list ------------------------------------------------------------------

// AAPCS64's va_list (Arm's "Procedure Call Standard", appendix B).
struct AapcsVaList {
    const uint8_t* stack;   // next stack argument
    const uint8_t* gr_top;  // end of the general register save area
    const uint8_t* vr_top;  // end of the FP/SIMD register save area
    int32_t gr_offs;        // negative offset from gr_top of the next GP register (0: none left)
    int32_t vr_offs;        // the same for the FP/SIMD registers, 16 bytes each
};

constexpr size_t kOverflowWords = 32;

// A System V va_list holding the same remaining arguments, with the storage
// it points into.
struct SysvVaStorage {
    SysVVaList va;
    alignas(16) uint8_t reg_save[176];
    uint64_t overflow[2 + kOverflowWords];
};

void convert_va_list(const AapcsVaList* a, SysvVaStorage* s) {
    std::memset(s, 0, sizeof(*s));
    const int gp_left = a->gr_offs < 0 ? -a->gr_offs / 8 : 0;   // at most 8
    const int fp_left = a->vr_offs < 0 ? -a->vr_offs / 16 : 0;  // at most 8
    const int gp_in_regs = std::min(gp_left, 6);
    const uint8_t* gp = a->gr_top + a->gr_offs;
    s->va.gp_offset = static_cast<uint32_t>(48 - 8 * gp_in_regs);
    std::memcpy(s->reg_save + s->va.gp_offset, gp, 8 * static_cast<size_t>(gp_in_regs));
    s->va.fp_offset = static_cast<uint32_t>(176 - 16 * fp_left);
    std::memcpy(s->reg_save + s->va.fp_offset, a->vr_top + a->vr_offs, 16 * static_cast<size_t>(fp_left));
    // Arguments System V would have read from registers but AAPCS64 had room
    // for (the 7th and 8th GP ones) come first in the overflow area.
    const int extra = gp_left - gp_in_regs;
    std::memcpy(s->overflow, gp + 8 * gp_in_regs, 8 * static_cast<size_t>(extra));
    if (a->stack) std::memcpy(s->overflow + extra, a->stack, 8 * kOverflowWords);
    s->va.overflow_arg_area = reinterpret_cast<uint8_t*>(s->overflow);
    s->va.reg_save_area = s->reg_save;
}

SRWLOCK g_va_lock = SRWLOCK_INIT;
std::unordered_map<uint64_t, unsigned> g_va_functions;

int va_list_index(uint64_t fn) {
    AcquireSRWLockShared(&g_va_lock);
    auto it = g_va_functions.find(fn);
    const int r = it == g_va_functions.end() ? -1 : static_cast<int>(it->second);
    ReleaseSRWLockShared(&g_va_lock);
    return r;
}

// ---- functions that act on the guest's own registers ----------------------------------
//
// setjmp and longjmp save and restore the caller's registers, so the host's
// cannot stand in for them: an arm64 library's imports of these names are
// bound to marker addresses (guest_intrinsic) that host_call handles here.
// The jmp_buf layout is private to the pair; bionic's arm64 jmp_buf has room
// for 32 words and this uses 22: x19..x30, sp, d8..d15, and a check word.

enum Intrinsic { kSetjmp, kSigsetjmp, kLongjmp, kIntrinsicCount };
const uint8_t g_intrinsics[kIntrinsicCount] = {};
constexpr uint64_t kJmpBufMagic = 0x77696e61706b6a62ull;  // "winapkjb"

bool run_intrinsic(Cpu& c, uint64_t target) {
    const uint64_t base = reinterpret_cast<uint64_t>(g_intrinsics);
    if (target < base || target >= base + kIntrinsicCount) return false;
    uint64_t* buf = reinterpret_cast<uint64_t*>(c.x[0]);
    switch (static_cast<Intrinsic>(target - base)) {
        case kSetjmp:
        case kSigsetjmp:  // the signal mask is not saved: the guest has no signals
            std::memcpy(buf, &c.x[19], 12 * sizeof(uint64_t));
            buf[12] = c.sp;
            for (int i = 0; i < 8; ++i) buf[13 + i] = c.v[8 + i].d[0];
            buf[21] = kJmpBufMagic ^ c.sp;
            c.x[0] = 0;
            c.pc = c.x[30];
            break;
        default: {  // longjmp, _longjmp, siglongjmp
            if (buf[21] != (kJmpBufMagic ^ buf[12])) fault(c, 0, "longjmp to a jmp_buf setjmp did not fill");
            const uint64_t value = uint32_t(c.x[1]);
            std::memcpy(&c.x[19], buf, 12 * sizeof(uint64_t));
            c.sp = buf[12];
            for (int i = 0; i < 8; ++i) {
                c.v[8 + i] = VReg{};
                c.v[8 + i].d[0] = buf[13 + i];
            }
            c.x[0] = value ? value : 1;
            c.pc = c.x[30];
            break;
        }
    }
    return true;
}

// ---- guest -> host ----------------------------------------------------------------

void host_call(Cpu& c, uint64_t target) {
    if (target == 0) fault(c, 0, "call through a null function pointer");
    if (run_intrinsic(c, target)) return;
    SysvCallFrame f;
    uint64_t gpr[8];
    std::memcpy(gpr, c.x, sizeof gpr);
    SysvVaStorage va;
    const int va_index = va_list_index(target);
    if (va_index >= 0 && va_index < 8) {
        convert_va_list(reinterpret_cast<const AapcsVaList*>(gpr[va_index]), &va);
        gpr[va_index] = reinterpret_cast<uint64_t>(&va.va);
    }
    std::memcpy(f.gpr, gpr, sizeof f.gpr);
    for (int i = 0; i < 8; ++i) f.fpr[i] = c.v[i].d[0];
    uint64_t stack[2 + kGuestStackWords];
    stack[0] = gpr[6];
    stack[1] = gpr[7];
    std::memcpy(stack + 2, reinterpret_cast<const void*>(c.sp), sizeof(uint64_t) * kGuestStackWords);
    f.stack = stack;
    f.nstack = 2 + kGuestStackWords;
    const uint64_t lr = c.x[30];
    winapk_arm64_call_sysv(reinterpret_cast<const void*>(target), &f);
    c.x[0] = f.rax;
    c.x[1] = f.rdx;
    c.v[0] = VReg{};
    c.v[0].d[0] = f.xmm0;
    c.v[1] = VReg{};
    c.v[1].d[0] = f.xmm1;
    c.pc = lr;
}

// ---- host -> guest ----------------------------------------------------------------

void run_guest(ThreadState& ts, uint64_t pc, const uint64_t gpr[8], const uint64_t fpr[8], const uint64_t* stack_args,
               size_t nstack, uint64_t* x0, uint64_t* x1, uint64_t* d0) {
    Cpu& c = ts.cpu;
    const bool nested = ts.depth > 0;
    std::unique_ptr<Cpu> saved;
    if (nested) saved.reset(new Cpu(c));
    uint64_t sp = nested ? c.sp : ts.stack_top;
    if (nstack) {
        sp = (sp - 8 * nstack) & ~uint64_t{15};
        std::memcpy(reinterpret_cast<void*>(sp), stack_args, 8 * nstack);
    }
    c.sp = sp;
    std::memcpy(c.x, gpr, 8 * sizeof(uint64_t));
    for (int i = 0; i < 8; ++i) {
        c.v[i] = VReg{};
        c.v[i].d[0] = fpr ? fpr[i] : 0;
    }
    c.x[30] = c.stop_pc = return_address();
    c.pc = pc;
    c.excl_valid = false;
    ++ts.depth;
    execute(ts);
    --ts.depth;
    *x0 = c.x[0];
    if (x1) *x1 = c.x[1];
    if (d0) *d0 = c.v[0].d[0];
    if (saved) c = *saved;
}

PVOID g_veh = nullptr;

// A host call into arm64 code faults on the first instruction fetch, because
// guest text is never mapped executable. Resume in the System V entry stub
// instead, with the target in r10: the call's return address is already on
// the stack and its arguments are still in registers.
LONG CALLBACK execute_fault_handler(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 ||
        r->ExceptionInformation[0] != 8 /* EXCEPTION_EXECUTE_FAULT */) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = ep->ContextRecord;
    if (!is_guest_code(reinterpret_cast<const void*>(ctx->Rip))) return EXCEPTION_CONTINUE_SEARCH;
    ctx->R10 = ctx->Rip;
    ctx->Rip = reinterpret_cast<DWORD64>(&winapk_arm64_sysv_entry);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// The functions of the Bionic shim that take a va_list.
void register_shim_va_list_functions() {
    static const struct {
        const char* name;
        unsigned index;
    } kVaList[] = {
        {"vsnprintf", 3}, {"vsprintf", 2}, {"vasprintf", 2}, {"vfprintf", 2}, {"vprintf", 1},
        {"vdprintf", 2}, {"__vsprintf_chk", 4}, {"__vsnprintf_chk", 5}, {"__android_log_vprint", 3},
        {"vsscanf", 2}, {"vfscanf", 2}, {"vscanf", 1}, {"vsyslog", 2},
    };
    for (const auto& f : kVaList) {
        if (void* p = mini_bionic_resolve(f.name)) register_va_list_function(p, f.index);
    }
}

}  // namespace

}  // namespace winapk::arm64

extern "C" WINAPK_GUEST_ABI void winapk_arm64_dispatch(uint64_t pc, CallbackFrame* frame) {
    using namespace winapk::arm64;
    ThreadState& ts = thread_state();
    uint64_t gpr[8];
    std::memcpy(gpr, frame->gpr, sizeof frame->gpr);
    gpr[6] = frame->stack_args[0];
    gpr[7] = frame->stack_args[1];
    run_guest(ts, pc, gpr, frame->fpr, frame->stack_args + 2, kCallbackStackWords, &frame->rax, &frame->rdx,
              &frame->xmm0);
}

namespace winapk::arm64 {

void init() {
    static const bool done = [] {
        Hooks h;
        h.code_range = code_range;
        h.host_call = host_call;
        h.svc = svc;
        h.fault = fault;
        set_hooks(h);
        g_veh = AddVectoredExceptionHandler(1, execute_fault_handler);
        register_shim_va_list_functions();
        return true;
    }();
    (void)done;
}

void register_code(const void* start, size_t size) {
    init();
    const uint64_t lo = reinterpret_cast<uint64_t>(start);
    AcquireSRWLockExclusive(&g_ranges_lock);
    g_ranges.push_back({lo, lo + size});
    ReleaseSRWLockExclusive(&g_ranges_lock);
}

void unregister_code(const void* start) {
    const uint64_t lo = reinterpret_cast<uint64_t>(start);
    AcquireSRWLockExclusive(&g_ranges_lock);
    g_ranges.erase(std::remove_if(g_ranges.begin(), g_ranges.end(), [&](const Range& r) { return r.lo == lo; }),
                   g_ranges.end());
    ReleaseSRWLockExclusive(&g_ranges_lock);
}

bool is_guest_code(const void* p) {
    uint64_t lo, hi;
    return code_range(reinterpret_cast<uint64_t>(p), &lo, &hi);
}

void* guest_intrinsic(const char* name) {
    static const struct {
        const char* name;
        Intrinsic kind;
    } kNames[] = {
        {"setjmp", kSetjmp}, {"_setjmp", kSetjmp}, {"sigsetjmp", kSigsetjmp},
        {"longjmp", kLongjmp}, {"_longjmp", kLongjmp}, {"siglongjmp", kLongjmp},
    };
    for (const auto& n : kNames) {
        if (std::strcmp(n.name, name) == 0) return const_cast<uint8_t*>(&g_intrinsics[n.kind]);
    }
    return nullptr;
}

bool set_engine(Engine e) {
    if (e == Engine::kJit && !jit_built()) return false;
    g_engine.store(e);
    return true;
}

Engine engine() { return g_engine.load(); }

uint64_t call(const void* fn, const uint64_t* args, size_t nargs) {
    init();
    uint64_t gpr[8] = {};
    if (nargs > 8) nargs = 8;
    if (nargs) std::memcpy(gpr, args, 8 * nargs);
    uint64_t x0 = 0;
    run_guest(thread_state(), reinterpret_cast<uint64_t>(fn), gpr, nullptr, nullptr, 0, &x0, nullptr, nullptr);
    return x0;
}

void* host_callable(void* fn) {
    if (!is_guest_code(fn)) return fn;
    static std::mutex lock;
    static std::unordered_map<void*, void*> entries;
    std::lock_guard<std::mutex> hold(lock);
    auto it = entries.find(fn);
    if (it != entries.end()) return it->second;
    void* entry = make_r10_jump(reinterpret_cast<void*>(&winapk_arm64_sysv_entry), reinterpret_cast<uint64_t>(fn));
    if (!entry) return fn;  // the fault path still works
    entries.emplace(fn, entry);
    return entry;
}

void register_va_list_function(const void* host_fn, unsigned index) {
    AcquireSRWLockExclusive(&g_va_lock);
    g_va_functions[reinterpret_cast<uint64_t>(host_fn)] = index;
    ReleaseSRWLockExclusive(&g_va_lock);
}

}  // namespace winapk::arm64
