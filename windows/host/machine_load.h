// machine_load.h -- how busy the whole machine was while an app started: one line at its first
// frame, beside the startup time it explains.
//
// A start that is slow because of the app or the runtime looks the same in the log as one that
// is slow because something else had the CPU (an antivirus scan of files just written, Windows'
// servicing, another program's own start): every step simply takes longer. The cold start of
// Jellyfin in comparison run 45513190823 took 13.4 s against 1.5-3.3 s for every other app,
// and steps no app's code touches were 15-18x slower there (the system fonts 4.6 s against
// 0.1-0.4 s, the starting window 2.1 s against ~0.1 s); without a record of the machine's load
// the log could not say why. So the runtime samples the system's CPU times and every process's
// at its start (`mark`) and again at its first frame (`log_first_frame`, native_cocoa.cpp), and
// logs the machine's busy share, its own, and the busiest other processes:
//
//   [win.apk] startup: machine 91% busy to the first frame (4 logical CPUs), this process 23%;
//             busiest others: MsMpEng.exe 7.9 s, TiWorker.exe 2.1 s, svchost.exe (6) 0.6 s
//
// Two samples, one system call each (NtQuerySystemInformation's process list, no process
// opened): nothing runs in between. A process that both started and ended between the samples
// is not seen. The arithmetic below has no Windows call, for test/machine_load_test.cpp.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace winapk::host::machine_load {

// One process in a sample: CPU times are 100 ns units, user + kernel.
struct Proc {
    uint32_t pid = 0;
    uint64_t created = 0;  // its creation time: a reused pid is another process
    uint64_t cpu = 0;
    std::string name;  // the image name, UTF-8
};

// The whole machine at one moment: GetSystemTimes' idle and kernel + user (kernel includes
// idle), summed over the logical processors, and every process.
struct Sample {
    uint64_t idle = 0;
    uint64_t total = 0;
    std::vector<Proc> procs;
};

struct Other {
    std::string name;
    int count = 0;     // processes of that name that used CPU between the samples
    uint64_t cpu = 0;  // their CPU time between the samples, 100 ns
};

// Other processes' CPU time between `a` and `b`, by image name, busiest first. A process not in
// `a` (started since) counts whole; `self` and the idle process (pid 0) are left out. Names whose
// time is below `floor` are dropped; at most `n` are returned.
inline std::vector<Other> busiest_others(const Sample& a, const Sample& b, uint32_t self, size_t n,
                                         uint64_t floor = 1'000'000 /* 0.1 s */) {
    std::map<std::pair<uint32_t, uint64_t>, uint64_t> before;
    for (const Proc& p : a.procs) before[{p.pid, p.created}] = p.cpu;
    std::map<std::string, Other> by_name;
    for (const Proc& p : b.procs) {
        if (p.pid == 0 || p.pid == self) continue;
        const auto it = before.find({p.pid, p.created});
        const uint64_t was = it == before.end() ? 0 : it->second;
        if (p.cpu <= was) continue;
        Other& o = by_name[p.name];
        o.name = p.name;
        ++o.count;
        o.cpu += p.cpu - was;
    }
    std::vector<Other> out;
    for (auto& [name, o] : by_name) {
        if (o.cpu >= floor) out.push_back(o);
    }
    std::sort(out.begin(), out.end(), [](const Other& x, const Other& y) {
        return x.cpu != y.cpu ? x.cpu > y.cpu : x.name < y.name;
    });
    if (out.size() > n) out.resize(n);
    return out;
}

// The line's text after "[win.apk] startup: ", or "" when the samples say nothing (no time
// passed, or a clock went back). `self_cpu` is this process's CPU time between the samples.
inline std::string describe(const Sample& a, const Sample& b, uint32_t self, uint64_t self_cpu, int cpus,
                            const char* until) {
    if (b.total <= a.total || b.idle < a.idle) return std::string();
    const double total = static_cast<double>(b.total - a.total);
    const double idle = std::min(total, static_cast<double>(b.idle - a.idle));
    char buf[160];
    std::snprintf(buf, sizeof buf, "machine %.0f%% busy %s (%d logical CPUs), this process %.0f%%",
                  100.0 * (total - idle) / total, until, cpus, std::min(100.0, 100.0 * self_cpu / total));
    std::string s = buf;
    const std::vector<Other> others = busiest_others(a, b, self, 3);
    if (!others.empty()) {
        s += "; busiest others: ";
        for (size_t i = 0; i < others.size(); ++i) {
            if (i) s += ", ";
            s += others[i].name.empty() ? "?" : others[i].name;
            if (others[i].count > 1) s += " (" + std::to_string(others[i].count) + ")";
            std::snprintf(buf, sizeof buf, " %.1f s", others[i].cpu / 1e7);
            s += buf;
        }
    }
    return s;
}

// ---- Windows (machine_load.cpp) ----------------------------------------------

// The start's sample: early in wmain, before the JVM.
void mark();

// At the first frame: logs the line, once, when mark() ran.
void log_first_frame();

}  // namespace winapk::host::machine_load
