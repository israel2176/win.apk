// exec_test.cpp -- winapk-exec-test: an Android program started as a child process by the Bionic
// shim's posix_spawn (src/bionic/process.cpp) and run by this test started again with --exec
// (host/exec_main.cpp, as winapk-runtime runs it), as an app's native code starts one: through the
// shim's exported functions, System V.
//
//   winapk-exec-test <winapk_exec_guest>
//
// The program (test/guest/exec_guest.c) prints its arguments and a variable, writes a line to
// stderr, upper-cases stdin to stdout and exits with 7 (or "exit=N"). Checks: its stdout and stdin
// through the shim's own pipes (pumped), stderr into a file (addopen), the arguments and the
// environment the spawn gave, the exit status; WNOHANG on a running child; kill() ending it with
// the signal; waitid; ECHILD with no child left; ENOENT for a missing program and ENOEXEC for a file
// that is not one.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "exec_main.h"

#include "winapk/guest_abi.h"
#include "winapk/mini_bionic.h"

namespace {

int g_failures = 0;
void check(const char* what, bool ok, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++g_failures;
}

#define GUEST WINAPK_GUEST_ABI
template <typename F>
F fn(const char* name) {
    void* p = winapk::mini_bionic_resolve(name);
    if (!p) {
        std::printf("  [FAIL] %s is not in the shim\n", name);
        std::exit(1);
    }
    return reinterpret_cast<F>(p);
}

struct Shim {
    int(GUEST* pipe)(int*) = fn<decltype(pipe)>("pipe");
    int(GUEST* close)(int) = fn<decltype(close)>("close");
    int64_t(GUEST* read)(int, void*, size_t) = fn<decltype(read)>("read");
    int64_t(GUEST* write)(int, const void*, size_t) = fn<decltype(write)>("write");
    int(GUEST* actions_init)(void**) = fn<decltype(actions_init)>("posix_spawn_file_actions_init");
    int(GUEST* actions_destroy)(void**) = fn<decltype(actions_destroy)>("posix_spawn_file_actions_destroy");
    int(GUEST* adddup2)(void**, int, int) = fn<decltype(adddup2)>("posix_spawn_file_actions_adddup2");
    int(GUEST* addopen)(void**, int, const char*, int, unsigned) = fn<decltype(addopen)>("posix_spawn_file_actions_addopen");
    int(GUEST* spawn)(int*, const char*, void* const*, void* const*, char* const*, char* const*) =
        fn<decltype(spawn)>("posix_spawn");
    int(GUEST* waitpid)(int, int*, int) = fn<decltype(waitpid)>("waitpid");
    int(GUEST* waitid)(int, int, void*, int) = fn<decltype(waitid)>("waitid");
    int(GUEST* kill)(int, int) = fn<decltype(kill)>("kill");
    int*(GUEST* errno_location)() = fn<decltype(errno_location)>("__errno");
};

std::string read_all(const Shim& s, int fd) {
    std::string out;
    char buf[512];
    for (;;) {
        const int64_t n = s.read(fd, buf, sizeof buf);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    // The children this test spawns are this program again (src/bionic/process.cpp), as an app's
    // are winapk-runtime.exe again: it runs the guest program as winapk-runtime --exec does.
    if (argc > 1 && std::strcmp(argv[1], "--exec") == 0) {
        int wargc = 0;
        wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        return winapk::host::exec_program_main(wargc - 2, wargv + 2);
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: winapk-exec-test <winapk_exec_guest>\n");
        return 2;
    }
    char full[MAX_PATH];
    GetFullPathNameA(argv[1], MAX_PATH, full, nullptr);
    const std::string program = full;
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    winapk::set_android_root((std::string(temp) + "winapk-exec-test-root").c_str());
    Shim s;

    std::printf("a child with pipes\n");
    int out[2], in[2];
    s.pipe(out);
    s.pipe(in);
    void* actions = nullptr;
    s.actions_init(&actions);
    s.adddup2(&actions, in[0], 0);
    s.adddup2(&actions, out[1], 1);
    const std::string err_file = std::string(temp) + "winapk-exec-test-stderr.txt";
    s.addopen(&actions, 2, err_file.c_str(), 01 | 0100 | 01000, 0644);  // O_WRONLY | O_CREAT | O_TRUNC
    std::string a0 = "winapk_exec_guest", a1 = "hello world", a2 = "x";
    char* args[] = {a0.data(), a1.data(), a2.data(), nullptr};
    std::string e0 = "WINAPK_EXEC_TEST=from the spawn";
    char* envs[] = {e0.data(), nullptr};
    int pid = 0;
    const int r = s.spawn(&pid, program.c_str(), &actions, nullptr, args, envs);
    check("posix_spawn", r == 0 && pid > 0, "errno " + std::to_string(r));
    s.actions_destroy(&actions);
    // The parent's copies of the child's ends: closed, as an app closes them after a spawn.
    s.close(in[0]);
    s.close(out[1]);
    s.write(in[1], "piped in\n", 9);
    s.close(in[1]);
    const std::string got = read_all(s, out[0]);
    s.close(out[0]);
    check("its arguments", got.find("argc=3\n") != std::string::npos && got.find("argv:hello world\n") != std::string::npos &&
                               got.find("argv:x\n") != std::string::npos,
          got);
    check("the spawn's environment", got.find("env:from the spawn\n") != std::string::npos);
    check("stdin through a pipe, upper-cased back", got.find("PIPED IN\n") != std::string::npos);
    int status = -1;
    check("waitpid: the child", s.waitpid(pid, &status, 0) == pid);
    check("exit status 7", status == (7 << 8), std::to_string(status));
    std::ifstream ef(err_file);
    std::stringstream es;
    es << ef.rdbuf();
    ef.close();
    check("stderr into the file the spawn opened", es.str() == "to stderr\n", es.str());
    DeleteFileA(err_file.c_str());

    std::printf("exit codes, WNOHANG, kill, waitid\n");
    std::string b1 = "exit=3";
    char* args2[] = {a0.data(), b1.data(), nullptr};
    int pid2 = 0, stdin_pipe[2];
    s.pipe(stdin_pipe);
    void* a2c = nullptr;
    s.actions_init(&a2c);
    s.adddup2(&a2c, stdin_pipe[0], 0);
    s.spawn(&pid2, program.c_str(), &a2c, nullptr, args2, envs);
    s.actions_destroy(&a2c);
    s.close(stdin_pipe[0]);
    // It waits on stdin, which stays open: still running.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check("WNOHANG while it runs: 0", s.waitpid(pid2, &status, 1) == 0);
    check("kill()", s.kill(pid2, 9) == 0);
    check("ended by the signal", s.waitpid(pid2, &status, 0) == pid2 && (status & 0x7f) == 9, std::to_string(status));
    s.close(stdin_pipe[1]);

    int pid3 = 0;
    s.spawn(&pid3, program.c_str(), nullptr, nullptr, args2, envs);  // stdin: this process's (empty)
    unsigned char info[128] = {};
    const int wr = s.waitid(1, pid3, info, 4);  // P_PID, WEXITED
    int code = 0, si_pid = 0;
    std::memcpy(&si_pid, info + 16, 4);
    std::memcpy(&code, info + 24, 4);
    check("waitid: exited with 3", wr == 0 && si_pid == pid3 && code == 3,
          "pid " + std::to_string(si_pid) + " status " + std::to_string(code));
    *s.errno_location() = 0;
    check("no child left: ECHILD", s.waitpid(-1, &status, 0) == -1 && *s.errno_location() == 10);

    std::printf("programs that are not\n");
    int none = 0;
    check("a missing program: ENOENT", s.spawn(&none, "/data/local/tmp/no-such-program", nullptr, nullptr, args, envs) == 2);
    const std::string text = std::string(temp) + "winapk-exec-test-text.txt";
    std::ofstream(text) << "#!/system/bin/sh\necho hi\n";
    check("a script: ENOEXEC (no shell here)", s.spawn(&none, text.c_str(), nullptr, nullptr, args, envs) == 8);
    DeleteFileA(text.c_str());

    std::printf(g_failures ? "%d FAILED\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
