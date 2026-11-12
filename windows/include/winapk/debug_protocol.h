// debug_protocol.h -- what the runtime's debug agent (host/debug_agent.cpp) and the launcher's Threads
// window (launcher/launcher_main.cpp, debug_threads.h) say to each other in debug mode.
//
// The agent listens on a named pipe per process, \\.\pipe\winapk-debug-<pid>. One question a
// connection, a line; the answer is text and ends when the agent closes the pipe:
//
//   threads\n            -> "winapk-debug 1\n", then a line per thread:
//                           T \t tid \t kind \t jid \t daemon \t state \t start \t name
//                           Every thread Windows lists for the process is there, and a Java thread
//                           whose Windows thread is not known as tid 0.
//   stack <tid> <jid>\n  -> the thread's Java frames and what its native stack shows, for a reader.
//
// kind: java (a java.lang.Thread; jid is its Thread.threadId(), else -1), guest (the app's native code
// made it with pthread_create), jvm (HotSpot's own: GC, the compilers), runtime (Win.apk's host) or
// system (Windows', a driver's, ANGLE's: whatever else started it). start is where the thread began:
// "libapp.so!worker+0x10" for a guest function, "module.dll+0x1234" otherwise. The name is last, so a
// tab in it cannot shift a field. No Windows call here: test/launcher_state_test.cpp reads it too.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace winapk::debug {

constexpr int kVersion = 1;
constexpr const char* kHello = "winapk-debug";

inline std::wstring pipe_name(uint32_t pid) { return L"\\\\.\\pipe\\winapk-debug-" + std::to_wstring(pid); }

struct AgentThread {
    uint32_t tid = 0;
    std::string kind;
    long long jid = -1;
    bool daemon = false;
    std::string state;
    std::string start;
    std::string name;
};

// A field on one line: no tab, no line break.
inline std::string one_line(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

inline std::string format_header() { return std::string(kHello) + " " + std::to_string(kVersion) + "\n"; }

inline std::string format_thread(const AgentThread& t) {
    return "T\t" + std::to_string(t.tid) + "\t" + one_line(t.kind) + "\t" + std::to_string(t.jid) + "\t" +
           (t.daemon ? "1" : "0") + "\t" + one_line(t.state) + "\t" + one_line(t.start) + "\t" + one_line(t.name) +
           "\n";
}

struct AgentReply {
    bool ok = false;  // the header was there and the version is one this reader knows
    int version = 0;
    std::vector<AgentThread> threads;
};

// Reads an answer to "threads". A line it cannot read is left out; a reply cut short keeps what came whole.
inline AgentReply parse_threads(const std::string& text) {
    AgentReply r;
    size_t at = 0;
    bool first = true;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) break;  // a line without its end did not arrive whole
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (first) {
            first = false;
            const std::string hello = std::string(kHello) + " ";
            if (line.compare(0, hello.size(), hello) != 0) return r;
            r.version = std::atoi(line.c_str() + hello.size());
            r.ok = r.version == kVersion;
            if (!r.ok) return r;
            continue;
        }
        if (line.compare(0, 2, "T\t") != 0) continue;
        std::vector<std::string> f;
        size_t p = 2;
        while (f.size() < 6) {
            const size_t tab = line.find('\t', p);
            if (tab == std::string::npos) break;
            f.push_back(line.substr(p, tab - p));
            p = tab + 1;
        }
        if (f.size() < 6) continue;
        AgentThread t;
        t.tid = static_cast<uint32_t>(std::strtoul(f[0].c_str(), nullptr, 10));
        t.kind = f[1];
        t.jid = std::atoll(f[2].c_str());
        t.daemon = f[3] == "1";
        t.state = f[4];
        t.start = f[5];
        t.name = line.substr(p);
        r.threads.push_back(std::move(t));
    }
    return r;
}

}  // namespace winapk::debug
