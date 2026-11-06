// arm64_test.cpp -- winapk-arm64-test: the arm64 engines and their bridge to
// the Windows host.
//
//   winapk-arm64-test <dir with libwinapk_arm64_*.so>
//
// Two kinds of checks:
//
//   * Differential: test/guest/arm64_isa.c is compiled into this program and,
//     three times (-O2, -O3, -O3 with the ARMv8.1 atomics), into arm64 guest
//     libraries. Every function runs both ways on the same inputs and the
//     results must match bit for bit: the native build is the reference for
//     the interpreter.
//
//   * The bridge, with test/guest/arm64_bridge_guest.c: libc calls into the
//     Bionic shim, variadic calls and the guest's own va_list, callbacks from
//     the host into guest code (qsort, pthread_create, __cxa_atexit, nested
//     guest -> host -> guest -> host), calls in both directions with stack
//     arguments and floating point, constructors, the stack protector, and
//     setjmp/longjmp.
//
//   * Control flow and edge cases, with test/guest/arm64_flow_guest.c:
//     returns into an ancestor frame (as an unwinder or longjmp written in
//     guest code makes), coroutines, RETAA, a return through BR, deep
//     recursion on a 1 MB thread, saturating conversions and FRECPS/FRSQRTS,
//     an exclusive-monitor counter shared by translated code and the JIT,
//     the store-buffer litmus test; with arm64_patch_guest.c, a library
//     patching its own code, and a library unloaded (dlclose) while
//     translated. The expected values are
//     an arm64 CPU's, worked out here, not the host compiler's.
//
//   * Speed, with test/guest/arm64_bench.c (built into this program too):
//     each benchmark natively and on each engine, the JIT's translation cost,
//     and where its time goes (arm64_guest.h, Stats).
//
// Exit code: the number of failed checks.

#include "winapk/aot_files.h"
#include "winapk/arm64_aot.h"
#include "winapk/arm64_guest.h"
#include "winapk/guest_abi.h"
#include "winapk/linker.h"
#include "winapk/mini_bionic.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#define ISA_EXPORT static __attribute__((noinline, used))
#include "arm64_isa.c"
#define BENCH_HOST
#define BENCH_EXPORT static __attribute__((noinline, used))
#include "arm64_bench.c"

namespace {

int g_failures = 0;

void check(const char* what, bool ok, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++g_failures;
}

std::string hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%" PRIx64, v);
    return buf;
}

void* sym(winapk::GuestLinker& linker, void* lib, const char* name) {
    std::string error;
    void* p = linker.lookup(lib, name, nullptr, &error);
    if (!p) check(name, false, error);
    return p;
}

const uint64_t kInputs[][3] = {
    {0, 0, 0},
    {1, 2, 3},
    {0xffffffffffffffffull, 1, 0x8000000000000000ull},
    {12345678901234ull, 987654321ull, 42},
    {0x7fffffff, 0x80000000, 0xffffffff},
    {0xdeadbeefcafebabeull, 0x0123456789abcdefull, 0xfedcba9876543210ull},
    {17, 0xffffffffffffffffull, 5},
    {1000000, 3, 7},
};

using IsaFn = u64 (*)(u64, u64, u64);
using GuestIsaFn = u64(WINAPK_GUEST_ABI*)(u64, u64, u64);

void run_isa(winapk::GuestLinker& linker, const char* file) {
    std::printf("%s\n", file);
    std::string error;
    void* lib = linker.open(file, winapk::GuestLinker::kRtldNow, &error);
    check("open", lib != nullptr, error);
    if (!lib) return;
    struct Case {
        const char* name;
        IsaFn native;
    };
#define ISA_CASE(name) {#name, &name},
    static const Case kCases[] = {ISA_TESTS(ISA_CASE)};
#undef ISA_CASE
    for (const Case& c : kCases) {
        void* fn = sym(linker, lib, c.name);
        if (!fn) continue;
        std::string first_mismatch;
        int mismatches = 0;
        for (const auto& in : kInputs) {
            const uint64_t want = c.native(in[0], in[1], in[2]);
            const uint64_t got = winapk::arm64::call(fn, in, 3);
            if (got != want) {
                if (!mismatches)
                    first_mismatch = "(" + hex(in[0]) + ", " + hex(in[1]) + ", " + hex(in[2]) + "): native " +
                                     hex(want) + ", arm64 " + hex(got);
                ++mismatches;
            }
        }
        check(c.name, mismatches == 0, first_mismatch);
    }
    // The same code entered the two other ways the host reaches guest code.
    auto* fn = reinterpret_cast<GuestIsaFn>(sym(linker, lib, "isa_loop"));
    if (fn) {
        const uint64_t want = isa_loop(5, 6, 300);
        check("a System V call into arm64 code (execute-fault path)", fn(5, 6, 300) == want);
        auto* entry = reinterpret_cast<GuestIsaFn>(winapk::arm64::host_callable(reinterpret_cast<void*>(fn)));
        check("the same call through host_callable()", entry != fn && entry(5, 6, 300) == want);
    }
}

WINAPK_GUEST_ABI uint64_t missing_function() { return 0; }

// The host function arm64_call_host calls (System V, like every shim function).
WINAPK_GUEST_ABI int64_t host_many_args(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g,
                                        int64_t h, int64_t i, int64_t j, double x, double y) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j +
           static_cast<int64_t>(x * 100 + y * 1000);
}

void run_bridge(winapk::GuestLinker& linker, const char* file) {
    std::printf("%s\n", file);
    std::string error;
    void* lib = linker.open(file, winapk::GuestLinker::kRtldNow, &error);
    check("open (APS2 + RELR relocations, stack protector)", lib != nullptr, error);
    if (!lib) return;
    auto fn = [&](const char* name) { return sym(linker, lib, name); };
    // For functions returning int: AAPCS64 leaves the upper half of x0 undefined.
    auto call0 = [&](const char* name) -> int {
        void* f = fn(name);
        return f ? static_cast<int>(winapk::arm64::call(f)) : -999999;
    };

    check("constructor ran through the interpreter", call0("arm64_ctor_ran") == 0x5150);
    {
        const uint64_t seed = 3;
        int want = 0;
        for (int i = 0; i < 100; ++i) want += static_cast<signed char>(seed + i);
        check("stack-protected function (guard in TPIDR_EL0[5])",
              static_cast<int>(winapk::arm64::call(fn("arm64_stack_guard"), &seed, 1)) == want);
    }
    check("libc through the shim (malloc, memcpy, strlen, strcmp, errno)", call0("arm64_libc") == 36);

    char buf[256] = {};
    uint64_t args[2] = {reinterpret_cast<uint64_t>(buf), sizeof buf};
    winapk::arm64::call(fn("arm64_snprintf"), args, 2);
    const char* want_printf = "1 two 3 4.50 5 6 7f 8 9.3 10 eleven 12";
    check("variadic snprintf with stack arguments", std::strcmp(buf, want_printf) == 0, buf);
    std::memset(buf, 0, sizeof buf);
    winapk::arm64::call(fn("arm64_vsnprintf"), args, 2);
    const char* want_v = "va|1|2.500|3|4|5|6|7|8|9.5|end";
    check("the guest's AAPCS64 va_list given to vsnprintf", std::strcmp(buf, want_v) == 0, buf);
    check("__android_log_print", call0("arm64_log_print") > 0);

    check("guest comparator called by the host's qsort", call0("arm64_qsort") == -3912);
    check("guest comparator called by the host's bsearch", call0("arm64_bsearch") == 325);
    check("guest -> host -> guest -> host (qsort comparator calling snprintf)", call0("arm64_nested") == 10004);
    int64_t want_threads = 0;
    for (int64_t n = 100; n < 104; ++n) want_threads += n * (n + 1) * (2 * n + 1) / 6;
    const int64_t threads = static_cast<int64_t>(winapk::arm64::call(fn("arm64_threads")));
    check("pthread_create with a guest start routine, four threads", threads == want_threads,
          std::to_string(threads));

    using ManyFn = int64_t(WINAPK_GUEST_ABI*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                                               int64_t, int64_t, double, double);
    if (auto* many = reinterpret_cast<ManyFn>(fn("arm64_many_args"))) {
        check("host -> guest: 10 integer + 2 double arguments", many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0.5, 0.25) == 685);
    }
    using JniSizedFn = int64_t(WINAPK_GUEST_ABI*)(
        int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
        int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
        int64_t, int64_t, int64_t, const int64_t*, const int64_t*);
    if (auto* jni_sized = reinterpret_cast<JniSizedFn>(fn("arm64_jni_sized"))) {
        // Each argument i is i + 1, so the sum is the sum of (i + 1)^2 for i < 29.
        const int64_t a27 = 28, a28 = 29;
        check("host -> guest: 29 integer arguments, 21 on the stack (Flutter's nativeSetViewportMetrics)",
              jni_sized(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
                        27, &a27, &a28) == 8555);
    }
    using ScaleFn = double(WINAPK_GUEST_ABI*)(double, float, int);
    if (auto* scale = reinterpret_cast<ScaleFn>(fn("arm64_scale"))) {
        check("host -> guest: double and float arguments, double result", scale(2.5, 4.0f, 3) == 30.0);
    }
    const uint64_t host_fn = reinterpret_cast<uint64_t>(&host_many_args);
    check("guest -> host: 10 integer + 2 double arguments",
          static_cast<int64_t>(winapk::arm64::call(fn("arm64_call_host"), &host_fn, 1)) == 685);
    using ParseFn = double(WINAPK_GUEST_ABI*)(const char*);
    if (auto* parse = reinterpret_cast<ParseFn>(fn("arm64_parse"))) {
        check("double result from the host (strtod) back to the host", parse("1.25") == 2.5);
    }

    check("setjmp/longjmp across three guest frames", call0("arm64_setjmp") == 421239,
          std::to_string(call0("arm64_setjmp")));

    const int exits_before = call0("arm64_exit_count");
    check("__cxa_atexit with a guest handler", call0("arm64_register_atexit") == 0);
    winapk::mini_bionic_run_atexit();
    check("the handler ran from the host's atexit list", call0("arm64_exit_count") == exits_before + 7);
}

// ---- arm64_flow_guest.c -------------------------------------------------------------

// What an arm64 CPU's FPToFixed gives for an already rounded value: NaN is 0,
// out of range saturates (C leaves both undefined, and x86 differs).
int64_t arm_sat_s(double r, int bits) {
    if (std::isnan(r)) return 0;
    const double hi = std::ldexp(1.0, bits - 1);
    if (r >= hi) return static_cast<int64_t>((uint64_t{1} << (bits - 1)) - 1);
    if (r < -hi) return -static_cast<int64_t>(uint64_t{1} << (bits - 2)) * 2;
    return static_cast<int64_t>(r);
}
uint64_t arm_sat_u(double r, int bits) {
    if (std::isnan(r) || r <= 0) return 0;
    if (r >= std::ldexp(1.0, bits)) return bits == 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1;
    return static_cast<uint64_t>(r);
}
// Round to nearest, ties away from zero, exactly (some C runtimes' round()
// is not: Wine's msvcrt rounds 0.49999999999999994 to 1).
template <typename F> F round_away(F v) {
    const F t = std::trunc(v);
    return std::fabs(v - t) >= F(0.5) ? t + std::copysign(F(1), v) : t;
}
uint32_t float_bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// The results of flow_fp_convert for one input, in its order.
std::vector<int64_t> want_convert(double d) {
    const float f = static_cast<float>(d);
    std::vector<int64_t> w;
    w.push_back(arm_sat_s(std::trunc(d), 64));
    w.push_back(static_cast<int32_t>(arm_sat_s(std::trunc(d), 32)));
    w.push_back(static_cast<int64_t>(arm_sat_u(std::trunc(d), 64)));
    w.push_back(static_cast<int64_t>(arm_sat_u(std::trunc(f), 32)));
    w.push_back(arm_sat_s(std::nearbyint(d), 64));
    w.push_back(arm_sat_s(round_away(d), 64));
    w.push_back(static_cast<int32_t>(arm_sat_s(std::floor(f), 32)));
    w.push_back(static_cast<int32_t>(arm_sat_s(std::ceil(f), 32)));
    for (double r : {std::nearbyint(d), round_away(d), std::floor(d), std::ceil(d), std::trunc(d), std::nearbyint(d)}) {
        w.push_back(arm_sat_s(std::trunc(r * 4), 64));  // (int64_t)(r * 4) is FCVTZS
    }
    return w;
}

std::vector<uint32_t> want_vector(const float* in) {
    std::vector<uint32_t> w;
    for (int i = 0; i < 4; ++i) w.push_back(static_cast<uint32_t>(arm_sat_s(std::trunc(in[i]), 32)));
    for (int i = 0; i < 4; ++i) w.push_back(static_cast<uint32_t>(arm_sat_u(std::trunc(in[i]), 32)));
    for (int i = 0; i < 4; ++i) w.push_back(static_cast<uint32_t>(arm_sat_s(std::nearbyint(in[i]), 32)));
    for (int i = 0; i < 4; ++i) w.push_back(float_bits(round_away(in[i])));
    for (int i = 0; i < 4; ++i) w.push_back(float_bits(std::nearbyint(in[i])));
    return w;
}

uint64_t deep_reference(uint64_t n) {
    uint64_t r = 1;
    for (uint64_t i = 1; i <= n; ++i) r = r * 3 + i;
    return r;
}

void run_flow(winapk::GuestLinker& linker, const char* file) {
    std::printf("%s\n", file);
    std::string error;
    void* lib = linker.open(file, winapk::GuestLinker::kRtldNow, &error);
    check("open", lib != nullptr, error);
    if (!lib) return;
    auto call = [&](const char* name, std::vector<uint64_t> args = {}) -> uint64_t {
        void* f = sym(linker, lib, name);
        return f ? winapk::arm64::call(f, args.data(), args.size()) : ~uint64_t{0};
    };
    auto expect = [&](const char* what, uint64_t got, uint64_t want) {
        check(what, got == want, got == want ? "" : "got " + hex(got) + ", want " + hex(want));
    };

    // acc goes 1, 12, 88, 619, 4335, 30346, 212422 down the six frames; x19
    // and the volatile local come back as they were at the save.
    expect("a return with RET into an ancestor frame (an unwinder's jump)", call("flow_unwind_ret"), 212422010006);
    expect("the same with BR", call("flow_unwind_br"), 212422010006);
    {
        uint64_t local = 100, acc = 0;
        for (uint64_t r = 0; r < 1000; ++r) {
            local = local * 3 + r;
            acc = acc * 31 + local;
        }
        expect("coroutines switching at one call site on their own stacks", call("flow_coroutines", {1000}), acc);
    }
    expect("a variadic guest function: 8 integers and 4 doubles through va_arg", call("flow_variadic"), 1634474);
    expect("a function entered at its start and at a symbol in its middle", call("flow_entries", {1}), 12112011);
    expect("the second entry from the host", call("flow_multi_mid", {1}), 11);
    expect("RETAA from a call", call("flow_call_retaa", {5}), 12);
    expect("RETAA from the host's call", call("flow_retaa", {7}), 8);
    expect("a return through BR x16 to the host's call", call("flow_br_lr", {5}), 7);
    {
        uint64_t acc = 1;
        for (uint64_t i = 0; i < 50; ++i) acc = ((i & 1) ? acc * 5 : acc + 3) + (i + 2) + (i + 1);
        expect("tail calls through a function pointer, BR and RETAA returns in a loop", call("flow_calls", {50}), acc);
    }
    // 20,000 frames of 32 bytes: 640 KB of guest stack, which an Android
    // thread's 1 MB holds. The host's stack must hold it too.
    const uint64_t depth = 20000;
    expect("recursion 20,000 deep", call("flow_deep", {depth}), deep_reference(depth));
    expect("recursion 20,000 deep through a function pointer", call("flow_deep_indirect", {depth}),
           deep_reference(depth));
    expect("both on a guest thread (1 MB stack)", call("flow_deep_thread", {depth}), deep_reference(depth) * 4);

    {
        const double nan = std::nan("");
        const double in[] = {nan, INFINITY, -INFINITY, 3e9, -3e9, 1e20, -1e20, 2.5, -2.5, -0.5, 3.5,
                             4294967295.5, -1.0, 0.49999999999999994, 9.2233720368547758e18};
        const int n = sizeof in / sizeof in[0];
        int64_t out[n * 14] = {};
        const uint64_t args[3] = {reinterpret_cast<uint64_t>(in), static_cast<uint64_t>(n),
                                  reinterpret_cast<uint64_t>(out)};
        void* f = sym(linker, lib, "flow_fp_convert");
        const uint64_t k = f ? winapk::arm64::call(f, args, 3) : 0;
        std::string first;
        int bad = 0;
        for (int i = 0; i < n; ++i) {
            const std::vector<int64_t> want = want_convert(in[i]);
            for (size_t j = 0; j < want.size(); ++j) {
                if (out[i * 14 + j] == want[j]) continue;
                if (!bad++)
                    first = "input " + std::to_string(in[i]) + ", result " + std::to_string(j) + ": got " +
                            hex(static_cast<uint64_t>(out[i * 14 + j])) + ", want " +
                            hex(static_cast<uint64_t>(want[j]));
            }
        }
        check("FCVT{ZS,ZU,NS,AS,MS,PS} saturation and FRINT{N,A,M,P,Z,X} (scalar)", k == uint64_t(n) * 14 && !bad,
              first);
    }
    {
        const float sets[2][4] = {{std::nanf(""), INFINITY, -INFINITY, 5e9f}, {2.5f, -2.5f, -0.5f, 3.5f}};
        std::string first;
        int bad = 0;
        for (const auto& in : sets) {
            uint32_t out[20] = {};
            const uint64_t args[2] = {reinterpret_cast<uint64_t>(in), reinterpret_cast<uint64_t>(out)};
            void* f = sym(linker, lib, "flow_fp_vector");
            if (f) winapk::arm64::call(f, args, 2);
            const std::vector<uint32_t> want = want_vector(in);
            for (size_t j = 0; j < want.size(); ++j) {
                if (out[j] != want[j] && !bad++) {
                    first = "result " + std::to_string(j) + ": got " + hex(out[j]) + ", want " + hex(want[j]);
                }
            }
        }
        check("FCVTZS, FCVTZU, FCVTNS, FRINTA, FRINTN (vector) on NaN, infinities, halves", !bad, first);
    }
    {
        // b * a for lane 3 is 1 + 2^-11 + 2^-24: fused, 2 - that is 1 - 2^-11 - 2^-24.
        const float a[4] = {0.0f, INFINITY, 2.0f, 1.000244140625f};
        const float b[4] = {INFINITY, 0.0f, 0.75f, 1.000244140625f};
        const uint32_t want[] = {0x40000000, 0x40000000, 0x3f000000, 0x3f7fdfff,  // FRECPS
                                 0x3fc00000, 0x3fc00000, 0x3f400000, 0x3f7ff000,  // FRSQRTS
                                 0x40000000, 0x3fc00000,                          // scalar, 0 * inf
                                 0x00000000, 0x80000000, 0x3f800000, 0x3f800000};  // FMAX FMIN FMAXNM FMINNM
        uint32_t out[14] = {};
        const uint64_t args[3] = {reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b),
                                  reinterpret_cast<uint64_t>(out)};
        void* f = sym(linker, lib, "flow_fp_steps");
        if (f) winapk::arm64::call(f, args, 3);
        std::string first;
        for (size_t j = 0; j < 14 && first.empty(); ++j) {
            if (out[j] != want[j]) {
                first = "result " + std::to_string(j) + ": got " + hex(out[j]) + ", want " + hex(want[j]);
            }
        }
        check("FRECPS/FRSQRTS (fused; 0 times infinity), FMAX/FMIN on zeros, FMAXNM/FMINNM on NaN", first.empty(),
              first);
    }

    expect("LDAXR/STLXR counter: a library's code and a mapped copy on two threads", call("flow_mixed", {200000}),
           400000);
    expect("store-buffer litmus test, STLR then LDAR: never both 0", call("flow_litmus", {100000}), 0);
}

// arm64_eh_guest.cpp: C++ exceptions thrown through translated frames (built
// only where an NDK was found; skipped otherwise).
void run_eh(winapk::GuestLinker& linker, const std::filesystem::path& dir) {
    std::printf("libwinapk_arm64_eh.so\n");
    std::error_code ec;
    if (!std::filesystem::exists(dir / "libwinapk_arm64_eh.so", ec)) {
        std::printf("  (not built: no Android NDK)\n");
        return;
    }
    std::string error;
    void* lib = linker.open("libwinapk_arm64_eh.so", winapk::GuestLinker::kRtldNow, &error);
    check("open (the NDK's libc++ and unwinder inside)", lib != nullptr, error);
    if (!lib) return;
    auto call = [&](const char* name, std::vector<uint64_t> args = {}) -> uint64_t {
        void* f = sym(linker, lib, name);
        return f ? winapk::arm64::call(f, args.data(), args.size()) : ~uint64_t{0};
    };
    auto expect = [&](const char* what, uint64_t got, uint64_t want) {
        check(what, got == want, got == want ? "" : "got " + std::to_string(got) + ", want " + std::to_string(want));
    };
    expect("a throw 40 frames down, their destructors run", call("eh_deep"), 780);
    expect("through a virtual call and a function pointer, rethrown", call("eh_mixed"), 2003313535);
    expect("1,000 throws in a loop", call("eh_loop", {1000}), 3500);
}

// libwinapk_arm64_rebased.so is libwinapk_arm64_unload.so linked 2 MB
// higher: the same code bytes at other addresses. Given the other's
// translation, the loader must refuse it (text_hash takes in the addresses),
// or the functions would be looked up 2 MB off.
void run_rebased(winapk::GuestLinker& linker, const std::filesystem::path& dir) {
    std::printf("libwinapk_arm64_rebased.so\n");
    const std::filesystem::path theirs = dir / "libwinapk_arm64_unload.so.aot.dll";
    std::error_code ec;
    if (!std::filesystem::exists(theirs, ec)) {
        std::printf("  (no translation of libwinapk_arm64_unload.so: skipped)\n");
        return;
    }
    const std::filesystem::path ours = dir / "libwinapk_arm64_rebased.so.aot.dll";
    std::filesystem::remove(ours, ec);  // (MinGW's libstdc++ does not overwrite)
    std::filesystem::copy_file(theirs, ours, ec);
    check("its translation replaced by the other library's", !ec, ec.message());
    std::string error;
    void* lib = linker.open("libwinapk_arm64_rebased.so", winapk::GuestLinker::kRtldNow, &error);
    check("open", lib != nullptr, error);
    if (!lib) return;
    void* fn = sym(linker, lib, "isa_arith");
    if (!fn) return;
    const uint64_t at = reinterpret_cast<uint64_t>(fn);
    check("the other library's translation is refused", !winapk::arm64::aot_lookup(at) &&
                                                             !winapk::arm64::aot_lookup(at - 0x200000));
    const uint64_t in[3] = {1, 2, 3};
    check("runs", winapk::arm64::call(fn, in, 3) == isa_arith(1, 2, 3));
}

// A translation cut short (a copy killed half way, data lost in a power cut)
// under a stamp that says its DLLs are sealed (include/winapk/aot_files.h):
// the loader must leave it alone and the JIT run the library. The same DLL
// whole, with its seal, is loaded: the 32 bytes after the image are nothing
// to LoadLibrary. Copies of libwinapk_arm64_unload.so and its translation.
void run_sealed(winapk::GuestLinker& linker, const std::filesystem::path& dir) {
    const std::filesystem::path so = dir / "libwinapk_arm64_unload.so", dll = dir / "libwinapk_arm64_unload.so.aot.dll";
    std::error_code ec;
    if (!std::filesystem::exists(dll, ec)) {
        std::printf("libwinapk_arm64_torn.so\n  (no translation of libwinapk_arm64_unload.so: skipped)\n");
        return;
    }
    for (const bool whole : {false, true}) {
        const char* file = whole ? "libwinapk_arm64_sealed.so" : "libwinapk_arm64_torn.so";
        std::printf("%s\n", file);
        const std::filesystem::path copy = dir / file, copy_dll = copy.u8string() + ".aot.dll";
        std::filesystem::remove(copy, ec);  // (MinGW's libstdc++ does not overwrite)
        std::filesystem::remove(copy_dll, ec);
        std::filesystem::copy_file(so, copy, ec);
        std::filesystem::copy_file(dll, copy_dll, ec);
        check("sealed", winapk::arm64::aot_seal(copy_dll));
        if (!whole) std::filesystem::resize_file(copy_dll, std::filesystem::file_size(copy_dll, ec) / 2, ec);
        char stamp[128];
        std::snprintf(stamp, sizeof stamp, "full 0 %u 0 0 %llx %llx\n", winapk::arm64::kAotVersion,
                      static_cast<unsigned long long>(winapk::arm64::aot_cpu_signature()),
                      static_cast<unsigned long long>(winapk::arm64::aot_lib_id(copy)));
        winapk::arm64::aot_write_file(copy.u8string() + ".aot.stamp", stamp);
        std::string error;
        void* lib = linker.open(file, winapk::GuestLinker::kRtldNow, &error);
        check("open", lib != nullptr, error);
        if (!lib) continue;
        void* fn = sym(linker, lib, "isa_arith");
        if (!fn) continue;
        const bool translated = winapk::arm64::aot_lookup(reinterpret_cast<uint64_t>(fn)) != nullptr;
        check(whole ? "its sealed translation is loaded" : "its translation cut short is refused", translated == whole);
        const uint64_t in[3] = {1, 2, 3};
        check("runs", winapk::arm64::call(fn, in, 3) == isa_arith(1, 2, 3));
        check("dlclose", linker.close(lib, &error) == 0, error);
    }
}

// arm64_patch_guest.c: a library that patches its own code. Each engine has
// its own copy, as a translation is dropped for good once its library
// writes code (arm64_aot.h, "Code the guest rewrites").
void run_patch(winapk::GuestLinker& linker, const char* file) {
    std::printf("%s\n", file);
    std::string error;
    void* lib = linker.open(file, winapk::GuestLinker::kRtldNow, &error);
    check("open", lib != nullptr, error);
    if (!lib) return;
    void* fn = sym(linker, lib, "flow_patch");
    void* target = sym(linker, lib, "flow_patch_target");
    if (!fn || !target) return;
    const bool aot = winapk::arm64::engine() == winapk::arm64::Engine::kAot;
    if (aot) {
        check("its translation is loaded", winapk::arm64::aot_lookup(reinterpret_cast<uint64_t>(target)) != nullptr);
    }
    const uint64_t got = winapk::arm64::call(fn);
    check("an inline hook: the library patches its own code and calls it", got == 142,
          got == 142 ? "" : "got " + std::to_string(got) + ", want 142");
    if (aot) {
        check("its translation is dropped", winapk::arm64::aot_lookup(reinterpret_cast<uint64_t>(target)) == nullptr);
    }
    if (void* threads = sym(linker, lib, "flow_patch_threads")) {
        check("patched by one thread while another runs it: the other sees the new code",
              winapk::arm64::call(threads) == 1);
    }
}

// A library unloaded with dlclose must take its translation with it: other
// code mapped there later (another library, a JIT's) is not that code.
void run_unload(winapk::GuestLinker& linker, const char* file) {
    std::printf("%s\n", file);
    std::string error;
    void* lib = linker.open(file, winapk::GuestLinker::kRtldNow, &error);
    check("open", lib != nullptr, error);
    if (!lib) return;
    void* fn = sym(linker, lib, "isa_arith");
    if (!fn) return;
    const uint64_t in[3] = {1, 2, 3};
    check("runs", winapk::arm64::call(fn, in, 3) == isa_arith(1, 2, 3));
    const bool translated = winapk::arm64::aot_lookup(reinterpret_cast<uint64_t>(fn)) != nullptr;
    if (winapk::arm64::engine() == winapk::arm64::Engine::kAot) check("its translation is loaded", translated);
    check("dlclose", linker.close(lib, &error) == 0, error);
    check("dlclose drops the translation", winapk::arm64::aot_lookup(reinterpret_cast<uint64_t>(fn)) == nullptr);
}

// ---- the random instruction sequences of gen_arm64_fuzz.py ------------------------------

struct FuzzState {
    uint64_t x[32];
    uint64_t nzcv;
    alignas(16) uint8_t v[32][16];
};
static_assert(offsetof(FuzzState, v) == 272 && sizeof(FuzzState) == 784, "the layout gen_arm64_fuzz.py uses");
constexpr size_t kFuzzScratch = 4096;
constexpr int kFuzzSeeds = 3;

uint64_t splitmix(uint64_t* s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// A start state with the values that find bugs: 0, -1, the extremes, small
// numbers, and in the vector registers floats and doubles that are small,
// negative, zero of either sign, infinite, NaN, denormal or random bits.
void fuzz_input(uint64_t seed, FuzzState* st, uint8_t* scratch) {
    uint64_t s = seed;
    static const uint64_t kInts[] = {0, 1, ~uint64_t{0}, 0x8000000000000000ull, 0x7fffffffffffffffull, 0x80000000,
                                     0x7fffffff, 0xffffffff, 2, 63, 64, 0xff, 0x100};
    for (uint64_t& v : st->x) {
        const uint64_t r = splitmix(&s);
        v = r % 3 == 0 ? kInts[(r >> 8) % (sizeof kInts / sizeof kInts[0])] : r % 3 == 1 ? (r >> 32) % 1000 : r;
    }
    st->nzcv = (splitmix(&s) & 0xf) << 28;
    static const float kFloats[] = {0.0f, -0.0f, 1.0f, -1.5f, 3.25f, 1e9f, 1e-40f, INFINITY, -INFINITY, NAN, 0.5f,
                                    2.5f, -2.5f, 4294967296.0f, 1e20f};
    static const double kDoubles[] = {0.0, -0.0, 1.0, -1.5, 3.25, 1e18, 1e-310, INFINITY, -INFINITY, NAN, 0.5,
                                      2.5, -2.5, 9.3e18, 1e300};
    for (auto& v : st->v) {
        const uint64_t kind = splitmix(&s) % 3;
        for (int lane = 0; lane < 4; ++lane) {
            const uint64_t r = splitmix(&s);
            if (kind == 0) {
                const float f = kFloats[r % (sizeof kFloats / sizeof kFloats[0])];
                std::memcpy(&v[4 * lane], &f, 4);
            } else if (kind == 1 && lane < 2) {
                const double d = kDoubles[r % (sizeof kDoubles / sizeof kDoubles[0])];
                std::memcpy(&v[8 * lane], &d, 8);
            } else if (kind == 2) {
                std::memcpy(&v[4 * lane], &r, 4);
            }
        }
    }
    for (size_t i = 0; i < kFuzzScratch; i += 8) {
        const uint64_t r = splitmix(&s);
        std::memcpy(scratch + i, &r, 8);
    }
}

bool nan32(uint32_t u) { return (u & 0x7f800000) == 0x7f800000 && (u & 0x7fffff); }
bool nan64(uint64_t u) { return (u & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (u & 0xfffffffffffffull); }

// Equal, except where both hold a NaN: which NaN an operation gives (its sign
// and payload) differs between the host's and Arm's rules, and the JIT runs
// with x86's (arm64_jit.cpp, Unsafe_InaccurateNaN).
bool same_word(uint64_t a, uint64_t b) {
    if (a == b || (nan64(a) && nan64(b))) return true;
    for (int half = 0; half < 2; ++half) {
        const uint32_t x = static_cast<uint32_t>(a >> (32 * half)), y = static_cast<uint32_t>(b >> (32 * half));
        if (x != y && !(nan32(x) && nan32(y))) return false;
    }
    return true;
}

std::string fuzz_diff(const FuzzState& want, const uint8_t* want_mem, const FuzzState& got, const uint8_t* got_mem) {
    for (int i = 2; i < 28; ++i) {
        if (want.x[i] != got.x[i]) return "x" + std::to_string(i) + " " + hex(got.x[i]) + ", want " + hex(want.x[i]);
    }
    if (want.nzcv != got.nzcv) return "NZCV " + hex(got.nzcv) + ", want " + hex(want.nzcv);
    for (int i = 0; i < 32; ++i) {
        for (int half = 0; half < 2; ++half) {
            uint64_t a, b;
            std::memcpy(&a, &want.v[i][8 * half], 8);
            std::memcpy(&b, &got.v[i][8 * half], 8);
            if (!same_word(a, b)) {
                return "v" + std::to_string(i) + "[" + std::to_string(half) + "] " + hex(b) + ", want " + hex(a);
            }
        }
    }
    for (size_t i = 0; i < kFuzzScratch; i += 8) {
        uint64_t a, b;
        std::memcpy(&a, want_mem + i, 8);
        std::memcpy(&b, got_mem + i, 8);
        if (!same_word(a, b)) return "memory +" + std::to_string(i) + " " + hex(b) + ", want " + hex(a);
    }
    return "";
}

// Each function from three start states on each engine; the interpreter's
// results are the reference.
void run_fuzz(winapk::GuestLinker& linker, const std::vector<std::pair<winapk::arm64::Engine, const char*>>& engines) {
    using winapk::arm64::Engine;
    std::printf("\n==== random instruction sequences (libwinapk_arm64_fuzz.so) ====\n");
    std::string error;
    void* lib = linker.open("libwinapk_arm64_fuzz.so", winapk::GuestLinker::kRtldNow, &error);
    check("open libwinapk_arm64_fuzz.so", lib != nullptr, error);
    if (!lib) return;
    std::vector<void*> fns;
    std::vector<bool> jit_nan;  // gen_arm64_fuzz.py: FMAX/FMIN, whose NaNs the JIT does not keep
    for (int i = 0;; ++i) {
        const std::string name = "fuzz_" + std::to_string(i);
        void* f = linker.lookup(lib, name.c_str(), nullptr, &error);
        if (!f) break;
        fns.push_back(f);
        jit_nan.push_back(linker.lookup(lib, (name + "_jitnan").c_str(), nullptr, &error) != nullptr);
    }
    check("functions found", !fns.empty(), std::to_string(fns.size()));
    struct Result {
        FuzzState st;
        std::vector<uint8_t> mem;
    };
    std::vector<Result> want;
    // One state and one scratch buffer for every run: the code keeps their
    // addresses in registers.
    static FuzzState st;
    alignas(16) static uint8_t mem[kFuzzScratch];
    // WINAPK_FUZZ_STATE=<i>: every function starts from function i's states
    // (for fuzz_bisect.py, whose functions are the prefixes of one).
    const char* fixed = std::getenv("WINAPK_FUZZ_STATE");
    auto run = [&](size_t f, int seed) {
        void* fn = fns[f];
        const uint64_t index = fixed ? std::strtoull(fixed, nullptr, 10) : f;
        fuzz_input(0x5eed0000u + static_cast<uint64_t>(seed) * 7919u + index * 104729u, &st, mem);
        const uint64_t args[2] = {reinterpret_cast<uint64_t>(&st), reinterpret_cast<uint64_t>(mem)};
        winapk::arm64::call(fn, args, 2);
        return Result{st, std::vector<uint8_t>(mem, mem + kFuzzScratch)};
    };
    winapk::arm64::set_engine(Engine::kInterpreter);
    for (size_t f = 0; f < fns.size(); ++f) {
        for (int seed = 0; seed < kFuzzSeeds; ++seed) want.push_back(run(f, seed));
    }
    for (const auto& e : engines) {
        if (e.first == Engine::kInterpreter) continue;
        winapk::arm64::set_engine(e.first);
        // The JIT's NaNs are x86's unless WINAPK_ARM64_FP=exact; then they
        // are Arm's, which the interpreter's are not (the sign of a NaN an
        // invalid operation makes): its results are compared only without.
        const bool exact = std::getenv("WINAPK_ARM64_FP") && std::strcmp(std::getenv("WINAPK_ARM64_FP"), "exact") == 0;
        if (e.first == Engine::kJit && exact) continue;
        size_t bad = 0, translated = 0, skipped = 0;
        std::string first;
        for (size_t f = 0; f < fns.size(); ++f) {
            if (winapk::arm64::aot_lookup(reinterpret_cast<uint64_t>(fns[f]))) ++translated;
            if (e.first == Engine::kJit && jit_nan[f]) {
                ++skipped;
                continue;
            }
            for (int seed = 0; seed < kFuzzSeeds; ++seed) {
                const Result got = run(f, seed);
                const Result& w = want[f * kFuzzSeeds + seed];
                const std::string diff = fuzz_diff(w.st, w.mem.data(), got.st, got.mem.data());
                if (diff.empty()) continue;
                ++bad;
                std::printf("    fuzz_%zu, start state %d: %s\n", f, seed, diff.c_str());
                if (first.empty()) first = "fuzz_" + std::to_string(f) + ": " + diff;
            }
        }
        if (e.first == Engine::kAot) {
            check("the functions are translated", translated == fns.size(),
                  std::to_string(translated) + " of " + std::to_string(fns.size()));
        }
        check((std::string(e.second) + " agrees with the interpreter on " + std::to_string(fns.size() - skipped) +
               " random functions, " + std::to_string(kFuzzSeeds) + " start states each")
                  .c_str(),
              bad == 0, std::to_string(bad) + " differ; first " + first);
    }
}

// The benchmarks of arm64_bench.c natively and on each engine. The results
// must agree; the times are informational (no threshold: CI machines vary too
// much). For each benchmark:
//
//   * native and the JIT, warm (best of three runs), and their ratio. The
//     interpreter, about 80 times slower, runs 1/kInterpScale of the
//     iterations and is checked for its result only.
//   * first: how much longer the JIT's first call takes than a warm one, on
//     code it has not seen: mostly translation, the cost that translating at
//     install time would take off the app's start.
//   * where the JIT's time goes, from a profiled run: host functions versus
//     arm64 code, and the transitions and fallbacks per run.
constexpr uint64_t kInterpScale = 32;

double time_ms(const std::function<uint64_t()>& body, uint64_t* result) {
    const auto t0 = std::chrono::steady_clock::now();
    *result = body();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

double best_ms(const std::function<uint64_t()>& body, uint64_t* result) {
    double best = 0;
    for (int i = 0; i < 3; ++i) {
        const double ms = time_ms(body, result);
        if (i == 0 || ms < best) best = ms;
    }
    return best;
}

void run_speed(winapk::GuestLinker& linker,
               const std::vector<std::pair<winapk::arm64::Engine, const char*>>& engines) {
    using winapk::arm64::Engine;
    std::printf("\n==== speed (libwinapk_arm64_bench.so, -O2; ratios to native) ====\n");
    std::string error;
    void* lib = linker.open("libwinapk_arm64_bench.so", winapk::GuestLinker::kRtldNow, &error);
    check("open libwinapk_arm64_bench.so", lib != nullptr, error);
    if (!lib) return;
    struct Bench {
        const char* name;
        IsaFn native;
        uint64_t iterations;
    };
#define BENCH_CASE(name, n) {#name, &name, n},
    static const Bench kBenches[] = {BENCHES(BENCH_CASE)};
#undef BENCH_CASE
    size_t results = 0, results_ok = 0;
    const Engine last = engines.back().first;
    std::printf("  %-10s %9s", "", "native");
    for (const auto& e : engines) {
        if (e.first != Engine::kInterpreter) std::printf(" %15s", e.second);
    }
    if (last != Engine::kInterpreter) std::printf(" %9s  where the %s's time goes", "first", engines.back().second);
    std::printf("\n");
    for (const Bench& b : kBenches) {
        void* fn = sym(linker, lib, b.name);
        if (!fn) continue;
        const uint64_t seed = 12345;
        uint64_t want = 0;
        const double native_ms = best_ms([&] { return b.native(b.iterations, seed, 0); }, &want);
        const uint64_t want_scaled = b.native(b.iterations / kInterpScale, seed, 0);
        std::printf("  %-10s %7.1fms", b.name + 6, native_ms);
        double first_ms = 0;
        std::vector<bool> matches;
        for (const auto& e : engines) {
            winapk::arm64::set_engine(e.first);
            uint64_t got = 0;
            if (e.first == Engine::kInterpreter) {
                const uint64_t args[3] = {b.iterations / kInterpScale, seed, 0};
                got = winapk::arm64::call(fn, args, 3);
                matches.push_back(got == want_scaled);
                continue;
            }
            const uint64_t args[3] = {b.iterations, seed, 0};
            const double cold = time_ms([&] { return winapk::arm64::call(fn, args, 3); }, &got);
            const double ms = best_ms([&] { return winapk::arm64::call(fn, args, 3); }, &got);
            first_ms = cold - ms;
            std::printf(" %7.1fms %5.1fx", ms, native_ms > 0 ? ms / native_ms : 0.0);
            matches.push_back(got == want);
        }
        if (last != Engine::kInterpreter) {
            winapk::arm64::set_profiling(true);
            const winapk::arm64::Stats s0 = winapk::arm64::stats();
            const uint64_t args[3] = {b.iterations, seed, 0};
            uint64_t got = 0;
            time_ms([&] { return winapk::arm64::call(fn, args, 3); }, &got);
            const winapk::arm64::Stats s1 = winapk::arm64::stats();
            winapk::arm64::set_profiling(false);
            const double run = static_cast<double>(s1.run_ns - s0.run_ns);
            const double host = static_cast<double>(s1.host_ns - s0.host_ns);
            std::printf(" %7.2fms  host %4.1f%%, %llu host calls, %llu callbacks, %llu exits, %llu interpreted",
                        first_ms, run > 0 ? 100 * host / run : 0.0,
                        static_cast<unsigned long long>(s1.host_calls - s0.host_calls),
                        static_cast<unsigned long long>(s1.guest_entries - s0.guest_entries - 1),
                        static_cast<unsigned long long>(s1.jit_exits - s0.jit_exits),
                        static_cast<unsigned long long>(s1.interp_insns - s0.interp_insns));
            if (last == Engine::kAot) {
                std::printf(", AOT %llu entries %llu bails",
                            static_cast<unsigned long long>(s1.aot_entries - s0.aot_entries),
                            static_cast<unsigned long long>(s1.aot_bails - s0.aot_bails));
            }
        }
        std::printf("\n");
        for (size_t i = 0; i < engines.size(); ++i) {
            if (!matches[i])
                check((std::string(b.name) + " on the " + engines[i].second + " matches native").c_str(), false);
        }
        results_ok += std::count(matches.begin(), matches.end(), true);
        results += matches.size();
    }
    check("every benchmark result matches native", results_ok == results,
          std::to_string(results_ok) + " of " + std::to_string(results));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: winapk-arm64-test <dir with libwinapk_arm64_*.so>\n");
        return 2;
    }
    winapk::LinkerConfig config;
    config.search_paths.push_back(std::filesystem::u8path(argv[1]));
    config.host = winapk::mini_bionic_resolve;
    // What the shim lacks and the runtime stubs (host/guest_libs.cpp): the
    // NDK's static libc++ refers to a few such (openlog, swprintf, ...),
    // never called here.
    config.load.unresolved = [](const char*) { return reinterpret_cast<void*>(&missing_function); };
    winapk::GuestLinker linker(config);

    // Every check on each engine the build has: the interpreter, the JIT
    // (dynarmic) when it is built in, and the code translated ahead of time
    // (the <lib>.so.aot.dll files next to the libraries, when present).
    std::vector<std::pair<winapk::arm64::Engine, const char*>> engines = {
        {winapk::arm64::Engine::kInterpreter, "interpreter"}};
    if (winapk::arm64::set_engine(winapk::arm64::Engine::kJit)) {
        engines.push_back({winapk::arm64::Engine::kJit, "JIT"});
    } else {
        std::printf("(this build has no JIT: -DWINAPK_ARM64_JIT=ON adds it)\n");
    }
    engines.push_back({winapk::arm64::Engine::kAot, "AOT"});
    for (const auto& e : engines) {
        winapk::arm64::set_engine(e.first);
        std::printf("\n==== Win.apk arm64, %s ====\n", e.second);
        run_isa(linker, "libwinapk_arm64_isa_O2.so");
        run_isa(linker, "libwinapk_arm64_isa_O3.so");
        run_isa(linker, "libwinapk_arm64_isa_lse.so");
        run_bridge(linker, "libwinapk_arm64_bridge.so");
        run_flow(linker, "libwinapk_arm64_flow.so");
        run_eh(linker, std::filesystem::u8path(argv[1]));
        run_unload(linker, "libwinapk_arm64_unload.so");
        run_patch(linker, e.first == winapk::arm64::Engine::kAot   ? "libwinapk_arm64_patch_aot.so"
                          : e.first == winapk::arm64::Engine::kJit ? "libwinapk_arm64_patch_jit.so"
                                                                   : "libwinapk_arm64_patch_interp.so");
    }
    winapk::arm64::set_engine(winapk::arm64::Engine::kAot);
    run_rebased(linker, std::filesystem::u8path(argv[1]));
    run_sealed(linker, std::filesystem::u8path(argv[1]));
    run_fuzz(linker, engines);
    run_speed(linker, engines);

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures;
}
