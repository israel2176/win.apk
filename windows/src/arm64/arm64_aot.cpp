// arm64_aot.cpp -- loading and running arm64 code translated ahead of time.
// See include/winapk/arm64_aot.h.

#include "winapk/arm64_aot.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_map>

#include <windows.h>

namespace winapk::arm64 {

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<uint64_t, AotFn> g_functions;  // guest address -> translated function

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

}  // namespace

bool aot_load(const char* so_path, uint64_t bias, uint64_t text_hash) {
    const std::filesystem::path dll = std::filesystem::u8path(std::string(so_path) + ".aot.dll");
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
    AcquireSRWLockExclusive(&g_lock);
    for (uint64_t i = 0; i < m->count; ++i) g_functions[bias + m->entries[i].vaddr] = m->entries[i].fn;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

AotFn aot_lookup(uint64_t pc) {
    AcquireSRWLockShared(&g_lock);
    auto it = g_functions.find(pc);
    const AotFn fn = it == g_functions.end() ? nullptr : it->second;
    ReleaseSRWLockShared(&g_lock);
    return fn;
}

uint32_t aot_run(AotFn fn, Cpu* c) { return fn(c); }

}  // namespace winapk::arm64
