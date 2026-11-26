// process.cpp -- the child processes an app's native code starts: posix_spawn and its file
// actions and attributes, the exec family, and waitpid / wait4 / waitid / kill on those children.
//
// Windows cannot copy a running process, so fork, vfork and clone still fail with ENOSYS
// (misc.cpp). What a child process here runs is an Android program, an ELF executable (an app's
// own: a puzzle generator, a helper binary, a terminal's tools), started as its own Windows
// process: winapk-exec.exe beside the runtime (host/exec_main.cpp) maps it with the same loader and
// Bionic shim, with the parent's Android root and app data folder, and runs it from its entry
// point. A path that is not an ELF executable fails as a kernel fails it: ENOENT for a file that
// does not exist (a device's /system/bin/sh has none here), ENOEXEC for one that is not a program.
//
// The child's standard streams are what the parent's descriptors 0, 1 and 2 are once the spawn's
// file actions have run (dup2, close, open, in order): a file is handed over as its handle; a
// pipe, socket or other descriptor of the shim's own (fd.cpp: objects inside this process, which
// Windows knows nothing of) through a Windows pipe and a thread that pumps between the two until
// either side ends. Other descriptors are not inherited (Windows hands a child its standard
// handles, not a descriptor table). The environment the spawn names goes to the child (its
// variables set over Android's defaults, as the shim's own env() has them).
//
// The exec family replaces the calling process on Linux: here the program runs as a child with the
// caller's standard streams, and the caller ends with its status when it ends (no atexit handlers
// run, as none do across an exec).
//
// A child's status is Linux's: exit code << 8; a child ended by kill() is that signal; one ended
// by an exception (an access violation, a fail-fast) is SIGSEGV, or SIGABRT for a fail-fast.

#include "bionic_internal.h"
#include "winapk/guest_abi.h"
#include "winapk/mini_bionic.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <io.h>
#include <windows.h>

namespace winapk::bionic {

namespace {

constexpr int kENOEXEC = 8;
constexpr int kSIGKILL = 9, kSIGSEGV = 11, kSIGABRT = 6, kSIGCHLD = 17;
constexpr int kWNOHANG = 1, kWEXITED = 4, kWNOWAIT = 0x01000000;
constexpr int kPAll = 0, kPPid = 1;
constexpr int kOWronly = 01, kORdwr = 02, kOCreat = 0100, kOTrunc = 01000, kOAppend = 02000;

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// One argument as CommandLineToArgvW (and the CRT) read it back.
std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (size_t i = 0;; ++i) {
        size_t slashes = 0;
        while (i < a.size() && a[i] == L'\\') ++i, ++slashes;
        if (i == a.size()) {
            r.append(slashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') {
            r.append(slashes * 2 + 1, L'\\');
        } else {
            r.append(slashes, L'\\');
        }
        r += a[i];
    }
    return r + L"\"";
}

void say_once(std::atomic<bool>& said, const std::string& line) {
    if (said.exchange(true)) return;
    std::fprintf(stderr, "[win.apk] %s\n", line.c_str());
    std::fflush(stderr);
}

// ---- the children ---------------------------------------------------------------------------

struct Child {
    HANDLE process = nullptr;
    int signal = 0;  // the signal kill() ended it with
};
std::mutex g_children_lock;
std::map<int, Child> g_children;

int status_of(DWORD code, int signal) {
    if (signal) return signal & 0x7f;
    if (code == 0xC0000409u) return kSIGABRT;  // a fail-fast (abort's)
    if (code >= 0xC0000000u) return kSIGSEGV;  // an exception the program did not survive
    return static_cast<int>((code & 0xff) << 8);
}

// ---- file actions and attributes (bionic's opaque pointers) -------------------------------------

struct FileAction {
    enum Kind { kOpen, kClose, kDup2 } kind;
    int fd = -1, newfd = -1, flags = 0;
    unsigned mode = 0;
    std::string path;
};
struct FileActions {
    std::vector<FileAction> actions;
};
struct SpawnAttr {
    short flags = 0;
    int pgroup = 0;
};

// Where one of the child's standard streams comes from.
struct Source {
    enum Kind { kParent, kFile, kClosed } kind = kParent;
    int fd = -1;  // kParent: the parent's descriptor
    std::string path;
    int flags = 0;
};

// The handle the child gets for `src` (inheritable; the caller closes it once the child has it),
// starting a pump for a descriptor of the shim's own; `child_reads` is the stream's direction.
HANDLE handle_for(const Source& src, bool child_reads) {
    if (src.kind == Source::kClosed) {
        return CreateFileW(L"NUL", child_reads ? GENERIC_READ : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    }
    if (src.kind == Source::kFile) {
        const int acc = src.flags & 3;
        DWORD access = acc == kOWronly ? GENERIC_WRITE : acc == kORdwr ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
        if (src.flags & kOAppend) access = FILE_APPEND_DATA | (acc == kORdwr ? GENERIC_READ : 0);
        DWORD create = OPEN_EXISTING;
        if ((src.flags & kOCreat) && (src.flags & kOTrunc)) create = CREATE_ALWAYS;
        else if (src.flags & kOCreat) create = OPEN_ALWAYS;
        else if (src.flags & kOTrunc) create = TRUNCATE_EXISTING;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        return CreateFileW(file_path(src.path.c_str()).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, create,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (std::shared_ptr<FdObject> obj = fd_object(src.fd)) {
        // An object of this process: a Windows pipe to the child, pumped by a thread holding the
        // object, so its other end sees the child's bytes (or end of file) as from a process.
        HANDLE read_end = nullptr, write_end = nullptr;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
        if (!CreatePipe(&read_end, &write_end, &sa, 64 * 1024)) return INVALID_HANDLE_VALUE;
        HANDLE ours = child_reads ? write_end : read_end;
        HANDLE theirs = child_reads ? read_end : write_end;
        SetHandleInformation(theirs, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        std::thread([obj, ours, child_reads] {
            std::vector<char> buf(64 * 1024);
            for (;;) {
                if (child_reads) {
                    const int64_t n = obj->read(buf.data(), buf.size());
                    if (n == 0) break;
                    if (n < 0) {
                        if (*guest_errno() == kEAGAIN) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                            continue;
                        }
                        break;
                    }
                    DWORD done = 0;
                    if (!WriteFile(ours, buf.data(), static_cast<DWORD>(n), &done, nullptr)) break;
                } else {
                    DWORD got = 0;
                    if (!ReadFile(ours, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) || got == 0) break;
                    size_t at = 0;
                    while (at < got) {
                        const int64_t n = obj->write(buf.data() + at, got - at);
                        if (n > 0) {
                            at += static_cast<size_t>(n);
                        } else if (n < 0 && *guest_errno() == kEAGAIN) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        } else {
                            at = got;  // the reader is gone: the rest is dropped, as with EPIPE
                        }
                    }
                }
            }
            CloseHandle(ours);
        }).detach();
        return theirs;
    }
    // A file descriptor: its handle, duplicated for the child.
    const intptr_t h = _get_osfhandle(src.fd);
    if (h == -1 || h == -2) return INVALID_HANDLE_VALUE;
    HANDLE dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(h), GetCurrentProcess(), &dup, 0, TRUE,
                         DUPLICATE_SAME_ACCESS)) {
        return INVALID_HANDLE_VALUE;
    }
    return dup;
}

// The program at `path` (UTF-8, Android's or a host path), and whether it is one winapk-exec runs:
// 0, or the errno a kernel's execve would give.
int check_program(const char* path, std::wstring* host) {
    if (!path || !*path) return kENOENT;
    *host = host_path(path);
    HANDLE f = CreateFileW(file_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_ACCESS_DENIED ? kEPERM : kENOENT;
    unsigned char h[20] = {};
    DWORD got = 0;
    const bool ok = ReadFile(f, h, sizeof h, &got, nullptr) && got == sizeof h;
    CloseHandle(f);
    if (!ok || h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F') return kENOEXEC;
    const unsigned machine = h[18] | (h[19] << 8);
    if (machine != 62) {  // EM_X86_64; an arm64 executable is not run yet
        static std::atomic<bool> said{false};
        say_once(said, "exec of an arm64 program (" + std::string(path) + "): only x86_64 programs run as child "
                       "processes here; ENOEXEC");
        return kENOEXEC;
    }
    return 0;
}

// The program `file` names, looked up in PATH when it has no '/' (posix_spawnp, execvp).
int resolve_program(const char* file, bool search, std::string* path, std::wstring* host) {
    if (!file || !*file) return kENOENT;
    if (!search || std::strchr(file, '/')) {
        *path = file;
        return check_program(file, host);
    }
    const char* env_path = nullptr;
    for (char** e = guest_environ(); e && *e; ++e) {
        if (std::strncmp(*e, "PATH=", 5) == 0) env_path = *e + 5;
    }
    std::string dirs = env_path ? env_path : "/system/bin";
    int err = kENOENT;
    for (size_t at = 0; at <= dirs.size();) {
        size_t end = dirs.find(':', at);
        if (end == std::string::npos) end = dirs.size();
        std::string candidate = dirs.substr(at, end - at);
        if (candidate.empty()) candidate = ".";
        candidate += "/";
        candidate += file;
        const int e = check_program(candidate.c_str(), host);
        if (e == 0) {
            *path = candidate;
            return 0;
        }
        if (e != kENOENT) err = e;
        at = end + 1;
    }
    return err;
}

// Starts the program: 0 and *pid, or an errno.
int spawn(const std::string& path, const std::wstring& host, char* const* argv, char* const* envp,
          const FileActions* actions, int* pid) {
    wchar_t self[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    std::wstring exec(self, n);
    exec.resize(exec.find_last_of(L"\\/") + 1);
    exec += L"winapk-exec.exe";
    if (GetFileAttributesW(exec.c_str()) == INVALID_FILE_ATTRIBUTES) {
        static std::atomic<bool> said{false};
        say_once(said, "no winapk-exec.exe beside the runtime: an app's child process cannot start");
        return kENOENT;
    }
    // The child's standard streams after the file actions.
    Source src[3];
    for (int i = 0; i < 3; ++i) src[i].fd = i;
    auto at = [&](int fd) -> Source* { return fd >= 0 && fd < 3 ? &src[fd] : nullptr; };
    if (actions) {
        for (const FileAction& a : actions->actions) {
            if (a.kind == FileAction::kDup2) {
                if (Source* to = at(a.newfd)) {
                    if (const Source* from = at(a.fd)) *to = *from;
                    else *to = Source{Source::kParent, a.fd, {}, 0};
                }
            } else if (a.kind == FileAction::kClose) {
                if (Source* s = at(a.fd)) s->kind = Source::kClosed;
            } else if (Source* s = at(a.fd)) {
                *s = Source{Source::kFile, -1, a.path, a.flags};
            }
        }
    }
    HANDLE std_handles[3];
    for (int i = 0; i < 3; ++i) {
        std_handles[i] = handle_for(src[i], i == 0);
        if (std_handles[i] == INVALID_HANDLE_VALUE || !std_handles[i]) {
            for (int k = 0; k < i; ++k) CloseHandle(std_handles[k]);
            return src[i].kind == Source::kFile ? kENOENT : kEBADF;
        }
        SetHandleInformation(std_handles[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    // The command line: the shim's mapping of Android's paths, the environment, the program, its argv.
    std::wstring cmd = quote(exec) + L" --android-root " + quote(android_root());
    if (!app_package().empty()) cmd += L" --app " + quote(wide(app_package())) + L" " + quote(app_data_dir());
    for (char* const* e = envp; e && *e; ++e) cmd += L" --env " + quote(wide(*e));
    cmd += L" -- " + quote(host);
    for (char* const* a = argv; a && *a; ++a) cmd += L" " + quote(wide(*a));
    if (!argv || !*argv) cmd += L" " + quote(wide(path));
    // Only the three handles are inherited.
    std::vector<HANDLE> list;
    for (HANDLE h : std_handles) {
        if (std::find(list.begin(), list.end(), h) == list.end()) list.push_back(h);
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> attr_mem(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_mem.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list.data(), list.size() * sizeof(HANDLE),
                              nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = std_handles[0];
    si.StartupInfo.hStdOutput = std_handles[1];
    si.StartupInfo.hStdError = std_handles[2];
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');
    const BOOL ok = CreateProcessW(exec.c_str(), line.data(), nullptr, nullptr, TRUE,
                                   EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &si.StartupInfo, &pi);
    const DWORD error = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    for (HANDLE h : list) CloseHandle(h);
    if (!ok) {
        std::fprintf(stderr, "[win.apk] cannot start the child process %s (error %lu)\n", path.c_str(),
                     static_cast<unsigned long>(error));
        return error == ERROR_NOT_ENOUGH_MEMORY ? kENOMEM : kEAGAIN;
    }
    CloseHandle(pi.hThread);
    *pid = static_cast<int>(pi.dwProcessId);
    std::lock_guard<std::mutex> hold(g_children_lock);
    g_children[*pid].process = pi.hProcess;
    return 0;
}

// ---- posix_spawn --------------------------------------------------------------------------------

int do_spawn(int* pid, const char* file, void* const* actions, char* const* argv, char* const* envp, bool search) {
    std::string path;
    std::wstring host;
    const int err = resolve_program(file, search, &path, &host);
    if (err) return err;
    int child = 0;
    const int r = spawn(path, host, argv, envp ? envp : guest_environ(),
                        actions && *actions ? static_cast<const FileActions*>(*actions) : nullptr, &child);
    if (r == 0 && pid) *pid = child;
    return r;
}
WINAPK_GUEST_ABI int g_posix_spawn(int* pid, const char* path, void* const* actions, void* const*, char* const* argv,
                                   char* const* envp) {
    return do_spawn(pid, path, actions, argv, envp, false);
}
WINAPK_GUEST_ABI int g_posix_spawnp(int* pid, const char* file, void* const* actions, void* const*, char* const* argv,
                                    char* const* envp) {
    return do_spawn(pid, file, actions, argv, envp, true);
}

WINAPK_GUEST_ABI int g_actions_init(void** a) {
    *a = new FileActions;
    return 0;
}
WINAPK_GUEST_ABI int g_actions_destroy(void** a) {
    delete static_cast<FileActions*>(*a);
    *a = nullptr;
    return 0;
}
FileActions* actions_of(void** a) {
    if (!a || !*a) return nullptr;
    return static_cast<FileActions*>(*a);
}
WINAPK_GUEST_ABI int g_actions_addopen(void** a, int fd, const char* path, int flags, unsigned mode) {
    FileActions* fa = actions_of(a);
    if (!fa || fd < 0 || !path) return kEINVAL;
    fa->actions.push_back({FileAction::kOpen, fd, -1, flags, mode, path});
    return 0;
}
WINAPK_GUEST_ABI int g_actions_addclose(void** a, int fd) {
    FileActions* fa = actions_of(a);
    if (!fa || fd < 0) return kEINVAL;
    fa->actions.push_back({FileAction::kClose, fd});
    return 0;
}
WINAPK_GUEST_ABI int g_actions_adddup2(void** a, int fd, int newfd) {
    FileActions* fa = actions_of(a);
    if (!fa || fd < 0 || newfd < 0) return kEINVAL;
    fa->actions.push_back({FileAction::kDup2, fd, newfd});
    return 0;
}
// addchdir and addfchdir (API 34): the child starts where winapk-exec starts it; accepted.
WINAPK_GUEST_ABI int g_actions_addchdir(void** a, const char*) { return actions_of(a) ? 0 : kEINVAL; }
WINAPK_GUEST_ABI int g_actions_addfchdir(void** a, int) { return actions_of(a) ? 0 : kEINVAL; }

WINAPK_GUEST_ABI int g_attr_init(void** a) {
    *a = new SpawnAttr;
    return 0;
}
WINAPK_GUEST_ABI int g_attr_destroy(void** a) {
    delete static_cast<SpawnAttr*>(*a);
    *a = nullptr;
    return 0;
}
WINAPK_GUEST_ABI int g_attr_setflags(void** a, short flags) {
    if (!a || !*a) return kEINVAL;
    static_cast<SpawnAttr*>(*a)->flags = flags;
    return 0;
}
WINAPK_GUEST_ABI int g_attr_getflags(void* const* a, short* flags) {
    if (!a || !*a || !flags) return kEINVAL;
    *flags = static_cast<SpawnAttr*>(*a)->flags;
    return 0;
}
WINAPK_GUEST_ABI int g_attr_setpgroup(void** a, int pgroup) {
    if (!a || !*a) return kEINVAL;
    static_cast<SpawnAttr*>(*a)->pgroup = pgroup;
    return 0;
}
// Signal masks, defaults, scheduling: a new process starts with them as Windows starts it.
WINAPK_GUEST_ABI int g_attr_accept(void** a, const void*) { return a && *a ? 0 : kEINVAL; }
WINAPK_GUEST_ABI int g_attr_get_empty(void* const* a, void* out) {
    if (!a || !*a || !out) return kEINVAL;
    std::memset(out, 0, 8);
    return 0;
}

// ---- the exec family ------------------------------------------------------------------------

[[noreturn]] void become(const std::string& path, const std::wstring& host, char* const* argv, char* const* envp) {
    int pid = 0;
    const int err = spawn(path, host, argv, envp, nullptr, &pid);
    if (err) {
        // spawn failed after the checks: exec has nothing to return to that could still be true
        std::fprintf(stderr, "[win.apk] exec(%s) failed to start: errno %d\n", path.c_str(), err);
        TerminateProcess(GetCurrentProcess(), 127);
    }
    HANDLE h;
    {
        std::lock_guard<std::mutex> hold(g_children_lock);
        h = g_children[pid].process;
    }
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), code);
    std::abort();
}

int do_exec(const char* file, char* const* argv, char* const* envp, bool search) {
    std::string path;
    std::wstring host;
    const int err = resolve_program(file, search, &path, &host);
    if (err) return *guest_errno() = err, -1;
    become(path, host, argv, envp ? envp : guest_environ());
}
WINAPK_GUEST_ABI int g_execve(const char* path, char* const* argv, char* const* envp) {
    return do_exec(path, argv, envp, false);
}
WINAPK_GUEST_ABI int g_execv(const char* path, char* const* argv) { return do_exec(path, argv, nullptr, false); }
WINAPK_GUEST_ABI int g_execvp(const char* file, char* const* argv) { return do_exec(file, argv, nullptr, true); }
WINAPK_GUEST_ABI int g_execvpe(const char* file, char* const* argv, char* const* envp) {
    return do_exec(file, argv, envp, true);
}
// The list forms (execl, execlp, execle): variadic, their arguments in the System V argument
// registers then on the stack as the fixed ones would be; read as fixed parameters, up to the NULL
// that ends them (execle's envp follows it).
#define WINAPK_EXEC_ARGS                                                                              \
    const char* a0, const char* a1, const char* a2, const char* a3, const char* a4, const char* s0,   \
        const char* s1, const char* s2, const char* s3, const char* s4, const char* s5, const char* s6, \
        const char* s7, const char* s8, const char* s9, const char* s10
std::vector<char*> list_args(std::initializer_list<const char*> all, char* const** envp_after) {
    std::vector<char*> out;
    bool ended = false;
    for (const char* a : all) {
        if (ended) {
            if (envp_after) *envp_after = reinterpret_cast<char* const*>(const_cast<char*>(a));
            break;
        }
        if (!a) {
            ended = true;
            if (!envp_after) break;
            continue;
        }
        out.push_back(const_cast<char*>(a));
    }
    out.push_back(nullptr);
    return out;
}
WINAPK_GUEST_ABI int g_execl(const char* path, WINAPK_EXEC_ARGS) {
    auto v = list_args({a0, a1, a2, a3, a4, s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10}, nullptr);
    return do_exec(path, v.data(), nullptr, false);
}
WINAPK_GUEST_ABI int g_execlp(const char* file, WINAPK_EXEC_ARGS) {
    auto v = list_args({a0, a1, a2, a3, a4, s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10}, nullptr);
    return do_exec(file, v.data(), nullptr, true);
}
WINAPK_GUEST_ABI int g_execle(const char* path, WINAPK_EXEC_ARGS) {
    char* const* envp = nullptr;
    auto v = list_args({a0, a1, a2, a3, a4, s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10}, &envp);
    return do_exec(path, v.data(), envp, false);
}
#undef WINAPK_EXEC_ARGS
WINAPK_GUEST_ABI int g_fexecve(int, char* const*, char* const*) { return *guest_errno() = kENOSYS, -1; }

// ---- waiting --------------------------------------------------------------------------------

// Reaps (or with `keep`, only looks at) a child that ended: `pid` > 0 that one, else any. 0 when
// `nohang` and none has ended; -1 with errno ECHILD when there is no such child.
int reap(int pid, bool nohang, bool keep, int* status) {
    std::vector<std::pair<int, HANDLE>> candidates;
    {
        std::lock_guard<std::mutex> hold(g_children_lock);
        for (const auto& [p, c] : g_children) {
            if (pid <= 0 || p == pid) candidates.push_back({p, c.process});
        }
    }
    if (candidates.empty()) return *guest_errno() = kECHILD, -1;
    for (;;) {
        // Up to 64 at a time (WaitForMultipleObjects' limit), polled in turn beyond that.
        for (size_t base = 0; base < candidates.size(); base += MAXIMUM_WAIT_OBJECTS) {
            const size_t n = std::min<size_t>(MAXIMUM_WAIT_OBJECTS, candidates.size() - base);
            std::vector<HANDLE> hs;
            for (size_t i = 0; i < n; ++i) hs.push_back(candidates[base + i].second);
            const bool last = base + n >= candidates.size();
            const DWORD wait = (nohang || !last || candidates.size() > MAXIMUM_WAIT_OBJECTS) ? 0 : INFINITE;
            const DWORD r = WaitForMultipleObjects(static_cast<DWORD>(n), hs.data(), FALSE, wait);
            if (r < WAIT_OBJECT_0 + n) {
                const auto [p, h] = candidates[base + (r - WAIT_OBJECT_0)];
                DWORD code = 0;
                GetExitCodeProcess(h, &code);
                int signal = 0;
                {
                    std::lock_guard<std::mutex> hold(g_children_lock);
                    auto it = g_children.find(p);
                    if (it != g_children.end()) {
                        signal = it->second.signal;
                        if (!keep) {
                            CloseHandle(it->second.process);
                            g_children.erase(it);
                        }
                    }
                }
                if (status) *status = status_of(code, signal);
                return p;
            }
        }
        if (nohang) return 0;
        if (candidates.size() > MAXIMUM_WAIT_OBJECTS) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

WINAPK_GUEST_ABI int g_waitpid(int pid, int* status, int options) {
    return reap(pid, (options & kWNOHANG) != 0, false, status);
}
WINAPK_GUEST_ABI int g_wait(int* status) { return reap(-1, false, false, status); }
WINAPK_GUEST_ABI int g_wait4(int pid, int* status, int options, void* rusage) {
    if (rusage) std::memset(rusage, 0, 144);  // struct rusage: no accounting of a child here
    return reap(pid, (options & kWNOHANG) != 0, false, status);
}
WINAPK_GUEST_ABI int g_wait3(int* status, int options, void* rusage) { return g_wait4(-1, status, options, rusage); }
// siginfo_t for SIGCHLD: si_signo, si_errno, si_code, then si_pid at 16, si_uid at 20, si_status at 24.
WINAPK_GUEST_ABI int g_waitid(int idtype, int id, void* info, int options) {
    if (idtype != kPAll && idtype != kPPid) return *guest_errno() = kEINVAL, -1;
    if (!(options & kWEXITED)) return *guest_errno() = kEINVAL, -1;
    int status = 0;
    const int pid = reap(idtype == kPPid ? id : -1, (options & kWNOHANG) != 0, (options & kWNOWAIT) != 0, &status);
    if (pid < 0) return -1;
    if (info) {
        std::memset(info, 0, 128);
        if (pid > 0) {
            auto* p = static_cast<unsigned char*>(info);
            const bool killed = (status & 0x7f) != 0;
            const int signo = kSIGCHLD, code = killed ? 2 : 1;  // CLD_KILLED, CLD_EXITED
            const int value = killed ? (status & 0x7f) : ((status >> 8) & 0xff);
            std::memcpy(p, &signo, 4);
            std::memcpy(p + 8, &code, 4);
            std::memcpy(p + 16, &pid, 4);
            std::memcpy(p + 24, &value, 4);
        }
    }
    return 0;
}

const ShimSymbol kSymbols[] = {
    WINAPK_SHIM("posix_spawn", g_posix_spawn),
    WINAPK_SHIM("posix_spawnp", g_posix_spawnp),
    WINAPK_SHIM("posix_spawn_file_actions_init", g_actions_init),
    WINAPK_SHIM("posix_spawn_file_actions_destroy", g_actions_destroy),
    WINAPK_SHIM("posix_spawn_file_actions_addopen", g_actions_addopen),
    WINAPK_SHIM("posix_spawn_file_actions_addclose", g_actions_addclose),
    WINAPK_SHIM("posix_spawn_file_actions_adddup2", g_actions_adddup2),
    WINAPK_SHIM("posix_spawn_file_actions_addchdir_np", g_actions_addchdir),
    WINAPK_SHIM("posix_spawn_file_actions_addfchdir_np", g_actions_addfchdir),
    WINAPK_SHIM("posix_spawnattr_init", g_attr_init),
    WINAPK_SHIM("posix_spawnattr_destroy", g_attr_destroy),
    WINAPK_SHIM("posix_spawnattr_setflags", g_attr_setflags),
    WINAPK_SHIM("posix_spawnattr_getflags", g_attr_getflags),
    WINAPK_SHIM("posix_spawnattr_setpgroup", g_attr_setpgroup),
    WINAPK_SHIM("posix_spawnattr_setsigmask", g_attr_accept),
    WINAPK_SHIM("posix_spawnattr_setsigmask64", g_attr_accept),
    WINAPK_SHIM("posix_spawnattr_setsigdefault", g_attr_accept),
    WINAPK_SHIM("posix_spawnattr_setsigdefault64", g_attr_accept),
    WINAPK_SHIM("posix_spawnattr_setschedparam", g_attr_accept),
    WINAPK_SHIM("posix_spawnattr_setschedpolicy", g_attr_accept),
    WINAPK_SHIM("posix_spawnattr_getsigmask", g_attr_get_empty),
    WINAPK_SHIM("posix_spawnattr_getsigdefault", g_attr_get_empty),
    WINAPK_SHIM("execve", g_execve),
    WINAPK_SHIM("execv", g_execv),
    WINAPK_SHIM("execvp", g_execvp),
    WINAPK_SHIM("execvpe", g_execvpe),
    WINAPK_SHIM("execl", g_execl),
    WINAPK_SHIM("execlp", g_execlp),
    WINAPK_SHIM("execle", g_execle),
    WINAPK_SHIM("fexecve", g_fexecve),
    WINAPK_SHIM("waitpid", g_waitpid),
    WINAPK_SHIM("wait", g_wait),
    WINAPK_SHIM("wait3", g_wait3),
    WINAPK_SHIM("wait4", g_wait4),
    WINAPK_SHIM("waitid", g_waitid),
};

}  // namespace

bool child_kill(int pid, int sig, int* result) {
    std::lock_guard<std::mutex> hold(g_children_lock);
    auto it = g_children.find(pid);
    if (it == g_children.end()) return false;
    if (sig == 0) {  // does it exist: until reaped, a zombie does
        *result = 0;
        return true;
    }
    // Windows has no signals between processes: the child ends, as SIGKILL or an unhandled
    // SIGTERM/SIGINT would end it.
    if (WaitForSingleObject(it->second.process, 0) == WAIT_TIMEOUT) {
        it->second.signal = sig;
        TerminateProcess(it->second.process, 128 + static_cast<UINT>(sig));
    }
    *result = 0;
    (void)kSIGKILL;
    return true;
}

ShimTable process_symbols() { return WINAPK_SHIM_TABLE(kSymbols); }

}  // namespace winapk::bionic
