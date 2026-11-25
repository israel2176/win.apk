// machine_load.cpp -- see machine_load.h.

#include "machine_load.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

namespace winapk::host::machine_load {

namespace {

// NtQuerySystemInformation(SystemProcessInformation)'s entry, as far as the image name and pid:
// winternl.h hides the times in Reserved fields. The layout is the documented x64 one.
struct ProcessEntry {
    ULONG next;
    ULONG threads;
    LARGE_INTEGER working_set_private;
    ULONG hard_faults;
    ULONG threads_high_watermark;
    ULONGLONG cycle_time;
    LARGE_INTEGER create_time;
    LARGE_INTEGER user_time;
    LARGE_INTEGER kernel_time;
    USHORT name_length;  // UNICODE_STRING ImageName, bytes
    USHORT name_max;
    PWSTR name;
    LONG base_priority;
    HANDLE pid;
};
static_assert(offsetof(ProcessEntry, pid) == 80, "SYSTEM_PROCESS_INFORMATION's x64 layout");

using QueryFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
constexpr ULONG kSystemProcessInformation = 5;

uint64_t u64(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

std::string utf8(const wchar_t* s, int n) {
    if (!s || n <= 0) return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, s, n, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len > 0 ? len : 0), '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, s, n, &out[0], len, nullptr, nullptr);
    return out;
}

bool take(Sample* s) {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return false;
    s->idle = u64(idle);
    s->total = u64(kernel) + u64(user);
    static const auto query = reinterpret_cast<QueryFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    if (!query) return true;  // the machine's share alone
    std::vector<uint8_t> buf(512 * 1024);
    for (int tries = 0; tries < 4; ++tries) {
        ULONG need = 0;
        const LONG st = query(kSystemProcessInformation, buf.data(), static_cast<ULONG>(buf.size()), &need);
        if (st == 0) break;
        if (st != static_cast<LONG>(0xC0000004L)) return true;  // not STATUS_INFO_LENGTH_MISMATCH
        buf.assign(std::max<size_t>(need + 64 * 1024, buf.size() * 2), 0);
        if (tries == 3) return true;
    }
    for (size_t off = 0;;) {
        if (off + sizeof(ProcessEntry) > buf.size()) break;
        const auto* e = reinterpret_cast<const ProcessEntry*>(buf.data() + off);
        Proc p;
        p.pid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->pid));
        p.created = static_cast<uint64_t>(e->create_time.QuadPart);
        p.cpu = static_cast<uint64_t>(e->user_time.QuadPart) + static_cast<uint64_t>(e->kernel_time.QuadPart);
        p.name = utf8(e->name, e->name_length / 2);
        s->procs.push_back(std::move(p));
        if (e->next == 0) break;
        off += e->next;
    }
    return true;
}

uint64_t own_cpu() {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    return u64(kernel) + u64(user);
}

std::mutex g_lock;
Sample g_start;
uint64_t g_start_own = 0;
bool g_marked = false;
std::atomic<bool> g_logged{false};

}  // namespace

void mark() {
    std::lock_guard<std::mutex> hold(g_lock);
    g_marked = take(&g_start);
    g_start_own = own_cpu();
}

void log_first_frame() {
    if (g_logged.exchange(true)) return;
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_marked) return;
    Sample now;
    if (!take(&now)) return;
    const uint64_t own = own_cpu();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::string line = describe(g_start, now, GetCurrentProcessId(), own > g_start_own ? own - g_start_own : 0,
                                      static_cast<int>(si.dwNumberOfProcessors), "to the first frame");
    if (line.empty()) return;
    std::fprintf(stderr, "[win.apk] startup: %s\n", line.c_str());
    std::fflush(stderr);
    g_start = Sample();  // ~150 processes' names: not kept for the run
}

}  // namespace winapk::host::machine_load
