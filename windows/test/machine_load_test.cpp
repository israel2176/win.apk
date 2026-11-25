// machine_load_test.cpp -- winapk-machine-load-test: the machine's load to the first frame
// (host/machine_load.h), the arithmetic and the line. No window and no Windows call.
//
//   winapk-machine-load-test

#include <cstdio>
#include <string>

#include "machine_load.h"

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

using namespace winapk::host::machine_load;

constexpr uint64_t kSecond = 10'000'000;

Proc proc(uint32_t pid, uint64_t created, double cpu_s, const char* name) {
    Proc p;
    p.pid = pid;
    p.created = created;
    p.cpu = static_cast<uint64_t>(cpu_s * kSecond);
    p.name = name;
    return p;
}

}  // namespace

int main() {
    // 4 CPUs over 10 s: 40 s of processor time, 4 s of it idle.
    Sample a, b;
    a.idle = 100 * kSecond;
    a.total = 400 * kSecond;
    b.idle = a.idle + 4 * kSecond;
    b.total = a.total + 40 * kSecond;
    a.procs = {proc(0, 0, 900, ""), proc(10, 1, 50, "MsMpEng.exe"), proc(20, 2, 1, "svchost.exe"),
               proc(21, 3, 1, "svchost.exe"), proc(30, 4, 2, "winapk-runtime.exe"), proc(40, 5, 3, "old.exe")};
    b.procs = {proc(0, 0, 935, ""),
               proc(10, 1, 58, "MsMpEng.exe"),        // 8 s
               proc(20, 2, 1.5, "svchost.exe"),       // 0.5 s
               proc(21, 3, 1.25, "svchost.exe"),      // 0.25 s
               proc(30, 4, 11, "winapk-runtime.exe"),  // self: 9 s
               proc(40, 9, 2, "new.exe"),             // pid 40 reused: a new process, 2 s whole
               proc(50, 10, 0.05, "tiny.exe"),        // under 0.1 s
               proc(60, 11, 3, "TiWorker.exe")};      // started since: 3 s whole

    std::printf("the busiest others\n");
    {
        const auto o = busiest_others(a, b, 30, 3);
        check("three, busiest first", o.size() == 3 && o[0].name == "MsMpEng.exe" && o[1].name == "TiWorker.exe" &&
                                          o[2].name == "new.exe");
        check("MsMpEng 8 s", o.size() == 3 && o[0].cpu == 8 * kSecond && o[0].count == 1);
        check("a reused pid counts whole", o.size() == 3 && o[2].cpu == 2 * kSecond);
        const auto all = busiest_others(a, b, 30, 10);
        bool self = false, idle = false, tiny = false;
        const Other* svc = nullptr;
        for (const Other& x : all) {
            self |= x.name == "winapk-runtime.exe";
            idle |= x.name.empty();
            tiny |= x.name == "tiny.exe";
            if (x.name == "svchost.exe") svc = &x;
        }
        check("not this process, not idle, not under 0.1 s", !self && !idle && !tiny);
        check("one name summed: svchost (2) 0.75 s", svc && svc->count == 2 && svc->cpu == kSecond * 3 / 4);
    }

    std::printf("the line\n");
    {
        const std::string s = describe(a, b, 30, 10 * kSecond, 4, "to the first frame");
        std::printf("    %s\n", s.c_str());
        check("busy share", s.find("machine 90% busy to the first frame (4 logical CPUs)") == 0);
        check("own share of all CPUs", s.find("this process 25%") != std::string::npos);
        check("others", s.find("; busiest others: MsMpEng.exe 8.0 s, TiWorker.exe 3.0 s, new.exe 2.0 s") !=
                            std::string::npos);
        b.procs.clear();
        const std::string bare = describe(a, b, 30, 9 * kSecond, 4, "to the first frame");
        check("no process list: the shares alone", bare.find("busiest") == std::string::npos && !bare.empty());
        Sample same = a;
        check("no time passed: no line", describe(a, same, 30, 0, 4, "x").empty());
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
