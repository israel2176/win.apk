// stub_hits.h -- how often this run called what Windows does not implement yet.
//
// A missing native method (host_natives.cpp) and a missing system function a
// guest library imports (guest_libs.cpp) are both bound to a stub that returns
// zero. Each stub's first call is logged where it happens; on top of that every
// call is counted, and when the process exits the log ends with the stubs this
// run reached, the busiest first. A crash far from the stub that caused it, or
// a screen that stays empty, then comes with the list of what the app asked for
// and did not get, in the log the user attaches to a report.
//
// Header only, so that both binders share one list without a new source file.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace winapk::host {

struct StubHit {
    std::string name;         // "com.macapk.loader.NativeCocoa.init()Z", "AMediaCodec_createDecoderByType"
    const char* kind;         // "native method", "system function"
    std::atomic<uint64_t> calls{0};
};

namespace stub_hits_detail {

inline std::mutex g_lock;
inline std::vector<StubHit*> g_all;  // never freed: a stub may run until the process is gone

constexpr size_t kMaxListed = 40;

inline void print_summary() {
    std::vector<std::pair<uint64_t, const StubHit*>> hit;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        for (const StubHit* h : g_all) {
            const uint64_t n = h->calls.load(std::memory_order_relaxed);
            if (n) hit.emplace_back(n, h);
        }
    }
    if (hit.empty()) return;
    std::sort(hit.begin(), hit.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::fprintf(stderr, "[win.apk] not implemented on Windows yet, called this run (%zu):\n", hit.size());
    for (size_t i = 0; i < hit.size() && i < kMaxListed; ++i) {
        std::fprintf(stderr, "[win.apk]   %llux %s %s\n", static_cast<unsigned long long>(hit[i].first),
                     hit[i].second->kind, hit[i].second->name.c_str());
    }
    if (hit.size() > kMaxListed) std::fprintf(stderr, "[win.apk]   ... and %zu more\n", hit.size() - kMaxListed);
    std::fflush(stderr);
}

}  // namespace stub_hits_detail

// A new stub's record, listed for the summary at exit. Thread safe.
inline StubHit* stub_hit_register(std::string name, const char* kind) {
    auto* h = new StubHit{std::move(name), kind};
    std::lock_guard<std::mutex> hold(stub_hits_detail::g_lock);
    // HotSpot ends the process through exit(), which runs this; a process
    // killed outright has no summary, only the first-call lines.
    if (stub_hits_detail::g_all.empty()) std::atexit(stub_hits_detail::print_summary);
    stub_hits_detail::g_all.push_back(h);
    return h;
}

// Counts one call. True on the first, when the caller logs it.
inline bool stub_hit(StubHit* h) {
    return h->calls.fetch_add(1, std::memory_order_relaxed) == 0;
}

}  // namespace winapk::host
