// exec_main.cpp -- winapk-exec.exe: one Android program (an x86_64 ELF executable) an app started as
// a child process, run as a Windows process of its own.
//
//   winapk-exec --android-root <dir> [--app <package> <data dir>] [--env NAME=value]... -- <program> <argv...>
//
// Started by the Bionic shim's posix_spawn and exec family (src/bionic/process.cpp) and by Java's
// ProcessBuilder for a program file (the java.base overlay's jdk.internal.util.WinApkExec), with the
// parent's mapping of Android's paths and the environment it gave. The program's standard streams
// are this process's (the spawner hands them over).
//
// The program is mapped by the same guest linker and Bionic shim an app's libraries are (its
// DT_NEEDED libraries found beside it, as the app's native library directory has them), its
// constructors run, and then its entry point is jumped to with the stack a kernel makes: argc, the
// arguments, a null, the environment, a null, the auxiliary vector (AT_PAGESZ, AT_RANDOM). Its
// crtbegin calls __libc_init, which calls main and exit (src/bionic/misc.cpp). The shim is in
// program mode: exit and _exit add nothing to the program's stderr.
//
// A program that cannot be loaded ends as bionic's linker ends it: CANNOT LINK EXECUTABLE on stderr
// and exit status 1.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "winapk/abi_thunk.h"
#include "winapk/linker.h"
#include "winapk/mini_bionic.h"

namespace {

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// A function the shim does not have: 0, as the runtime's stubs answer, without a line on the
// program's stderr.
void quiet_missing(void*, const void*) {}
void* missing_stub(const char*) {
    static void* stub = winapk::make_tagged_zero_stub(reinterpret_cast<void*>(&quiet_missing), nullptr);
    return stub;
}
void* missing_object(const char*) { return std::calloc(1, 4096); }

[[noreturn]] void cannot_link(const std::string& program, const std::string& why) {
    std::fprintf(stderr, "CANNOT LINK EXECUTABLE \"%s\": %s\n", program.c_str(), why.c_str());
    std::fflush(stderr);
    ExitProcess(1);
}

// The program's entry point, from its ELF header.
uint64_t entry_vaddr(const std::filesystem::path& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return 0;
    unsigned char h[64] = {};
    const size_t got = std::fread(h, 1, sizeof h, f);
    std::fclose(f);
    if (got != sizeof h) return 0;
    uint64_t entry = 0;
    std::memcpy(&entry, h + 24, 8);
    return entry;
}

}  // namespace

// Switches to the program's stack and jumps to its entry: rcx the entry, rdx the stack (Microsoft
// x64 arguments). rbp and rdx are zero, as a kernel leaves them (rdx: no rtld_fini).
extern "C" [[noreturn]] void winapk_exec_jump(void* entry, void* stack);
__asm__(
    ".text\n"
    ".globl winapk_exec_jump\n"
    "winapk_exec_jump:\n"
    "    movq %rdx, %rsp\n"
    "    xorl %ebp, %ebp\n"
    "    xorl %edx, %edx\n"
    "    jmpq *%rcx\n");

namespace {

// Builds the kernel's stack on this thread's own stack (so Windows' stack bounds hold) and jumps.
[[noreturn]] __attribute__((noinline)) void run(void* entry, const std::vector<std::string>& args,
                                                const std::vector<std::string>& env) {
    alignas(16) uint64_t block[4096];
    static uint64_t random_bytes[2];
    random_bytes[0] = GetTickCount64() * 0x9E3779B97F4A7C15ull ^ reinterpret_cast<uintptr_t>(&block);
    random_bytes[1] = static_cast<uint64_t>(GetCurrentProcessId()) * 0xC2B2AE3D27D4EB4Full;
    size_t i = 0;
    const size_t limit = sizeof block / sizeof block[0] - 8;
    block[i++] = args.size();
    for (const std::string& a : args) {
        if (i < limit) block[i++] = reinterpret_cast<uintptr_t>(a.c_str());
    }
    block[i++] = 0;
    for (const std::string& e : env) {
        if (i < limit) block[i++] = reinterpret_cast<uintptr_t>(e.c_str());
    }
    block[i++] = 0;
    block[i++] = 6;  // AT_PAGESZ
    block[i++] = 4096;
    block[i++] = 25;  // AT_RANDOM
    block[i++] = reinterpret_cast<uintptr_t>(random_bytes);
    block[i++] = 0;  // AT_NULL
    block[i++] = 0;
    winapk_exec_jump(entry, block);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring root, package, data_dir;
    std::vector<std::string> env;
    int i = 1;
    for (; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--") {
            ++i;
            break;
        }
        if (a == L"--android-root" && i + 1 < argc) {
            root = argv[++i];
        } else if (a == L"--app" && i + 2 < argc) {
            package = argv[++i];
            data_dir = argv[++i];
        } else if (a == L"--env" && i + 1 < argc) {
            env.push_back(utf8(argv[++i]));
        } else {
            std::fprintf(stderr, "winapk-exec: unknown option %ls\n", argv[i]);
            return 2;
        }
    }
    if (i >= argc) {
        std::fprintf(stderr, "usage: winapk-exec --android-root <dir> [--app <package> <data dir>] [--env NAME=value]... "
                             "-- <program> <argv...>\n");
        return 2;
    }
    const std::filesystem::path program = argv[i++];
    std::vector<std::string> args;
    for (; i < argc; ++i) args.push_back(utf8(argv[i]));
    if (args.empty()) args.push_back(utf8(program.wstring()));

    if (!root.empty()) winapk::set_android_root(utf8(root).c_str());
    if (!package.empty()) winapk::set_app_data(utf8(package).c_str(), utf8(data_dir).c_str());
    winapk::mini_bionic_program_mode(env);

    winapk::LinkerConfig config;
    config.host = winapk::mini_bionic_resolve;
    config.load.unresolved = missing_stub;
    config.load.unresolved_object = missing_object;
    config.search_paths.push_back(program.parent_path());
    static winapk::GuestLinker linker(config);  // lives as long as the process
    std::string error;
    winapk::GuestLibrary* lib = linker.open(utf8(program.wstring()), winapk::GuestLinker::kRtldNow, &error);
    if (!lib) cannot_link(args[0], error.empty() ? "cannot load it" : error);
    const winapk::ElfImage* image = winapk::GuestLinker::image_of(lib);
    if (!image || image->is_arm64()) cannot_link(args[0], "not an x86_64 program");
    const uint64_t entry = entry_vaddr(program);
    if (!entry) cannot_link(args[0], "no entry point");

    // The environment main gets is the shim's own (__libc_init passes `environ`); the spawner's
    // variables on the stack are what a program that walks past argv finds.
    run(reinterpret_cast<void*>(image->load_bias() + entry), args, env);
}
