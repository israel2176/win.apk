// debug_agent.cpp -- see debug_agent.h.

#include "debug_agent.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#include <jvmti.h>

#include "winapk/debug_protocol.h"
#include "winapk/linker.h"
#include "winapk/mini_bionic.h"

namespace winapk::host {

namespace {

JavaVM* g_vm = nullptr;
jvmtiEnv* g_jvmti = nullptr;  // the agent's own environment: its callbacks and thread-local storage are not the host's
bool g_line_numbers = false, g_source_file = false, g_contended = false, g_monitor_info = false;

using NtQueryThread = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
NtQueryThread nt_query_thread() {
    static const auto f = reinterpret_cast<NtQueryThread>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    return f;
}

std::string utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

HMODULE module_of(const void* addr) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &mod))
        return nullptr;
    return mod;
}

bool in_guest(const void* addr) {
    winapk::GuestLinker* linker = winapk::GuestLinker::process();
    return linker && linker->image_containing(addr);
}

// "libapp.so!symbol+0x10" or "libapp.so+0x1234" for a guest address, "module.dll+0x1234" for a host one,
// empty for an address no image holds (the JIT's code, a thunk).
std::string describe(const void* addr) {
    if (!addr) return {};
    if (winapk::GuestLinker* linker = winapk::GuestLinker::process()) {
        winapk::GuestLinker::AddrInfo info{};
        if (linker->describe(addr, &info) && info.file) {
            const char* name = info.file;
            for (const char* p = info.file; *p; ++p)
                if (*p == '\\' || *p == '/') name = p + 1;
            const auto a = reinterpret_cast<uintptr_t>(addr);
            if (info.symbol)
                return std::string(name) + "!" + info.symbol + "+" + hex(a - reinterpret_cast<uintptr_t>(info.symbol_addr));
            return std::string(name) + "+" + hex(a - reinterpret_cast<uintptr_t>(info.base));
        }
    }
    if (HMODULE mod = module_of(addr)) {
        wchar_t path[MAX_PATH];
        const DWORD len = GetModuleFileNameW(mod, path, MAX_PATH);
        const wchar_t* name = path;
        for (DWORD i = 0; i < len; ++i)
            if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
        return utf8(name) + "+" + hex(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod));
    }
    return {};
}

// ---- the process's threads, as Windows has them ---------------------------------------------------------

struct OsThread {
    DWORD tid = 0;
    std::string name;             // SetThreadDescription's
    const void* start = nullptr;  // the Win32 start address
};

std::vector<OsThread> os_threads() {
    std::vector<OsThread> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get_description = reinterpret_cast<GetDescription>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        OsThread t;
        t.tid = te.th32ThreadID;
        if (HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, t.tid)) {
            PWSTR desc = nullptr;
            if (get_description && SUCCEEDED(get_description(h, &desc)) && desc) {
                t.name = utf8(desc);
                LocalFree(desc);
            }
            void* start = nullptr;
            constexpr ULONG kThreadQuerySetWin32StartAddress = 9;
            if (nt_query_thread() &&
                nt_query_thread()(h, kThreadQuerySetWin32StartAddress, &start, sizeof(start), nullptr) == 0)
                t.start = start;
            CloseHandle(h);
        }
        out.push_back(std::move(t));
    }
    CloseHandle(snap);
    return out;
}

// ---- the Java threads ---------------------------------------------------------------------------------

// A new Java thread, on that thread: its Windows thread id goes into the agent's thread-local storage.
void JNICALL on_thread_start(jvmtiEnv* jvmti, JNIEnv*, jthread thread) {
    jvmti->SetThreadLocalStorage(thread, reinterpret_cast<void*>(static_cast<uintptr_t>(GetCurrentThreadId())));
}

struct JavaThread {
    jthread ref = nullptr;  // a local reference of the request's frame
    DWORD tid = 0;          // 0: not known
    jlong jid = -1;
    std::string name;
    bool daemon = false;
    jint state = 0;
};

const char* state_name(jint s) {
    if (!(s & JVMTI_THREAD_STATE_ALIVE)) return (s & JVMTI_THREAD_STATE_TERMINATED) ? "terminated" : "new";
    if (s & JVMTI_THREAD_STATE_BLOCKED_ON_MONITOR_ENTER) return "blocked on a monitor";
    const bool timed = (s & JVMTI_THREAD_STATE_WAITING_WITH_TIMEOUT) != 0;
    if (s & JVMTI_THREAD_STATE_SLEEPING) return "sleeping";
    if (s & JVMTI_THREAD_STATE_PARKED) return timed ? "parked (timed)" : "parked";
    if (s & JVMTI_THREAD_STATE_IN_OBJECT_WAIT) return timed ? "waiting (timed)" : "waiting";
    if (s & JVMTI_THREAD_STATE_WAITING) return "waiting";
    if (s & JVMTI_THREAD_STATE_IN_NATIVE) return "in native code";
    if (s & JVMTI_THREAD_STATE_RUNNABLE) return "running";
    return "alive";
}

jlong thread_id(JNIEnv* env, jthread t) {
    static jmethodID id = nullptr;
    if (!id) {
        jclass cls = env->FindClass("java/lang/Thread");
        if (cls) id = env->GetMethodID(cls, "threadId", "()J");
        if (!id) {
            env->ExceptionClear();
            if (cls) id = env->GetMethodID(cls, "getId", "()J");
        }
        if (!id) {
            env->ExceptionClear();
            return -1;
        }
    }
    const jlong v = env->CallLongMethod(t, id);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return -1;
    }
    return v;
}

std::vector<JavaThread> java_threads(JNIEnv* env) {
    std::vector<JavaThread> out;
    jint count = 0;
    jthread* threads = nullptr;
    if (g_jvmti->GetAllThreads(&count, &threads) != JVMTI_ERROR_NONE) return out;
    for (jint i = 0; i < count; ++i) {
        JavaThread t;
        t.ref = threads[i];
        jvmtiThreadInfo info{};
        if (g_jvmti->GetThreadInfo(t.ref, &info) == JVMTI_ERROR_NONE) {
            if (info.name) {
                t.name = info.name;
                g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(info.name));
            }
            t.daemon = info.is_daemon;
            if (info.thread_group) env->DeleteLocalRef(info.thread_group);
            if (info.context_class_loader) env->DeleteLocalRef(info.context_class_loader);
        }
        g_jvmti->GetThreadState(t.ref, &t.state);
        void* tls = nullptr;
        if (g_jvmti->GetThreadLocalStorage(t.ref, &tls) == JVMTI_ERROR_NONE)
            t.tid = static_cast<DWORD>(reinterpret_cast<uintptr_t>(tls));
        t.jid = thread_id(env, t.ref);
        out.push_back(std::move(t));
    }
    g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(threads));
    return out;
}

// The Java threads that started before the agent have no Windows thread id yet. HotSpot gives a Java
// thread's name to its Windows thread: where one such name is on one unclaimed thread of the JVM's, they
// are the same thread, and it is remembered.
void match_by_name(std::vector<JavaThread>& java, const std::vector<OsThread>& os) {
    const HMODULE jvm = GetModuleHandleW(L"jvm.dll");
    std::unordered_map<DWORD, bool> claimed;
    for (const JavaThread& j : java)
        if (j.tid) claimed[j.tid] = true;
    for (JavaThread& j : java) {
        if (j.tid || j.name.empty()) continue;
        if (std::count_if(java.begin(), java.end(),
                          [&](const JavaThread& o) { return !o.tid && o.name == j.name; }) != 1)
            continue;
        const OsThread* found = nullptr;
        int matches = 0;
        for (const OsThread& o : os) {
            if (claimed.count(o.tid) || o.name != j.name || !o.start || module_of(o.start) != jvm) continue;
            found = &o;
            ++matches;
        }
        if (matches != 1) continue;
        j.tid = found->tid;
        claimed[j.tid] = true;
        g_jvmti->SetThreadLocalStorage(j.ref, reinterpret_cast<void*>(static_cast<uintptr_t>(j.tid)));
    }
}

std::string answer_threads(JNIEnv* env) {
    std::string out = winapk::debug::format_header();
    const std::vector<OsThread> os = os_threads();
    std::vector<JavaThread> java = java_threads(env);
    match_by_name(java, os);

    std::unordered_map<DWORD, const JavaThread*> java_by_tid;
    for (const JavaThread& j : java)
        if (j.tid) java_by_tid[j.tid] = &j;
    std::unordered_map<DWORD, const void*> guest_start;
    {
        std::vector<winapk::GuestThread> guest(256);
        size_t n = winapk::guest_threads(guest.data(), guest.size());
        if (n > guest.size()) {
            guest.resize(n + 64);
            n = (std::min)(winapk::guest_threads(guest.data(), guest.size()), guest.size());
        }
        for (size_t i = 0; i < n; ++i) guest_start[guest[i].tid] = guest[i].start;
    }
    const HMODULE jvm = GetModuleHandleW(L"jvm.dll"), exe = GetModuleHandleW(nullptr);

    std::unordered_map<DWORD, bool> listed;
    for (const OsThread& o : os) {
        winapk::debug::AgentThread t;
        t.tid = o.tid;
        t.name = o.name;
        const auto g = guest_start.find(o.tid);
        t.start = describe(g != guest_start.end() ? g->second : o.start);
        const auto j = java_by_tid.find(o.tid);
        if (j != java_by_tid.end()) {
            t.kind = "java";
            t.jid = j->second->jid;
            t.daemon = j->second->daemon;
            t.state = state_name(j->second->state);
            t.name = j->second->name;
        } else if (g != guest_start.end()) {
            t.kind = "guest";
        } else {
            const HMODULE mod = o.start ? module_of(o.start) : nullptr;
            t.kind = mod && mod == jvm ? "jvm" : mod && mod == exe ? "runtime" : "system";
        }
        listed[o.tid] = true;
        out += winapk::debug::format_thread(t);
    }
    // A Java thread whose Windows thread is not known, or that started since the snapshot.
    for (const JavaThread& j : java) {
        if (j.tid && listed.count(j.tid)) continue;
        if (!(j.state & JVMTI_THREAD_STATE_ALIVE)) continue;
        winapk::debug::AgentThread t;
        t.tid = j.tid;
        t.kind = "java";
        t.jid = j.jid;
        t.daemon = j.daemon;
        t.state = state_name(j.state);
        t.name = j.name;
        out += winapk::debug::format_thread(t);
    }
    return out;
}

// ---- one thread's stack ---------------------------------------------------------------------------------

// "Lcom/app/Main$1;" -> "com.app.Main$1".
std::string class_name(jclass cls) {
    char* sig = nullptr;
    if (!cls || g_jvmti->GetClassSignature(cls, &sig, nullptr) != JVMTI_ERROR_NONE || !sig) return "?";
    std::string s = sig;
    g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(sig));
    if (s.size() >= 2 && s.front() == 'L' && s.back() == ';') s = s.substr(1, s.size() - 2);
    std::replace(s.begin(), s.end(), '/', '.');
    return s;
}

std::string java_frame(JNIEnv* env, const jvmtiFrameInfo& f) {
    std::string out = "  at ";
    jclass cls = nullptr;
    g_jvmti->GetMethodDeclaringClass(f.method, &cls);
    out += class_name(cls);
    char* name = nullptr;
    if (g_jvmti->GetMethodName(f.method, &name, nullptr, nullptr) == JVMTI_ERROR_NONE && name) {
        out += std::string(".") + name;
        g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(name));
    }
    jboolean native = JNI_FALSE;
    g_jvmti->IsMethodNative(f.method, &native);
    std::string where;
    if (native) {
        where = "Native Method";
    } else {
        char* file = nullptr;
        if (g_source_file && cls && g_jvmti->GetSourceFileName(cls, &file) == JVMTI_ERROR_NONE && file) {
            where = file;
            g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(file));
        }
        jint n = 0;
        jvmtiLineNumberEntry* table = nullptr;
        if (g_line_numbers && g_jvmti->GetLineNumberTable(f.method, &n, &table) == JVMTI_ERROR_NONE && table) {
            jint line = -1;
            for (jint i = 0; i < n; ++i)
                if (table[i].start_location <= f.location) line = table[i].line_number;
            g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(table));
            if (line >= 0) where += (where.empty() ? "line " : ":") + std::to_string(line);
        }
        if (where.empty()) where = "Unknown Source";
    }
    if (cls) env->DeleteLocalRef(cls);
    return out + "(" + where + ")\n";
}

std::string java_stack(JNIEnv* env, const JavaThread& t) {
    std::string out = "Java thread \"" + t.name + "\"" + (t.daemon ? " daemon" : "") + ", id " +
                      std::to_string(t.jid) + ": " + state_name(t.state) + "\n";
    jobject monitor = nullptr;
    if (g_contended && g_jvmti->GetCurrentContendedMonitor(t.ref, &monitor) == JVMTI_ERROR_NONE && monitor) {
        jclass cls = env->GetObjectClass(monitor);
        out += "  for the monitor of a " + class_name(cls);
        jvmtiMonitorUsage use{};
        if (g_monitor_info && g_jvmti->GetObjectMonitorUsage(monitor, &use) == JVMTI_ERROR_NONE) {
            if (use.owner) {
                jvmtiThreadInfo owner{};
                if (g_jvmti->GetThreadInfo(use.owner, &owner) == JVMTI_ERROR_NONE && owner.name) {
                    out += std::string(", held by \"") + owner.name + "\"";
                    g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(owner.name));
                }
            }
            if (use.waiters) g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(use.waiters));
            if (use.notify_waiters) g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(use.notify_waiters));
        }
        out += "\n";
    }
    static jvmtiFrameInfo frames[256];
    jint n = 0;
    const jvmtiError e = g_jvmti->GetStackTrace(t.ref, 0, 256, frames, &n);
    if (e != JVMTI_ERROR_NONE) return out + "  (no Java frames: JVMTI error " + std::to_string(e) + ")\n";
    if (n == 0) out += "  (no Java frames)\n";
    for (jint i = 0; i < n; ++i) out += java_frame(env, frames[i]);
    if (n == 256) out += "  ...\n";
    return out;
}

bool executable(const void* addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// What the thread's native stack shows. The thread is stopped only for as long as it takes to copy its
// registers and the top of its stack, with nothing allocated and no lock taken meanwhile: it may hold the
// heap's. The copy is read after it runs again.
std::string native_stack(DWORD tid) {
    if (tid == GetCurrentThreadId()) return {};
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) return "Native: the thread has ended\n";
    struct BasicInfo {  // THREAD_BASIC_INFORMATION
        LONG exit_status;
        void* teb;
        void* pid;
        void* tid;
        ULONG_PTR affinity;
        LONG priority, base_priority;
    } basic{};
    uintptr_t stack_base = 0;
    if (nt_query_thread() && nt_query_thread()(h, 0, &basic, sizeof(basic), nullptr) == 0 && basic.teb)
        stack_base = reinterpret_cast<uintptr_t>(static_cast<NT_TIB*>(basic.teb)->StackBase);
    constexpr size_t kWords = 8192;  // 64 KB of stack
    static uint64_t words[kWords];
    alignas(16) static CONTEXT ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    SIZE_T got = 0;
    bool ok = false;
    if (SuspendThread(h) != static_cast<DWORD>(-1)) {
        ok = GetThreadContext(h, &ctx) != 0;
        if (ok && stack_base > ctx.Rsp) {
            const size_t want = (std::min)(static_cast<size_t>(stack_base - ctx.Rsp), kWords * 8) & ~size_t{7};
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(ctx.Rsp), words, want, &got))
                got = 0;
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    if (!ok) return "Native: the thread's registers could not be read\n";

    auto line = [](const std::string& label, uint64_t addr) {
        const std::string what = describe(reinterpret_cast<const void*>(addr));
        return "  " + label + hex(addr) + "  " + (what.empty() ? "(compiled Java or generated code)" : what) + "\n";
    };
    std::string out = "Native:\n" + line("pc  ", ctx.Rip);
    const size_t n = got / 8;
    // The frames the frame pointers link: only as far as the code keeps one.
    uint64_t fp = ctx.Rbp;
    for (int depth = 0; depth < 64; ++depth) {
        if (fp < ctx.Rsp || (fp & 7) || (fp - ctx.Rsp) / 8 + 1 >= n) break;
        const size_t i = static_cast<size_t>((fp - ctx.Rsp) / 8);
        const uint64_t ret = words[i + 1], next = words[i];
        if (!ret || !executable(reinterpret_cast<const void*>(ret))) break;
        out += line("ret ", ret);
        if (next <= fp) break;
        fp = next;
    }
    // Guest code has no unwind tables and the JIT keeps no frame pointer: the stack's values that are code
    // addresses of an image. Return addresses stand out; the rest is noise a reader can discount.
    std::string scan;
    int found = 0;
    uint64_t last = 0;
    for (size_t i = 0; i < n && found < 48; ++i) {
        const uint64_t v = words[i];
        if (v < 0x10000 || v == last) continue;
        const void* p = reinterpret_cast<const void*>(v);
        if (!in_guest(p) && !(module_of(p) && executable(p))) continue;
        const std::string what = describe(p);
        if (what.empty()) continue;
        char at[32];
        std::snprintf(at, sizeof(at), "  [sp+0x%04x] ", static_cast<unsigned>(i * 8));
        scan += at + what + "\n";
        last = v;
        ++found;
    }
    if (!scan.empty()) out += "Code addresses on the stack (return addresses among them):\n" + scan;
    return out;
}

std::string answer_stack(JNIEnv* env, DWORD tid, jlong jid) {
    std::string out;
    std::vector<JavaThread> java = java_threads(env);
    const JavaThread* found = nullptr;
    for (const JavaThread& j : java) {
        if (tid ? j.tid == tid : (jid >= 0 && j.jid == jid)) found = &j;
    }
    if (!found && jid >= 0) {  // known by name only since the list was sent
        for (const JavaThread& j : java)
            if (j.jid == jid && (!j.tid || j.tid == tid)) found = &j;
    }
    if (found) out += java_stack(env, *found);
    if (tid) {
        if (!out.empty()) out += "\n";
        out += native_stack(tid);
    } else if (found) {
        out += "\nNative: which Windows thread runs this Java thread is not known (it started before the debug "
               "agent and its name is on none, or on several)\n";
    }
    if (out.empty()) out = "The thread has ended.\n";
    return out;
}

// ---- the pipe -------------------------------------------------------------------------------------------

std::string answer(JNIEnv* env, const std::string& question) {
    std::string out;
    if (env->PushLocalFrame(2048) != JNI_OK) {
        env->ExceptionClear();
        return "error: out of memory\n";
    }
    if (question == "threads") {
        out = answer_threads(env);
    } else if (question.compare(0, 6, "stack ") == 0) {
        unsigned long tid = 0;
        long long jid = -1;
        std::sscanf(question.c_str() + 6, "%lu %lld", &tid, &jid);
        out = answer_stack(env, static_cast<DWORD>(tid), static_cast<jlong>(jid));
    } else {
        out = "error: unknown question\n";
    }
    env->PopLocalFrame(nullptr);
    return out;
}

// One connection at a time, one question each. Only this user's processes can open the pipe (the default
// security of a pipe made by the user), and none from another machine; a client that connects and says
// nothing holds the agent, and nothing else.
DWORD WINAPI serve(void*) {
    JNIEnv* env = nullptr;
    static char name[] = "WinApk-Debug";
    JavaVMAttachArgs args{JNI_VERSION_1_8, name, nullptr};
    if (g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &args) != JNI_OK) return 1;
    const std::wstring pipe_name = winapk::debug::pipe_name(GetCurrentProcessId());
    auto listen = [&] {
        for (;;) {
            HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
                                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                           PIPE_UNLIMITED_INSTANCES, 1 << 16, 4096, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) return pipe;
            Sleep(1000);
        }
    };
    HANDLE pipe = listen();
    for (;;) {
        const bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected) {
            std::string question;
            char c = 0;
            DWORD got = 0;
            while (question.size() < 256 && ReadFile(pipe, &c, 1, &got, nullptr) && got == 1 && c != '\n')
                if (c != '\r') question += c;
            const std::string reply = answer(env, question);
            size_t sent = 0;
            while (sent < reply.size()) {
                DWORD wrote = 0;
                if (!WriteFile(pipe, reply.data() + sent, static_cast<DWORD>(reply.size() - sent), &wrote, nullptr) ||
                    !wrote)
                    break;
                sent += wrote;
            }
        }
        // The next connection's end exists before this one closes: the launcher's next question, sent the
        // moment it has read this answer to its end, always finds the pipe.
        HANDLE next = listen();
        if (connected) FlushFileBuffers(pipe);  // until the reader has it all, or is gone
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        pipe = next;
    }
}

}  // namespace

bool debug_mode_on() {
    wchar_t env[8];
    const DWORD n = GetEnvironmentVariableW(L"WINAPK_DEBUG", env, 8);
    if (n > 0) return !(n == 1 && env[0] == L'0');
    DWORD value = 0, size = sizeof value;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Win.apk", L"DebugMode", RRF_RT_REG_DWORD, nullptr, &value,
                        &size) == ERROR_SUCCESS &&
           value != 0;
}

void start_debug_agent(JavaVM* vm) {
    if (g_vm || !debug_mode_on()) return;
    jvmtiEnv* jvmti = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK || !jvmti) {
        std::fprintf(stderr, "[win.apk] debug mode: no JVMTI, no thread agent\n");
        return;
    }
    g_vm = vm;
    g_jvmti = jvmti;
    // Each one asked for alone: one the VM refuses takes nothing else with it.
    auto add = [&](void (*set)(jvmtiCapabilities&)) {
        jvmtiCapabilities caps{};
        set(caps);
        return jvmti->AddCapabilities(&caps) == JVMTI_ERROR_NONE;
    };
    g_line_numbers = add([](jvmtiCapabilities& c) { c.can_get_line_numbers = 1; });
    g_source_file = add([](jvmtiCapabilities& c) { c.can_get_source_file_name = 1; });
    g_contended = add([](jvmtiCapabilities& c) { c.can_get_current_contended_monitor = 1; });
    g_monitor_info = add([](jvmtiCapabilities& c) { c.can_get_monitor_info = 1; });
    jvmtiEventCallbacks callbacks{};
    callbacks.ThreadStart = &on_thread_start;
    jvmti->SetEventCallbacks(&callbacks, static_cast<jint>(sizeof(callbacks)));
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_THREAD_START, nullptr);
    // This thread created the VM: it is "main".
    jvmti->SetThreadLocalStorage(nullptr, reinterpret_cast<void*>(static_cast<uintptr_t>(GetCurrentThreadId())));
    HANDLE t = CreateThread(nullptr, 0, serve, nullptr, 0, nullptr);
    if (!t) {
        std::fprintf(stderr, "[win.apk] debug mode: the thread agent could not start (error %lu)\n", GetLastError());
        return;
    }
    CloseHandle(t);
    std::fprintf(stderr, "[win.apk] debug mode: the thread agent answers on \\\\.\\pipe\\winapk-debug-%lu\n",
                 GetCurrentProcessId());
}

}  // namespace winapk::host
