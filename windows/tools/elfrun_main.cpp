// elfrun_main.cpp -- winapk-elfrun, the ELF loader's check program (the
// project's first proof of concept, still run by CI).
//
// Loads Android x86_64 shared objects with the Win.apk ELF loader and checks
// what has to hold before anything bigger is built on top of it:
//
//   * the loader maps, relocates (RELA, RELR, APS2) and initializes a .so;
//   * System V calls cross the boundary both ways: host -> guest with int,
//     double and stack-passed arguments, guest -> host shim, and host -> guest
//     callbacks (qsort comparator);
//   * the patched %fs:0x28 stack-guard loads hold on the main thread and on
//     fresh threads, with and without a guard value installed, and a guard
//     load in code the guest writes at run time is patched when it faults.
//
//   winapk-elfrun <libwinapk_poc_guest.so> [<another build of it> ...]
//   winapk-elfrun --load-only <any x86_64 .so>   load it and report what is missing
//
// Exit code: the number of failed checks.

#include "winapk/elf_loader.h"
#include "winapk/fs_tls_patch.h"
#include "winapk/guest_abi.h"
#include "winapk/mini_bionic.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>

namespace {

int g_failures = 0;

void check(const char* what, bool ok, const std::string& detail) {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ",
                detail.c_str());
    if (!ok) ++g_failures;
}

template <typename T>
void expect_eq(const char* what, T got, T want) {
    std::string detail = "got " + std::to_string(got);
    if (got != want) detail += ", want " + std::to_string(want);
    check(what, got == want, detail);
}

// Guest entry points, called with the System V convention.
using AddFn = int(WINAPK_GUEST_ABI*)(int, int);
using ScaleFn = double(WINAPK_GUEST_ABI*)(double, int);
using ManyArgsFn = long(WINAPK_GUEST_ABI*)(long, long, long, long, long, long, long, long,
                                           double, double, double, double);
using IntFromIntFn = int(WINAPK_GUEST_ABI*)(int);
using IntFn = int(WINAPK_GUEST_ABI*)();
using WrittenGuardFn = int(WINAPK_GUEST_ABI*)(long long*);
using HostCallback = int(WINAPK_GUEST_ABI*)(int);
using CallbackFn = int(WINAPK_GUEST_ABI*)(HostCallback, int);

// A host function the guest calls through a plain C function pointer.
WINAPK_GUEST_ABI int host_triple(int v) { return v * 3; }

struct ThreadProbe {
    IntFromIntFn stack_guard;
    bool install_guard;
    uint64_t guard;
    int result;
};

DWORD WINAPI stack_guard_thread(LPVOID p) {
    auto* probe = static_cast<ThreadProbe*>(p);
    if (probe->install_guard) winapk::set_guest_stack_guard_for_current_thread(probe->guard);
    probe->result = probe->stack_guard(1);
    return 0;
}

int run_on_new_thread(IntFromIntFn fn, bool install_guard, uint64_t guard) {
    ThreadProbe probe{fn, install_guard, guard, -1};
    HANDLE t = CreateThread(nullptr, 0, stack_guard_thread, &probe, 0, nullptr);
    if (!t) return -2;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return probe.result;
}

template <typename Fn>
Fn sym(const winapk::ElfImage& img, const char* name) {
    void* p = img.dlsym(name);
    if (!p) check(name, false, "symbol not exported");
    return reinterpret_cast<Fn>(p);
}

uint64_t fresh_guard() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (static_cast<uint64_t>(t.QuadPart) * 0x9e3779b97f4a7c15ull) & ~0xffull;  // low byte 0, as Bionic
}

// commit_guest_stack_for_current_thread on a fresh 16 MB thread (the JVM's
// -Xss16m). The pages 512 KB under StackLimit, which a guest frame over a page
// writes without probing, must end up committed. With `guard_pages` set the
// thread first lays its stack out as Windows does part way down, whatever the
// system did (Wine commits a thread's whole stack): a reserved run, then a
// guard region of that many pages (x64 Windows keeps more than one), then the
// committed part, so both shapes run under Wine and on Windows alike.
struct StackCommitProbe {
    int guard_pages;
    size_t moved;
    bool deep_committed;
};

DWORD WINAPI stack_commit_thread(LPVOID p) {
    auto* probe = static_cast<StackCommitProbe*>(p);
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    constexpr uintptr_t kPage = 4096;
    if (probe->guard_pages > 0) {
        ULONG_PTR low = 0, high = 0;
        GetCurrentThreadStackLimits(&low, &high);
        volatile char here = 0;
        const uintptr_t limit = (reinterpret_cast<uintptr_t>(&here) - 64 * 1024) & ~(kPage - 1);
        const uintptr_t guard = limit - probe->guard_pages * kPage;
        const uintptr_t floor = low + 64 * 1024;  // left as it is: the JVM's zones would be here
        VirtualFree(reinterpret_cast<void*>(floor), guard - floor, MEM_DECOMMIT);
        VirtualAlloc(reinterpret_cast<void*>(guard), limit - guard, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD);
        tib->StackLimit = reinterpret_cast<void*>(limit);
    }
    const uintptr_t deep = reinterpret_cast<uintptr_t>(tib->StackLimit) - 512 * 1024;
    probe->moved = winapk::commit_guest_stack_for_current_thread();
    MEMORY_BASIC_INFORMATION mbi{};
    probe->deep_committed = VirtualQuery(reinterpret_cast<void*>(deep), &mbi, sizeof mbi) &&
                            mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD);
    if (probe->deep_committed) *reinterpret_cast<volatile char*>(deep) = 1;  // a frame's write, unprobed
    return 0;
}

void run_stack_commit_checks() {
    for (int guard_pages : {0, 1, 3}) {
        StackCommitProbe probe{guard_pages, 0, false};
        HANDLE t = CreateThread(nullptr, 16 * 1024 * 1024, stack_commit_thread, &probe,
                                STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!t) {
            check("guest stack commit thread", false, "CreateThread failed");
            continue;
        }
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        char what[96], detail[96];
        if (guard_pages) {
            std::snprintf(what, sizeof what, "guest stack committed ahead, %d-page guard region", guard_pages);
        } else {
            std::snprintf(what, sizeof what, "guest stack committed ahead, the system's layout");
        }
        std::snprintf(detail, sizeof detail, "StackLimit moved %zu KB, 512 KB below it %s", probe.moved / 1024,
                      probe.deep_committed ? "committed" : "only reserved");
        check(what, probe.deep_committed && (guard_pages == 0 || probe.moved >= 900 * 1024), detail);
    }
}

void run_poc_checks(const winapk::ElfImage& img) {
    auto add = sym<AddFn>(img, "winapk_test_add");
    auto scale = sym<ScaleFn>(img, "winapk_test_scale");
    auto many = sym<ManyArgsFn>(img, "winapk_test_many_args");
    auto relocs = sym<IntFromIntFn>(img, "winapk_test_relocs");
    auto add_ptr = static_cast<void* const*>(img.dlsym("winapk_test_add_ptr"));
    auto ctor_ran = sym<IntFn>(img, "winapk_test_ctor_ran");
    auto libc = sym<IntFn>(img, "winapk_test_libc");
    auto stack_guard = sym<IntFromIntFn>(img, "winapk_test_stack_guard");
    auto qsort_test = sym<IntFn>(img, "winapk_test_qsort");
    auto callback = sym<CallbackFn>(img, "winapk_test_callback");
    auto log = sym<IntFn>(img, "winapk_test_log");
    if (!add || !scale || !many || !relocs || !ctor_ran || !libc || !stack_guard || !qsort_test ||
        !callback || !log) {
        return;
    }

    expect_eq("int args (add 2+40)", add(2, 40), 42);
    {
        const double got = scale(1.5, 4);
        check("double arg + double return (1.5 * 4)", std::fabs(got - 6.0) < 1e-12,
              "got " + std::to_string(got));
    }
    // 1+2*2+..+8*8 = 204, plus 0.5*10 + 0.25*100 + 0.125*1000 + 0.0625*10000 = 780
    expect_eq("8 int args (2 on the stack) + 4 doubles", many(1, 2, 3, 4, 5, 6, 7, 8, 0.5, 0.25, 0.125, 0.0625), 984L);
    expect_eq("RELATIVE + R_X86_64_64 relocations", relocs(5), 143);
    check("R_X86_64_64 data slot holds the exported function",
          add_ptr && *add_ptr == img.dlsym("winapk_test_add"), "");
    expect_eq("DT_INIT_ARRAY constructor ran", ctor_ran(), 0x1234);
    expect_eq("malloc/memcpy/strlen/strcmp/puts/free via the shim", libc(), 37);

    // sum of 1..64 = 2080
    expect_eq("stack guard, main thread", stack_guard(1), 2080);
    expect_eq("stack guard, new thread, slot never set", run_on_new_thread(stack_guard, false, 0), 2080);
    expect_eq("stack guard, new thread, own guard", run_on_new_thread(stack_guard, true, fresh_guard()), 2080);

    if (auto written = sym<WrittenGuardFn>(img, "winapk_test_written_guard")) {
        // gs:[0x28] holds a value of its own here (main() installs one).
        const size_t fixed = winapk::fs_stack_guard_sites_fixed_at_run_time();
        long long guard = 0;
        const int patched = written(&guard);
        if (patched == 0) {
            // Unpatched and no fault: FS has a base here (Wine on Linux points
            // it at the host thread's TLS), so there was nothing to fix. On
            // Windows the load faults and is patched, or the process dies.
            std::printf("  [SKIP] stack guard in code written at run time: FS reads do not fault here\n");
        } else {
            expect_eq("stack guard in code written at run time, patched as it faults", patched, 1);
            expect_eq("  it read gs:[0x28]", guard, static_cast<long long>(winapk::read_guest_stack_guard_slot()));
            expect_eq("  one load patched by the fault handler",
                      winapk::fs_stack_guard_sites_fixed_at_run_time() - fixed, static_cast<size_t>(1));
        }
    }

    expect_eq("guest comparator called by host qsort", qsort_test(), 188);
    expect_eq("host callback called by the guest (3*20 + 3*21)", callback(host_triple, 20), 123);
    expect_eq("__android_log_write via the shim", log(), 1);
}

}  // namespace

int main(int argc, char** argv) {
    bool load_only = false;
    int first = 1;
    if (argc > 1 && std::strcmp(argv[1], "--load-only") == 0) {
        load_only = true;
        first = 2;
    }
    if (first >= argc) {
        std::fprintf(stderr, "usage: winapk-elfrun [--load-only] <android-x86_64.so> [...]\n");
        return 2;
    }

    std::printf("Win.apk loader PoC\n");
    run_stack_commit_checks();
    {
        const uint64_t v = 0x00c0ffee5157a100ull;
        winapk::set_guest_stack_guard_for_current_thread(v);
        const uint64_t seen = winapk::read_guest_stack_guard_slot();
        char detail[96];
        std::snprintf(detail, sizeof(detail), "wrote 0x%" PRIx64 ", gs:[0x28] reads 0x%" PRIx64, v, seen);
        check("NT_TIB.ArbitraryUserPointer is gs:[0x28]", seen == v, detail);
        winapk::set_guest_stack_guard_for_current_thread(fresh_guard());
    }

    for (int i = first; i < argc; ++i) {
        std::printf("\n%s\n", argv[i]);
        std::string error;
        auto img = winapk::ElfImage::load(argv[i], winapk::mini_bionic_resolve, &error);
        check("load", img != nullptr, img ? "" : error);
        if (!img) continue;
        std::printf("  soname=%s bias=0x%" PRIxPTR " size=%zu relocations=%zu fs-patched=%zu\n",
                    img->soname().c_str(), img->load_bias(), img->mapped_size(),
                    img->relocations_applied(), img->fs_sites_patched());
        for (const std::string& n : img->needed()) std::printf("  needs %s\n", n.c_str());
        if (load_only) continue;
        check("stack-guard loads patched", img->fs_sites_patched() > 0,
              std::to_string(img->fs_sites_patched()) + " site(s)");
        run_poc_checks(*img);
    }

    winapk::mini_bionic_run_atexit();
    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures;
}
