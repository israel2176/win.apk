// aot_guard_test.cpp -- winapk-aot-guard-test: the crash guard of the arm64
// translation (host/aot_guard.h) through the real crash path.
//
//   winapk-aot-guard-test <scratch dir>
//
// Every step is a start of the same "app": the test runs itself again with
// --child, stderr to that start's log (the app's run log). The child installs
// the crash report (host/crash_report.cpp) as the runtime does, asks
// aot_guard_start whether translation is on, loads a translation through the
// loader (arm64_aot.cpp, aot_load) when it is, with winapk-aot-guard-dll.dll
// standing in for <lib>.so.aot.dll, and then ends the way the step says: a
// write to address 0 nobody handles, abort(), HotSpot's fatal error handler
// (crash_report_fatal, as the JVM's "abort" hook calls it), or a normal end (the
// TerminateProcess a closed window or Cocoa_exitProcess makes). The parent
// checks each start's log lines and what the state file says:
//
//   - a crash with translated code loaded is counted; a clean run between two
//     crashes sets the count back; a crash with nothing translated loaded is
//     not counted;
//   - after two in a row (a fault, then abort()), the next start runs with
//     translation off and logs why and which file to delete;
//   - two crashes in a row on the JIT: "not the translation", and the next
//     start turns translation on again, for good for that build;
//   - another build of the app (another id) starts the count over, and
//     deleting the file turns translation back on.
//
// Exit code: the number of failed checks.

#include "aot_guard.h"
#include "crash_report.h"

#include "winapk/arm64_aot.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <windows.h>

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kGuardTestTextHash = 0x6775617264746573ull;  // aot_guard_dll.cpp's

int g_failures = 0;

void check(const char* what, bool ok, const std::string& detail = "") {
    // The detail (a start's whole log) only when it failed.
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, ok || detail.empty() ? "" : ":\n",
                ok ? "" : detail.c_str());
    if (!ok) ++g_failures;
}

// ---- the child: one start of the app ----------------------------------------------

__attribute__((noinline)) void fault() { *reinterpret_cast<volatile int*>(0) = 1; }

int child(const fs::path& dir, uint64_t id, const std::wstring& action) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    winapk::host::install_crash_report();
    winapk::host::set_crash_dump_location(dir, L"guardtest");
    const bool on = winapk::host::aot_guard_start(dir / "app", id);
    std::fprintf(stderr, "[test] the guard says: translation %s\n", on ? "on" : "off");
    bool loaded = false;
    if (action != L"noload-crash") {
        loaded = winapk::arm64::aot_load((dir / "lib" / "libguard.so").u8string().c_str(), 0, kGuardTestTextHash,
                                         0x10000, 0x20000);
    }
    std::fprintf(stderr, "[test] translation loaded: %s\n", loaded ? "yes" : "no");
    std::fflush(stderr);
    if (action == L"crash" || action == L"noload-crash") fault();
    if (action == L"abort") std::abort();
    if (action == L"fatal") {  // HotSpot's fatal error handler, through the JVM's "abort" hook
        winapk::host::crash_report_fatal("the JVM's fatal error handler");
        TerminateProcess(GetCurrentProcess(), 1);
    }
    // A closed window, Cocoa_exitProcess.
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

// ---- the parent -----------------------------------------------------------------

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

struct Start {
    std::string log;
    DWORD code = 0;
};

int g_step = 0;

Start start(const fs::path& dir, uint64_t id, const wchar_t* action) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path log = dir / ("start-" + std::to_string(++g_step) + ".log");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" --child \"" + dir.wstring() + L"\" " + std::to_wstring(id) +
                       L" " + action;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = out;
    PROCESS_INFORMATION pi{};
    Start s;
    const bool started = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle(out);
    if (!started) {
        check("start the child", false);
        return s;
    }
    if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 9);
    GetExitCodeProcess(pi.hProcess, &s.code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    s.log = read_file(log);
    std::printf("start %d (%ls): exit 0x%08lx\n", g_step, action, static_cast<unsigned long>(s.code));
    return s;
}

int parent(const fs::path& dir) {
    std::printf("Win.apk arm64 translation crash guard\n");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "app", ec);
    fs::create_directories(dir / "lib", ec);
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path dll = fs::path(self).parent_path() / "winapk-aot-guard-dll.dll";
    fs::copy_file(dll, dir / "lib" / "libguard.so.aot.dll", ec);
    check("the stand-in translation is in place", !ec, dll.u8string());
    const fs::path state = winapk::host::aot_guard_file(dir / "app");
    const std::string file = state.u8string();
    const char* kCounted1 = "[win.apk] arm64 AOT: crash counted: 1 of 2 starts in a row with translated code loaded";
    const char* kCounted2 =
        "[win.apk] arm64 AOT: crash counted: 2 starts in a row crashed with translated code loaded; the next start "
        "runs the app on the JIT";
    const char* kOff =
        "[win.apk] arm64 AOT: off for this app: it crashed 2 starts in a row with translated code loaded (";

    Start s = start(dir, 1, L"crash");
    check("a crash with translated code loaded is counted",
          has(s.log, "libguard.so.aot.dll: 0 translated functions") && has(s.log, kCounted1) && s.code != 0, s.log);
    check("the crash went through the crash path", has(s.log, "[win.apk] crash: unhandled access violation"));
    check("the state file says 1", has(read_file(state), "crashes 1\n"), read_file(state));

    s = start(dir, 1, L"clean");
    check("a clean run says nothing and keeps translation on",
          s.code == 0 && has(s.log, "translation loaded: yes") && !has(s.log, "crash counted"), s.log);
    s = start(dir, 1, L"crash");
    check("the clean run set the count back: the next crash is 1 again", has(s.log, kCounted1), s.log);
    s = start(dir, 1, L"noload-crash");
    check("a crash with no translated code loaded is not counted",
          has(s.log, "crash not counted against the translation: no translated code was loaded"), s.log);
    s = start(dir, 1, L"abort");
    check("abort() is a crash too: 2 in a row, the next start runs on the JIT", has(s.log, kCounted2), s.log);

    s = start(dir, 1, L"clean");
    check("the next start turns translation off and says why and how to retry",
          has(s.log, kOff) && has(s.log, "abort() from") && has(s.log, "access violation 0xc0000005 at") &&
              has(s.log, "; the JIT runs it. Delete " + file + " to try the translation again."),
          s.log);
    check("no translation is loaded with it off",
          has(s.log, "translation off") && has(s.log, "translation loaded: no") &&
              !has(s.log, "translated functions"),
          s.log);
    s = start(dir, 1, L"clean");
    check("it stays off after a clean run on the JIT", has(s.log, kOff) && has(s.log, "translation loaded: no"), s.log);
    s = start(dir, 1, L"crash");
    check("a crash on the JIT is counted",
          has(s.log, "[win.apk] arm64 AOT: crash counted: 1 of 2 starts in a row on the JIT"), s.log);
    s = start(dir, 1, L"crash");
    check("two crashes in a row on the JIT: not the translation",
          has(s.log, "[win.apk] arm64 AOT: the app crashes on the JIT too: not the translation (2 starts in a row "
                     "with translation off); the next start uses the translation again"),
          s.log);
    s = start(dir, 1, L"crash");
    check("the next start turns translation on again",
          has(s.log, "[win.apk] arm64 AOT: on again for this app: it crashed on the JIT too, 2 starts in a row (") &&
              has(s.log, "translation loaded: yes"),
          s.log);
    check("and the guard stands down for this build: crashes are no longer counted",
          has(s.log, "crash not counted against the translation: this build of the app crashed on the JIT as well"),
          s.log);
    s = start(dir, 1, L"crash");
    check("still on after another crash", has(s.log, "translation loaded: yes") && !has(s.log, kOff), s.log);

    s = start(dir, 2, L"crash");
    check("another build of the app starts the count over",
          has(s.log, "the app or the translator changed since the crashes counted in") && has(s.log, kCounted1),
          s.log);
    s = start(dir, 2, L"fatal");
    check("the JVM's fatal error handler is a crash too, counted again",
          has(s.log, "[win.apk] crash: the JVM's fatal error handler; the process ends") && has(s.log, kCounted2),
          s.log);
    s = start(dir, 2, L"clean");
    check("off again for the new build", has(s.log, kOff) && has(s.log, "translation loaded: no"), s.log);
    fs::remove(state, ec);
    s = start(dir, 2, L"clean");
    check("deleting the file turns translation on again",
          !has(s.log, kOff) && has(s.log, "translation loaded: yes"), s.log);

    bool temps = false;
    for (const auto& e : fs::directory_iterator(dir / "app", ec)) temps |= e.path().extension() == L".tmp";
    check("no temporary files are left beside the state", !temps);
    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 5 && std::wstring(argv[1]) == L"--child") {
        return child(argv[2], std::wcstoull(argv[3], nullptr, 10), argv[4]);
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: winapk-aot-guard-test <scratch dir>\n");
        return 2;
    }
    return parent(argv[1]);
}
