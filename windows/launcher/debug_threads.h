// debug_threads.h -- the rows of the Threads window (debug mode): what Windows lists for an app's
// processes, put together with what each runtime's debug agent says of its own threads
// (../include/winapk/debug_protocol.h).
//
// The rule the window stands on: every thread Windows lists is a row, whether or not an agent answered
// and whatever it said. The agent only adds to a row (a Java thread's name and state, where the app's
// native code started a thread), and adds rows of its own only for threads Windows' list did not have:
// one that started since, or a Java thread whose Windows thread it does not know.
//
// No lock and no Windows call, as app_runs.h: test/launcher_state_test.cpp drives it.

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../include/winapk/debug_protocol.h"

namespace winapk::launcher {

enum class ThreadKind { kJava, kApp, kJvm, kRuntime, kSystem, kUnknown };

// A thread as Windows has it.
struct OsThread {
    uint32_t tid = 0;
    int priority = 0;
    bool timed = false;  // cpu is known (the thread could be opened)
    uint64_t cpu = 0;    // kernel + user time, in 100 ns
    std::string name;    // SetThreadDescription's
    std::string start;   // the module its start address is in, when it could be read
};

struct ThreadRow {
    uint32_t pid = 0, tid = 0;  // tid 0: a Java thread whose Windows thread is not known
    long long jid = -1;         // Thread.threadId() of a Java thread
    std::string process;
    std::string name;
    ThreadKind kind = ThreadKind::kUnknown;
    std::string state;  // a Java thread's
    bool daemon = false;
    bool timed = false;
    uint64_t cpu = 0;
    double cpu_pct = -1;  // of one core over the last interval; below 0: not known yet
    int priority = 0;
    std::string start;
    bool agent = false;  // the process's agent knows the thread
};

inline ThreadKind kind_of(const std::string& agent_kind) {
    if (agent_kind == "java") return ThreadKind::kJava;
    if (agent_kind == "guest") return ThreadKind::kApp;
    if (agent_kind == "jvm") return ThreadKind::kJvm;
    if (agent_kind == "runtime") return ThreadKind::kRuntime;
    if (agent_kind == "system") return ThreadKind::kSystem;
    return ThreadKind::kUnknown;
}

inline std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Without an agent, the module a thread started in is all there is to go by. jvm.dll starts the Java
// threads and HotSpot's own alike, and the runtime's exe its host threads and the app's native ones:
// those stay unknown rather than be called the wrong thing.
inline ThreadKind guess_kind(const std::string& start_module) {
    const std::string m = lower(start_module);
    if (m.empty() || m == "jvm.dll" || m == "winapk-runtime.exe") return ThreadKind::kUnknown;
    return ThreadKind::kSystem;
}

inline const char* kind_label(ThreadKind k) {
    switch (k) {
        case ThreadKind::kJava: return "Java";
        case ThreadKind::kApp: return "App native";
        case ThreadKind::kJvm: return "JVM";
        case ThreadKind::kRuntime: return "Win.apk";
        case ThreadKind::kSystem: return "System";
        case ThreadKind::kUnknown: break;
    }
    return "";
}

// One process's rows. `agent`: its agent's answer, or null when it has none or did not answer.
inline std::vector<ThreadRow> merge_threads(uint32_t pid, const std::string& process, const std::vector<OsThread>& os,
                                            const winapk::debug::AgentReply* agent) {
    std::map<uint32_t, const winapk::debug::AgentThread*> known;
    if (agent && agent->ok)
        for (const auto& t : agent->threads)
            if (t.tid) known[t.tid] = &t;
    std::vector<ThreadRow> rows;
    std::map<uint32_t, bool> listed;
    for (const OsThread& o : os) {
        ThreadRow r;
        r.pid = pid;
        r.tid = o.tid;
        r.process = process;
        r.name = o.name;
        r.timed = o.timed;
        r.cpu = o.cpu;
        r.priority = o.priority;
        r.start = o.start;
        r.kind = guess_kind(o.start);
        const auto k = known.find(o.tid);
        if (k != known.end()) {
            const winapk::debug::AgentThread& a = *k->second;
            r.agent = true;
            r.kind = kind_of(a.kind);
            r.jid = a.jid;
            r.state = a.state;
            r.daemon = a.daemon;
            if (!a.name.empty()) r.name = a.name;
            if (!a.start.empty()) r.start = a.start;
        }
        listed[o.tid] = true;
        rows.push_back(std::move(r));
    }
    if (agent && agent->ok) {
        for (const auto& a : agent->threads) {
            if (a.tid && listed.count(a.tid)) continue;
            ThreadRow r;
            r.pid = pid;
            r.tid = a.tid;
            r.process = process;
            r.agent = true;
            r.kind = kind_of(a.kind);
            r.jid = a.jid;
            r.state = a.state;
            r.daemon = a.daemon;
            r.name = a.name;
            r.start = a.start;
            rows.push_back(std::move(r));
        }
    }
    return rows;
}

// Each thread's share of one core between two looks at its CPU time.
class CpuTracker {
public:
    // `now` in 100 ns, on any clock that only goes forward.
    void apply(std::vector<ThreadRow>& rows, uint64_t now) {
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> next;
        for (ThreadRow& r : rows) {
            if (!r.timed || !r.tid) continue;
            const auto key = std::make_pair(r.pid, r.tid);
            const auto p = prev_.find(key);
            if (p != prev_.end() && now > time_ && r.cpu >= p->second)
                r.cpu_pct = 100.0 * static_cast<double>(r.cpu - p->second) / static_cast<double>(now - time_);
            next[key] = r.cpu;
        }
        prev_ = std::move(next);
        time_ = now;
    }
    void reset() {
        prev_.clear();
        time_ = 0;
    }

private:
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> prev_;
    uint64_t time_ = 0;
};

// Whether the row has every word of `query` (lower case, split at spaces) in its name, process, kind,
// state, start or thread id.
inline bool row_matches(const ThreadRow& r, const std::string& query) {
    const std::string hay = lower(r.name + "\n" + r.process + "\n" + kind_label(r.kind) + "\n" + r.state + "\n" + r.start +
                                  "\n" + std::to_string(r.tid));
    size_t at = 0;
    while (at < query.size()) {
        size_t end = query.find(' ', at);
        if (end == std::string::npos) end = query.size();
        if (end > at && hay.find(query.substr(at, end - at)) == std::string::npos) return false;
        at = end + 1;
    }
    return true;
}

enum class ThreadColumn { kProcess, kTid, kName, kKind, kState, kCpuPct, kCpuTime, kPriority, kStart, kCount };

inline void sort_rows(std::vector<ThreadRow>& rows, ThreadColumn by, bool descending) {
    auto cmp = [by](const ThreadRow& a, const ThreadRow& b) -> int {
        auto three = [](auto x, auto y) { return x < y ? -1 : y < x ? 1 : 0; };
        switch (by) {
            case ThreadColumn::kProcess: {
                const int c = three(lower(a.process), lower(b.process));
                return c ? c : three(a.pid, b.pid);
            }
            case ThreadColumn::kTid: return three(a.tid, b.tid);
            case ThreadColumn::kName: return three(lower(a.name), lower(b.name));
            case ThreadColumn::kKind: return three(static_cast<int>(a.kind), static_cast<int>(b.kind));
            case ThreadColumn::kState: return three(a.state, b.state);
            case ThreadColumn::kCpuPct: return three(a.cpu_pct, b.cpu_pct);
            case ThreadColumn::kCpuTime: return three(a.cpu, b.cpu);
            case ThreadColumn::kPriority: return three(a.priority, b.priority);
            case ThreadColumn::kStart: return three(lower(a.start), lower(b.start));
            case ThreadColumn::kCount: break;
        }
        return 0;
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const ThreadRow& a, const ThreadRow& b) {
        const int c = cmp(a, b);
        if (c) return descending ? c > 0 : c < 0;
        // The same order every second for equal rows: a list that is read must not shuffle.
        if (a.pid != b.pid) return a.pid < b.pid;
        if (a.tid != b.tid) return a.tid < b.tid;
        return a.jid < b.jid;
    });
}

struct ThreadCounts {
    int total = 0, java = 0, app = 0, jvm = 0, runtime = 0, system = 0, unknown = 0;
};

inline ThreadCounts count_threads(const std::vector<ThreadRow>& rows) {
    ThreadCounts c;
    for (const ThreadRow& r : rows) {
        ++c.total;
        switch (r.kind) {
            case ThreadKind::kJava: ++c.java; break;
            case ThreadKind::kApp: ++c.app; break;
            case ThreadKind::kJvm: ++c.jvm; break;
            case ThreadKind::kRuntime: ++c.runtime; break;
            case ThreadKind::kSystem: ++c.system; break;
            case ThreadKind::kUnknown: ++c.unknown; break;
        }
    }
    return c;
}

}  // namespace winapk::launcher
