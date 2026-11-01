// crash_report.cpp -- see crash_report.h.

#include "crash_report.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <set>
#include <string>

#include <windows.h>

#include "winapk/linker.h"

namespace winapk::host {

namespace {

std::mutex g_lock;
std::set<uint64_t> g_reported;  // one report per faulting address

// "libfoo.so+0x1234 (symbol)" for a guest address, "bar.dll+0x1234" for a host
// one, "" for neither. Must not allocate much: it runs inside the handler.
bool describe(const void* addr, char* out, size_t n) {
    if (winapk::GuestLinker* linker = winapk::GuestLinker::process()) {
        winapk::GuestLinker::AddrInfo info{};
        if (linker->describe(addr, &info) && info.file) {
            const char* name = std::strrchr(info.file, '\\');
            name = name ? name + 1 : info.file;
            const auto off = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(info.base);
            if (info.symbol) {
                std::snprintf(out, n, "%s+0x%llx (%s+0x%llx)", name, static_cast<unsigned long long>(off), info.symbol,
                              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) -
                                                              reinterpret_cast<uintptr_t>(info.symbol_addr)));
            } else {
                std::snprintf(out, n, "%s+0x%llx", name, static_cast<unsigned long long>(off));
            }
            return true;
        }
    }
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod) && mod) {
        wchar_t path[MAX_PATH];
        const DWORD len = GetModuleFileNameW(mod, path, MAX_PATH);
        const wchar_t* name = path;
        for (DWORD i = 0; i < len; ++i) {
            if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
        }
        std::snprintf(out, n, "%ls+0x%llx", name,
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod)));
        return true;
    }
    return false;
}

bool in_guest(const void* addr) {
    winapk::GuestLinker* linker = winapk::GuestLinker::process();
    return linker && linker->image_containing(addr);
}

// HotSpot's JIT faults on purpose (implicit null checks, safepoint polls) and
// handles its own; those are in code no module owns, or in jvm.dll. A fault
// is worth a line when it is in a guest image, or in a module outside the JVM
// and Windows: the runtime's own, ANGLE's, a stub DLL.
bool worth_reporting(const void* rip) {
    if (in_guest(rip)) return true;
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(rip), &mod) || !mod) {
        return false;  // JIT code, or nowhere
    }
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(mod, path, MAX_PATH);
    wchar_t lower[MAX_PATH];
    for (size_t i = 0; i < MAX_PATH; ++i) {
        lower[i] = static_cast<wchar_t>(towlower(path[i]));
        if (!path[i]) break;
    }
    if (std::wcsstr(lower, L"\\jvm.dll") || std::wcsstr(lower, L"\\java.dll")) return false;
    wchar_t windir[MAX_PATH];
    const UINT wn = GetSystemWindowsDirectoryW(windir, MAX_PATH);
    if (wn && wn < MAX_PATH) {
        for (UINT i = 0; i < wn; ++i) windir[i] = static_cast<wchar_t>(towlower(windir[i]));
        if (std::wcsncmp(lower, windir, wn) == 0) return false;
    }
    return true;
}

const char* code_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
        default: return nullptr;
    }
}

// The frames above `rsp`: guest code has no unwind tables, so the stack is
// scanned for values that are code addresses in a guest image or a reportable
// module. Return addresses stand out; the rest is noise a reader can discount.
// Returns how many were found; `print` false only counts guest ones.
int scan_frames(uint64_t rsp, bool print) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(rsp), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return 0;
    const auto* top = reinterpret_cast<const uint64_t*>(rsp);
    const auto* end = reinterpret_cast<const uint64_t*>(static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize);
    int found = 0;
    for (const uint64_t* p = top; p < end && p < top + 4096 && found < 12; ++p) {
        const void* v = reinterpret_cast<const void*>(*p);
        if (!v) continue;
        if (print ? !worth_reporting(v) : !in_guest(v)) continue;
        char frame[512];
        if (!describe(v, frame, sizeof(frame))) continue;
        if (print) std::fprintf(stderr, "[win.apk]   stack[%+lld]: %s\n", static_cast<long long>((p - top) * 8), frame);
        ++found;
    }
    return found;
}

LONG CALLBACK report_fault(EXCEPTION_POINTERS* e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    const char* name = code_name(code);
    if (!name) return EXCEPTION_CONTINUE_SEARCH;
    const auto rip = static_cast<uint64_t>(e->ContextRecord->Rip);
    const auto rsp = static_cast<uint64_t>(e->ContextRecord->Rsp);
    // A fault inside a Windows DLL or the JVM is the JVM's own business unless
    // guest code is on the stack: memcpy on a guest pointer, a host function
    // the shim called with the guest's arguments (Pixel Wheels died in one
    // with nothing in the log).
    const bool own = worth_reporting(reinterpret_cast<const void*>(rip));
    if (!own && scan_frames(rsp, false) == 0) return EXCEPTION_CONTINUE_SEARCH;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_reported.insert(rip).second) return EXCEPTION_CONTINUE_SEARCH;
    }
    char where[512] = "?";
    describe(reinterpret_cast<const void*>(rip), where, sizeof(where));
    char line[1024];
    if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2) {
        const char* kind = e->ExceptionRecord->ExceptionInformation[0] == 0   ? "reading"
                           : e->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing"
                                                                              : "executing";
        std::snprintf(line, sizeof(line), "[win.apk] crash: %s %s 0x%llx at %s%s (rip 0x%llx, thread %lu)\n", name,
                      kind, static_cast<unsigned long long>(e->ExceptionRecord->ExceptionInformation[1]), where,
                      own ? "" : " (a host function, called from guest code)", static_cast<unsigned long long>(rip),
                      GetCurrentThreadId());
    } else {
        std::snprintf(line, sizeof(line), "[win.apk] crash: %s at %s%s (rip 0x%llx, thread %lu)\n", name, where,
                      own ? "" : " (a host function, called from guest code)", static_cast<unsigned long long>(rip),
                      GetCurrentThreadId());
    }
    std::fputs(line, stderr);
    scan_frames(rsp, true);
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

void on_abort(int) {
    std::fputs("[win.apk] host abort(): the runtime's own code or a host library called abort\n", stderr);
    void* frames[32];
    const USHORT n = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        char where[512];
        if (describe(frames[i], where, sizeof(where))) std::fprintf(stderr, "[win.apk]   #%u %s\n", i, where);
    }
    std::fflush(stderr);
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}

void on_terminate() {
    const char* what = "unknown";
    try {
        if (std::exception_ptr p = std::current_exception()) std::rethrow_exception(p);
    } catch (const std::exception& ex) {
        what = ex.what();
    } catch (...) {
    }
    std::fprintf(stderr, "[win.apk] host terminate(): uncaught C++ exception: %s\n", what);
    std::fflush(stderr);
    std::abort();
}

}  // namespace

void install_crash_report() {
    static std::once_flag once;
    std::call_once(once, [] {
        AddVectoredExceptionHandler(0, report_fault);  // 0: after every handler added before it
        signal(SIGABRT, on_abort);
        std::set_terminate(on_terminate);
    });
}

}  // namespace winapk::host
