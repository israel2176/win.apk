// x86_syscall_test.cpp -- winapk-syscall-test: raw Linux system calls in x86_64
// guest code (include/winapk/x86_syscall.h).
//
//   winapk-syscall-test <guest dir> [<another build of libwinapk_syscall_guest.so> ...]
//
// First the instruction decoder against encodings whose lengths are known.
// Then, from <guest dir>:
//   * libwinapk_plain_guest.so, ordinary code with no SYSCALL: it must load
//     byte for byte as in the file and leave the run-time scan off;
//   * libwinapk_jit_guest.so, code written as a JIT writes it (W^X rounds on
//     fresh pages, an anonymous file mapped writable and executable), run with
//     the scan off and again once it is on, timed;
//   * libwinapk_syscall_guest.so (guest/syscall_guest.c): its SYSCALL
//     instructions found, raw calls from a constructor, the kernel's register
//     and error conventions, raw mmap/mprotect/munmap, code written at run
//     time, and 0f 05 bytes that are no instruction left alone.
// Every further build of the last must give the same checksum (a packed copy,
// checked by hand: docs/research/packers.md).
//
// Exit code: the number of failed checks.

#include "winapk/elf_loader.h"
#include "winapk/fs_tls_patch.h"
#include "winapk/guest_abi.h"
#include "winapk/mini_bionic.h"
#include "winapk/x86_syscall.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

namespace {

int g_failures = 0;

void check(const std::string& what, bool ok, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++g_failures;
}

struct Encoding {
    const char* what;
    std::vector<uint8_t> bytes;
    size_t len;  // 0: not decodable
};

void decoder_checks() {
    std::printf("instruction lengths\n");
    const Encoding kCases[] = {
        {"syscall", {0x0f, 0x05}, 2},
        {"ud2", {0x0f, 0x0b}, 2},
        {"ret", {0xc3}, 1},
        {"push rbp", {0x55}, 1},
        {"push r15", {0x41, 0x57}, 2},
        {"mov eax, imm32", {0xb8, 0x27, 0, 0, 0}, 5},
        {"mov rax, imm64", {0x48, 0xb8, 1, 2, 3, 4, 5, 6, 7, 8}, 10},
        {"mov ax, imm16", {0x66, 0xb8, 1, 2}, 4},
        {"mov rbp, rsp", {0x48, 0x89, 0xe5}, 3},
        {"sub rsp, imm8", {0x48, 0x83, 0xec, 0x20}, 4},
        {"sub rsp, imm32", {0x48, 0x81, 0xec, 0, 1, 0, 0}, 7},
        {"mov rax, [rip+disp32]", {0x48, 0x8b, 0x05, 1, 2, 3, 4}, 7},
        {"mov rax, fs:[0x28]", {0x64, 0x48, 0x8b, 0x04, 0x25, 0x28, 0, 0, 0}, 9},
        {"mov [rsp+8], rdi (SIB disp8)", {0x48, 0x89, 0x7c, 0x24, 0x08}, 5},
        {"lea rax, [rbx+rcx*4+disp32]", {0x48, 0x8d, 0x84, 0x8b, 1, 2, 3, 4}, 8},
        {"mov eax, [disp32] (SIB, no base)", {0x8b, 0x04, 0x25, 1, 2, 3, 4}, 7},
        {"mov dword [rbp-4], imm32", {0xc7, 0x45, 0xfc, 1, 2, 3, 4}, 7},
        {"mov word [rax], imm16", {0x66, 0xc7, 0x00, 1, 2}, 5},
        {"test byte [rax], imm8", {0xf6, 0x00, 0x01}, 3},
        {"test dword [rax], imm32", {0xf7, 0x00, 1, 2, 3, 4}, 6},
        {"neg dword [rax]", {0xf7, 0x18}, 2},
        {"call rel32", {0xe8, 0, 0, 0, 0}, 5},
        {"jmp rel8", {0xeb, 0x10}, 2},
        {"je rel32", {0x0f, 0x84, 0, 0, 0, 0}, 6},
        {"jmp [rip+disp32]", {0xff, 0x25, 0, 0, 0, 0}, 6},
        {"call rax", {0xff, 0xd0}, 2},
        {"endbr64", {0xf3, 0x0f, 0x1e, 0xfa}, 4},
        {"nop dword [rax+rax+0]", {0x0f, 0x1f, 0x44, 0x00, 0x00}, 5},
        {"nopw cs:[rax+rax+0]", {0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0, 0, 0, 0}, 10},
        {"movsxd rax, ecx", {0x48, 0x63, 0xc1}, 3},
        {"imul eax, ecx, imm8", {0x6b, 0xc1, 0x05}, 3},
        {"cmpxchg [rdi], ecx (lock)", {0xf0, 0x0f, 0xb1, 0x0f}, 4},
        {"pshufd xmm0, xmm1, imm8", {0x66, 0x0f, 0x70, 0xc1, 0x1b}, 5},
        {"pshufb xmm0, xmm1 (0f 38)", {0x66, 0x0f, 0x38, 0x00, 0xc1}, 5},
        {"palignr xmm0, xmm1, imm8 (0f 3a)", {0x66, 0x0f, 0x3a, 0x0f, 0xc1, 0x04}, 6},
        {"movups xmm0, [rsi]", {0x0f, 0x10, 0x06}, 3},
        {"vmovdqu ymm0, [rsi] (VEX c5)", {0xc5, 0xfe, 0x6f, 0x06}, 4},
        {"vpshufb ymm0, ymm1, ymm2 (VEX c4, 0f38)", {0xc4, 0xe2, 0x75, 0x00, 0xc2}, 5},
        {"vpalignr ymm0, ymm1, ymm2, imm8 (VEX c4, 0f3a)", {0xc4, 0xe3, 0x75, 0x0f, 0xc2, 0x04}, 6},
        {"vzeroupper", {0xc5, 0xf8, 0x77}, 3},
        {"vmovdqu64 zmm0, [rsi+0x40] (EVEX, disp8)", {0x62, 0xf1, 0xfe, 0x48, 0x6f, 0x46, 0x01}, 7},
        {"vpternlogd zmm0, zmm1, zmm2, imm8 (EVEX 0f3a)", {0x62, 0xf3, 0x75, 0x48, 0x25, 0xc2, 0x96}, 7},
        {"movabs rax, [moffs64]", {0x48, 0xa1, 1, 2, 3, 4, 5, 6, 7, 8}, 10},
        {"enter imm16, imm8", {0xc8, 0x10, 0x00, 0x00}, 4},
        {"ret imm16", {0xc2, 0x08, 0x00}, 3},
        {"bt eax, imm8", {0x0f, 0xba, 0xe0, 0x03}, 4},
        {"shld eax, ecx, imm8", {0x0f, 0xa4, 0xc8, 0x04}, 4},
        {"fld qword [rax] (x87)", {0xdd, 0x00}, 2},
        {"rep movsb", {0xf3, 0xa4}, 2},
        {"cpuid", {0x0f, 0xa2}, 2},
        {"rdtscp (0f 01 f9)", {0x0f, 0x01, 0xf9}, 3},
        {"push imm32", {0x68, 1, 2, 3, 4}, 5},
        {"REX before a prefix is not REX", {0x48, 0x66, 0xb8, 1, 2}, 5},
        {"pushad (invalid in 64-bit mode)", {0x60}, 0},
        {"truncated", {0x48, 0x8b, 0x05, 1, 2}, 0},
    };
    for (const Encoding& e : kCases) {
        const size_t got = winapk::x86::instruction_length(e.bytes.data(), e.bytes.size());
        check(e.what, got == e.len, "length " + std::to_string(got) + (got == e.len ? "" : ", want " + std::to_string(e.len)));
    }
}

using LongFn = long(WINAPK_GUEST_ABI*)();
using IntFn = int(WINAPK_GUEST_ABI*)();
using ChecksumFn = unsigned(WINAPK_GUEST_ABI*)(unsigned);

using JitWxFn = long(WINAPK_GUEST_ABI*)(int, int);
using JitDualFn = long(WINAPK_GUEST_ABI*)(int);

std::unique_ptr<winapk::ElfImage> load(const std::string& path) {
    std::printf("\n%s\n", path.c_str());
    std::string error;
    auto img = winapk::ElfImage::load(path, winapk::mini_bionic_resolve, &error);
    check("load (and its constructors)", img != nullptr, error);
    return img;
}

// Whether a guest's request for read-execute memory is still given as asked: the run-time scan is off.
bool run_time_scan_off() {
    void* page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    const int prot = winapk::x86::exec_protection(page, 4096, 5);
    winapk::x86::forget(page, 4096);
    VirtualFree(page, 0, MEM_RELEASE);
    return prot == 5;
}

// Ordinary compiled code with no SYSCALL: nothing found, nothing changed, nothing turned on.
void plain_library(const std::string& path) {
    const size_t sites_before = winapk::x86::sites_patched();
    auto img = load(path);
    if (!img) return;
    check("no SYSCALL found", img->syscall_sites_patched() == 0 && winapk::x86::sites_patched() == sites_before);
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t compared = 0, differing = 0;
    for (const winapk::elf::Phdr& ph : img->phdrs()) {
        if (ph.p_type != winapk::elf::kPtLoad || !(ph.p_flags & winapk::elf::kPfX)) continue;
        if (ph.p_offset + ph.p_filesz > file.size()) continue;
        const auto* mem = reinterpret_cast<const char*>(img->load_bias() + ph.p_vaddr);
        for (uint64_t i = 0; i < ph.p_filesz; ++i) differing += mem[i] != file[ph.p_offset + i];
        compared += ph.p_filesz;
    }
    check("executable segments byte for byte as in the file", compared > 0 && differing == 0,
          std::to_string(compared) + " bytes compared, " + std::to_string(differing) + " differ");
    check("the run-time scan stays off", run_time_scan_off());
}

// A JIT's ways of making code: W^X on fresh pages, and one anonymous file mapped writable and executable.
// The time per round is printed: the run-time scan, once on, adds a first-run fault per page made executable.
void jit_checks(winapk::ElfImage& img, const char* when, double* us_per_round) {
    auto wx = reinterpret_cast<JitWxFn>(img.dlsym("jit_wx"));
    auto dual = reinterpret_cast<JitDualFn>(img.dlsym("jit_dual_mapped"));
    if (!wx || !dual) return check(std::string("jit functions exported (") + when + ")", false);
    constexpr int kRounds = 2000, kCalls = 200;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    const long r = wx(kRounds, kCalls);
    QueryPerformanceCounter(&t1);
    *us_per_round = 1e6 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart) / kRounds;
    char detail[96];
    std::snprintf(detail, sizeof detail, "returned %ld, %.2f us per round", r, *us_per_round);
    check(std::string("W^X: two fresh pages written, RX, run ") + std::to_string(kCalls) + " times, rewritten; " +
              std::to_string(kRounds) + " rounds (" + when + ")",
          r == 1, detail);
    const long d = dual(500);
    check(std::string("an anonymous file written through RW and run through RX, 500 times (") + when + ")", d == 1,
          "returned " + std::to_string(d));
}

bool syscall_library(const std::string& path, unsigned* checksum) {
    const size_t before = winapk::x86::sites_patched();
    auto img = load(path);
    if (!img) return false;
    std::printf("  SYSCALL sites patched at load: %zu, in all: %zu, calls answered: %zu\n",
                img->syscall_sites_patched(), winapk::x86::sites_patched() - before, winapk::x86::calls_answered());
    check("SYSCALL instructions found", img->syscall_sites_patched() > 0);
    check("the run-time scan is on", !run_time_scan_off());
    auto ctor = reinterpret_cast<IntFn>(img->dlsym("syscall_ctor_ran"));
    check("syscall_ctor_ran", ctor && ctor() == 1);
    for (const char* name : {"syscall_getpid", "syscall_rcx_is_return", "syscall_pipe_roundtrip", "syscall_errors",
                             "syscall_runtime_code", "syscall_copied_code", "syscall_cross_page"}) {
        auto fn = reinterpret_cast<LongFn>(img->dlsym(name));
        const long r = fn ? fn() : -1000;
        check(name, r == 1, "returned " + std::to_string(r));
    }
    // 0f 05 that is no instruction: jumped over, and inside an immediate.
    auto decoy = reinterpret_cast<LongFn>(img->dlsym("syscall_decoy"));
    auto skipped = static_cast<const uint8_t*>(img->dlsym("syscall_decoy_skipped"));
    auto immediate = static_cast<const uint8_t*>(img->dlsym("syscall_decoy_immediate"));
    check("0f 05 jumped over is left alone", skipped && skipped[0] == 0x0f && skipped[1] == 0x05);
    check("0f 05 inside an immediate is left alone",
          immediate && immediate[0] == 0xb8 && immediate[1] == 0x0f && immediate[2] == 0x05);
    check("the code around them runs", decoy && decoy() == 0x50f);
    // /proc/self/auxv by raw calls (guest/syscall_guest.c): one bit per check.
    {
        auto fn = reinterpret_cast<LongFn>(img->dlsym("syscall_auxv"));
        const long r = fn ? fn() : -1000;
        check("/proc/self/auxv: AT_NULL last, the values getauxval gives, AT_PAGESZ 4096, AT_RANDOM 16 readable "
              "bytes, AT_PLATFORM x86_64; getauxval of a missing type 0 and ENOENT",
              r == 63, std::to_string(r));
    }
    auto sum = reinterpret_cast<ChecksumFn>(img->dlsym("syscall_checksum"));
    check("syscall_checksum exported", sum != nullptr);
    *checksum = sum ? sum(77) : 0;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: winapk-syscall-test <guest dir> [<another build of libwinapk_syscall_guest.so> ...]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    winapk::set_guest_stack_guard_for_current_thread(0x5eed5eed5eed5e00ull);
    winapk::commit_guest_stack_for_current_thread();
    decoder_checks();
    const std::string dir = std::string(argv[1]) + "/";
    // First the libraries without a SYSCALL, while nothing has turned the run-time scan on.
    plain_library(dir + "libwinapk_plain_guest.so");
    auto jit = load(dir + "libwinapk_jit_guest.so");
    check("the JIT library has no SYSCALL", jit && jit->syscall_sites_patched() == 0);
    double off = 0, on = 0;
    if (jit) jit_checks(*jit, "run-time scan off", &off);
    check("the run-time scan is still off", run_time_scan_off());
    std::vector<unsigned> sums;
    unsigned sum = 0;
    if (syscall_library(dir + "libwinapk_syscall_guest.so", &sum)) sums.push_back(sum);
    if (jit) {
        jit_checks(*jit, "run-time scan on", &on);
        std::printf("  the run-time scan's cost: %.2f us per round (two pages made executable, then first run)\n",
                    on - off);
    }
    for (int i = 2; i < argc; ++i) {
        if (syscall_library(argv[i], &sum)) sums.push_back(sum);
    }
    for (size_t i = 1; i < sums.size(); ++i) {
        check("the same code in " + std::string(argv[i + 1]), sums[i] == sums[0],
              std::to_string(sums[i]) + " against " + std::to_string(sums[0]));
    }
    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures;
}
