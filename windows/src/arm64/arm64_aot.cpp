// arm64_aot.cpp -- loading and running arm64 code translated ahead of time.
// See include/winapk/arm64_aot.h.

#include "winapk/arm64_aot.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

extern "C" __attribute__((sysv_abi)) void winapk_arm64_call_sysv(const void* fn, void* frame);  // arm64_guest.cpp

namespace winapk::arm64 {

namespace {

// Guest address -> translated function. Read on every entry, written when a
// library loads: readers take the current map without a lock, a load
// publishes a new one (the old ones stay, as readers may still hold them).
using FunctionMap = std::unordered_map<uint64_t, AotFn>;
SRWLOCK g_load_lock = SRWLOCK_INIT;
std::atomic<const FunctionMap*> g_functions{new FunctionMap};

// A call or jump the translation could not resolve statically.
uint32_t runtime_branch(Cpu* c, uint64_t target) {
    if (AotFn fn = aot_lookup(target)) {
        c->pc = target;
        return fn(c);
    }
    uint64_t lo, hi;
    if (hooks().code_range(target, &lo, &hi)) {
        // arm64 code without a translation: the JIT runs it, and the rest of
        // this guest call with it.
        c->pc = target;
        return kAotBail;
    }
    // A host function (the Bionic shim, GL, JNI, ...) or setjmp/longjmp.
    const uint64_t ret = c->x[30];
    c->pc = target;
    hooks().host_call(*c, target);
    return c->pc == ret ? kAotDone : kAotBail;  // longjmp lands elsewhere
}

void runtime_step(Cpu* c) { step(*c); }

uint32_t runtime_plain(uint64_t target) { return hooks().plain_host && hooks().plain_host(target); }

// Every arm64 library mapped, for the profile and for translations that
// arrive while the app runs. Guarded by g_libs_lock.
struct Library {
    std::string so;  // UTF-8
    uint64_t bias = 0, text_hash = 0, lo = 0, hi = 0;
    bool translated = false;      // a DLL is loaded for it
    bool profile_read = false;    // the profile file's addresses are in `seen`
    std::unordered_set<uint64_t> seen;  // library vaddrs recorded (this run and the file)
    std::vector<uint64_t> pending;      // recorded, not in the file yet
};
SRWLOCK g_libs_lock = SRWLOCK_INIT;
std::vector<std::unique_ptr<Library>> g_libs;

std::filesystem::path sibling(const std::string& so, const char* suffix) {
    return std::filesystem::u8path(so + suffix);
}

bool load_dll(const std::filesystem::path& dll, uint64_t bias, uint64_t text_hash);

// ---- naming a fault in translated code ---------------------------------------------
// A bad guest pointer in translated code is a native access violation in the
// DLL, and the process died with only "c0000005 in <lib>.so.aot.dll+offset"
// in the Windows event log. Each loaded DLL's range and functions are kept
// so a fault can be reported as the guest function it is in; the handler
// only reports (once per place), and the fault goes on to whoever handles it.
struct LoadedDll {
    uint64_t lo, hi;
    std::string so;
    std::vector<std::pair<uint64_t, uint64_t>> fns;  // (host address, guest vaddr), sorted
};
SRWLOCK g_dlls_lock = SRWLOCK_INIT;
std::vector<LoadedDll>* g_dlls = new std::vector<LoadedDll>;

LONG CALLBACK report_aot_fault(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t rip = e->ContextRecord->Rip;
    char line[512] = {};
    AcquireSRWLockShared(&g_dlls_lock);
    for (const LoadedDll& d : *g_dlls) {
        if (rip < d.lo || rip >= d.hi) continue;
        auto it = std::upper_bound(d.fns.begin(), d.fns.end(), std::make_pair(rip, UINT64_MAX));
        const ULONG_PTR* info = e->ExceptionRecord->ExceptionInformation;
        if (it == d.fns.begin()) break;
        --it;
        std::snprintf(line, sizeof line,
                      "[win.apk] arm64 AOT: access violation %s 0x%llx in %s, guest function 0x%llx "
                      "(translated code +0x%llx)\n",
                      info[0] == 1 ? "writing" : info[0] == 8 ? "executing" : "reading",
                      static_cast<unsigned long long>(info[1]), d.so.c_str(),
                      static_cast<unsigned long long>(it->second), static_cast<unsigned long long>(rip - it->first));
        break;
    }
    ReleaseSRWLockShared(&g_dlls_lock);
    if (line[0]) {
        static std::atomic<uint64_t> last{0};
        if (last.exchange(rip) != rip) {  // not again for a fault that repeats
            DWORD n;
            WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(std::strlen(line)), &n, nullptr);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void remember_dll(HMODULE h, const AotModule* m, const std::filesystem::path& dll) {
    static std::once_flag handler;
    std::call_once(handler, [] { AddVectoredExceptionHandler(0, report_aot_fault); });
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(h) + dos->e_lfanew);
    LoadedDll d;
    d.lo = reinterpret_cast<uint64_t>(h);
    d.hi = d.lo + nt->OptionalHeader.SizeOfImage;
    d.so = dll.stem().u8string();  // <lib>.so.aot.dll -> <lib>.so.aot
    if (d.so.size() > 4 && d.so.compare(d.so.size() - 4, 4, ".aot") == 0) d.so.resize(d.so.size() - 4);
    for (uint64_t i = 0; i < m->count; ++i) {
        d.fns.emplace_back(reinterpret_cast<uint64_t>(m->entries[i].fn), m->entries[i].vaddr);
    }
    std::sort(d.fns.begin(), d.fns.end());
    AcquireSRWLockExclusive(&g_dlls_lock);
    g_dlls->push_back(std::move(d));
    ReleaseSRWLockExclusive(&g_dlls_lock);
}

}  // namespace

bool aot_load(const char* so_path, uint64_t bias, uint64_t text_hash, uint64_t code_lo, uint64_t code_hi) {
    auto lib = std::make_unique<Library>();
    lib->so = so_path;
    lib->bias = bias;
    lib->text_hash = text_hash;
    lib->lo = code_lo;
    lib->hi = code_hi;
    lib->translated = load_dll(sibling(lib->so, ".aot.dll"), bias, text_hash);
    const bool translated = lib->translated;
    AcquireSRWLockExclusive(&g_libs_lock);
    g_libs.push_back(std::move(lib));
    ReleaseSRWLockExclusive(&g_libs_lock);
    return translated;
}

void aot_profile_hit(uint64_t pc) {
    AcquireSRWLockExclusive(&g_libs_lock);
    for (const auto& lib : g_libs) {
        if (pc < lib->lo || pc >= lib->hi) continue;
        if (lib->seen.insert(pc - lib->bias).second) lib->pending.push_back(pc - lib->bias);
        break;
    }
    ReleaseSRWLockExclusive(&g_libs_lock);
}

size_t aot_profile_flush() {
    struct Batch {
        Library* lib;
        std::string so;
        uint64_t text_hash;
        bool read;
        std::vector<uint64_t> hits;
    };
    std::vector<Batch> batches;
    AcquireSRWLockExclusive(&g_libs_lock);
    for (const auto& lib : g_libs) {
        if (lib->pending.empty()) continue;
        batches.push_back({lib.get(), lib->so, lib->text_hash, lib->profile_read, std::move(lib->pending)});
        lib->pending.clear();
        lib->profile_read = true;
    }
    ReleaseSRWLockExclusive(&g_libs_lock);

    size_t grew = 0;
    char header[64];
    for (Batch& b : batches) {
        const std::filesystem::path path = sibling(b.so, ".aot.profile");
        std::snprintf(header, sizeof header, "winapk-aot-profile 1 %" PRIx64, b.text_hash);
        bool fresh = true;  // no usable file: start one
        if (!b.read) {
            // First write this run: what earlier runs recorded is not news.
            std::ifstream in(path, std::ios::binary);
            std::string line;
            if (in && std::getline(in, line) && line == header) {
                fresh = false;
                std::unordered_set<uint64_t> old;
                while (std::getline(in, line)) {
                    if (!line.empty()) old.insert(std::strtoull(line.c_str(), nullptr, 16));
                }
                std::vector<uint64_t> news;
                for (uint64_t h : b.hits) {
                    if (!old.count(h)) news.push_back(h);
                }
                b.hits.swap(news);
                AcquireSRWLockExclusive(&g_libs_lock);
                b.lib->seen.insert(old.begin(), old.end());
                ReleaseSRWLockExclusive(&g_libs_lock);
            }
        } else {
            fresh = false;
        }
        if (b.hits.empty() && !fresh) continue;
        std::ofstream out(path, fresh ? std::ios::binary | std::ios::trunc : std::ios::binary | std::ios::app);
        if (fresh) out << header << '\n';
        char row[24];
        for (uint64_t h : b.hits) {
            std::snprintf(row, sizeof row, "%" PRIx64 "\n", h);
            out << row;
        }
        if (out && !b.hits.empty()) ++grew;
    }
    return grew;
}

size_t aot_load_pending() {
    struct Want {
        Library* lib;
        std::string so;
        uint64_t bias, text_hash;
    };
    std::vector<Want> want;
    AcquireSRWLockShared(&g_libs_lock);
    for (const auto& lib : g_libs) {
        if (!lib->translated) want.push_back({lib.get(), lib->so, lib->bias, lib->text_hash});
    }
    ReleaseSRWLockShared(&g_libs_lock);
    size_t loaded = 0;
    for (const Want& w : want) {
        std::error_code ec;
        bool ok = false;
        for (const char* suffix : {".aot.dll", ".aot.dll.new"}) {
            const std::filesystem::path dll = sibling(w.so, suffix);
            if (std::filesystem::exists(dll, ec) && (ok = load_dll(dll, w.bias, w.text_hash))) break;
        }
        if (!ok) continue;
        AcquireSRWLockExclusive(&g_libs_lock);
        w.lib->translated = true;
        ReleaseSRWLockExclusive(&g_libs_lock);
        std::fprintf(stderr, "[win.apk] arm64 AOT: %s: translation loaded while the app runs\n", w.so.c_str());
        ++loaded;
    }
    return loaded;
}

namespace {

bool load_dll(const std::filesystem::path& dll, uint64_t bias, uint64_t text_hash) {
    std::error_code ec;
    if (!std::filesystem::exists(dll, ec)) return false;
    HMODULE h = LoadLibraryW(dll.wstring().c_str());
    if (!h) {
        std::fprintf(stderr, "[win.apk] arm64 AOT: cannot load %s (error %lu)\n", dll.u8string().c_str(),
                     GetLastError());
        return false;
    }
    const auto* m = reinterpret_cast<const AotModule*>(GetProcAddress(h, "winapk_aot_module"));
    const uint32_t layout[4] = {offsetof(Cpu, sp), offsetof(Cpu, nzcv), offsetof(Cpu, tpidr), offsetof(Cpu, v)};
    const char* why = nullptr;
    if (!m) why = "no winapk_aot_module";
    else if (m->version != kAotVersion) why = "built by another version of winapk-aot";
    else if (std::memcmp(m->layout, layout, sizeof layout) != 0) why = "built for another CPU state layout";
    else if (m->text_hash != text_hash) why = "built from another build of the library";
    if (why) {
        std::fprintf(stderr, "[win.apk] arm64 AOT: ignoring %s: %s\n", dll.u8string().c_str(), why);
        FreeLibrary(h);
        return false;
    }
    *m->base = bias;
    m->runtime->branch = runtime_branch;
    m->runtime->step = runtime_step;
    m->runtime->plain = runtime_plain;
    m->runtime->call_sysv = winapk_arm64_call_sysv;
    AcquireSRWLockExclusive(&g_load_lock);
    auto* next = new FunctionMap(*g_functions.load());
    for (uint64_t i = 0; i < m->count; ++i) (*next)[bias + m->entries[i].vaddr] = m->entries[i].fn;
    g_functions.store(next, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_load_lock);
    remember_dll(h, m, dll);
    std::fprintf(stderr, "[win.apk] arm64 AOT: %s: %llu translated functions\n", dll.filename().u8string().c_str(),
                 static_cast<unsigned long long>(m->count));
    return true;
}

}  // namespace

AotFn aot_lookup(uint64_t pc) {
    const FunctionMap* map = g_functions.load(std::memory_order_acquire);
    auto it = map->find(pc);
    return it == map->end() ? nullptr : it->second;
}

uint32_t aot_run(AotFn fn, Cpu* c) { return fn(c); }

}  // namespace winapk::arm64
