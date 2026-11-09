// aot_stamp_test.cpp -- winapk-aot-stamp-test: a library's translation stamp (include/winapk/aot_stamp.h),
// which reads no file and makes no Windows call.
//
//   winapk-aot-stamp-test
//
// The line: each kind is written and read back, an old stamp is a short one, a word from a newer build is
// kOther. The loader: what it refuses, and when a DLL must carry a seal. The worker: for each kind,
// whether there is a step to take now (a changed library or translator, another CPU, the back-off after
// failures and its limit, a pass to finish, a profile that grew, the interval between two translations
// from a profile). The launcher: which stamps mean "translated by its profile".

#include <cstdio>
#include <string>

#include "winapk/aot_stamp.h"

namespace {

using namespace winapk::arm64;

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

constexpr unsigned kVersion = 7, kMaxFailures = 4;
constexpr uint64_t kCpu = 0xc0ffee, kLib = 0xabc123;
constexpr int64_t kNow = 1000000;

AotStamp stamp(AotStampKind kind, size_t lines = 0, unsigned failures = 0, int64_t retry_after = 0,
               unsigned version = kVersion, uint64_t cpu = kCpu) {
    return aot_parse_stamp(aot_stamp_line(kind, lines, version, failures, retry_after, cpu, kLib));
}

// What the worker knows: a current stamp, this translator and CPU, a profile of `profile` lines.
AotStampFacts facts(bool have, size_t profile = 0, bool current = true) {
    AotStampFacts f;
    f.have = have;
    f.current = current;
    f.version = kVersion;
    f.cpu = kCpu;
    f.now = kNow;
    f.max_failures = kMaxFailures;
    f.profile_lines = [profile] { return profile; };
    return f;
}

void test_line() {
    std::printf("the line\n");
    const std::string line = aot_stamp_line(AotStampKind::kWait, 1234, 7, 2, 1700000000, 0xdeadbeefULL, 0x1122334455667788ULL);
    check("the line is 'kind lines version failures retry cpu lib'", line == "wait 1234 7 2 1700000000 deadbeef 1122334455667788\n");
    const AotStamp s = aot_parse_stamp(line);
    check("and is read back whole",
          s.kind == AotStampKind::kWait && s.lines == 1234 && s.version == 7 && s.failures == 2 &&
              s.retry_after == 1700000000 && s.cpu == 0xdeadbeefULL && s.lib == 0x1122334455667788ULL && s.fields == 7);
    bool all = true;
    for (const AotStampKind k : {AotStampKind::kFull, AotStampKind::kProfile, AotStampKind::kWait, AotStampKind::kSkip,
                                 AotStampKind::kBad, AotStampKind::kWritten}) {
        all = all && aot_parse_stamp(aot_stamp_line(k, 0, 7, 0, 0, 1, 2)).kind == k;
    }
    check("every kind is read back as itself", all);
    check("no stamp is kNone", aot_parse_stamp("").kind == AotStampKind::kNone && aot_parse_stamp("").fields == 0);
    check("blank text is kNone", aot_parse_stamp(" \n").kind == AotStampKind::kNone);
    const AotStamp old = aot_parse_stamp("profile 300\n");
    check("a stamp from before versions: the fields it has, the rest 0",
          old.kind == AotStampKind::kProfile && old.lines == 300 && old.version == 0 && old.cpu == 0 && old.lib == 0 &&
              old.fields == 2);
    const AotStamp cut = aot_parse_stamp("full 99 end\n");
    check("reading stops at what is not a number", cut.kind == AotStampKind::kFull && cut.lines == 99 && cut.fields == 2);
    const AotStamp newer = aot_parse_stamp("sideways 5 9 0 0 1 2\n");
    check("a word this build does not know is kOther, its numbers read",
          newer.kind == AotStampKind::kOther && newer.lines == 5 && newer.version == 9 && newer.fields == 7);
    check("a kind is its whole word", aot_parse_stamp("fuller 1\n").kind == AotStampKind::kOther);
}

void test_loader() {
    std::printf("the loader\n");
    bool sealed = true;
    check("no stamp: loaded, and a DLL from before seals is accepted",
          aot_stamp_refuses(aot_parse_stamp(""), kCpu, &sealed) == nullptr && !sealed);
    check("a whole translation: loaded, its DLL must be sealed",
          aot_stamp_refuses(stamp(AotStampKind::kFull), kCpu, &sealed) == nullptr && sealed);
    check("from a profile: loaded", aot_stamp_refuses(stamp(AotStampKind::kProfile, 500), kCpu, &sealed) == nullptr && sealed);
    check("a crashed translation: refused", aot_stamp_refuses(stamp(AotStampKind::kBad), kCpu, &sealed) != nullptr && !sealed);
    check("a library that rewrites its code: refused",
          aot_stamp_refuses(stamp(AotStampKind::kWritten), kCpu, &sealed) != nullptr && !sealed);
    check("'bad' is refused however short the stamp", aot_stamp_refuses(aot_parse_stamp("bad\n"), kCpu, &sealed) != nullptr);
    check("translated for another CPU: refused",
          aot_stamp_refuses(stamp(AotStampKind::kFull, 0, 0, 0, kVersion, kCpu + 1), kCpu, &sealed) != nullptr && !sealed);
    check("a stamp from before it named a CPU: loaded, no seal asked",
          aot_stamp_refuses(aot_parse_stamp("full 0 7 0 0 0\n"), kCpu, &sealed) == nullptr && !sealed);
    check("a stamp too short to name a CPU: loaded, no seal asked",
          aot_stamp_refuses(aot_parse_stamp("full 0 7\n"), kCpu, &sealed) == nullptr && !sealed);
    check("a stamp that names no library: loaded, no seal asked",
          aot_stamp_refuses(aot_parse_stamp("full 0 7 0 0 c0ffee 0\n"), kCpu, &sealed) == nullptr && !sealed);
}

void test_worker() {
    std::printf("the worker\n");
    const AotStamp none = aot_parse_stamp("");
    check("no stamp, no translation: translate", aot_stamp_needs_work(none, facts(false)));
    check("a whole translation in place: nothing to do", !aot_stamp_needs_work(stamp(AotStampKind::kFull), facts(true, 100000)));
    check("its DLL is gone or cut short: translate again", aot_stamp_needs_work(stamp(AotStampKind::kFull), facts(false)));
    check("made by another translator: translate again",
          aot_stamp_needs_work(stamp(AotStampKind::kFull, 0, 0, 0, kVersion - 1), facts(true)));
    check("made for another CPU: translate again",
          aot_stamp_needs_work(stamp(AotStampKind::kFull, 0, 0, 0, kVersion, kCpu + 1), facts(true)));

    check("not an arm64 library: left alone", !aot_stamp_needs_work(stamp(AotStampKind::kSkip), facts(false)));
    check("until the library changes", aot_stamp_needs_work(stamp(AotStampKind::kSkip), facts(false, 0, false)));
    check("rewrites its own code: left alone, even from an older translator",
          !aot_stamp_needs_work(stamp(AotStampKind::kWritten, 0, 0, 0, kVersion - 1), facts(false)));
    check("a crashed translation: left alone", !aot_stamp_needs_work(stamp(AotStampKind::kBad), facts(false)));
    check("until the translator changes", aot_stamp_needs_work(stamp(AotStampKind::kBad, 0, 0, 0, kVersion - 1), facts(false)));
    check("or the library does", aot_stamp_needs_work(stamp(AotStampKind::kBad), facts(false, 0, false)));

    check("too large, no profile yet: wait", !aot_stamp_needs_work(stamp(AotStampKind::kWait, 0), facts(false, 199)));
    check("its profile reached 200 lines: translate", aot_stamp_needs_work(stamp(AotStampKind::kWait, 0), facts(false, 200)));
    check("a profile of 1000 that grew by less than a quarter: wait",
          !aot_stamp_needs_work(stamp(AotStampKind::kWait, 1000), facts(false, 1249)));
    check("by a quarter: translate", aot_stamp_needs_work(stamp(AotStampKind::kWait, 1000), facts(false, 1250)));

    check("failed, the next try not due: wait",
          !aot_stamp_needs_work(stamp(AotStampKind::kWait, 0, 1, kNow + 1), facts(false, 100000)));
    check("due: try again", aot_stamp_needs_work(stamp(AotStampKind::kWait, 0, 1, kNow), facts(false)));
    check("failed as often as allowed: no more tries",
          !aot_stamp_needs_work(stamp(AotStampKind::kWait, 0, kMaxFailures, 0), facts(false, 100000)));
    check("until the translator changes",
          aot_stamp_needs_work(stamp(AotStampKind::kWait, 0, kMaxFailures, kNow + 999, kVersion - 1), facts(false)));
    check("or the library does",
          aot_stamp_needs_work(stamp(AotStampKind::kWait, 0, kMaxFailures, kNow + 999), facts(false, 0, false)));
    AotStampFacts pass = facts(false, 0);
    pass.pass_to_finish = true;
    check("a pass to finish does not jump the back-off",
          !aot_stamp_needs_work(stamp(AotStampKind::kWait, 0, 1, kNow + 1), pass));
    check("an interrupted pass, nothing failed: go on", aot_stamp_needs_work(stamp(AotStampKind::kWait, 0), pass));
    AotStampFacts pass_have = facts(true, 0);
    pass_have.pass_to_finish = true;
    check("an interrupted pass over a whole translation: go on", aot_stamp_needs_work(stamp(AotStampKind::kFull), pass_have));

    check("from a profile, the profile grown, within the interval: wait",
          !aot_stamp_needs_work(stamp(AotStampKind::kProfile, 1000, 0, kNow + 1), facts(true, 5000)));
    check("after the interval: translate again",
          aot_stamp_needs_work(stamp(AotStampKind::kProfile, 1000, 0, kNow), facts(true, 5000)));
    check("after the interval, the profile not grown enough: nothing to do",
          !aot_stamp_needs_work(stamp(AotStampKind::kProfile, 1000, 0, kNow), facts(true, 1249)));
    check("from a profile, its DLL gone: translate at once",
          aot_stamp_needs_work(stamp(AotStampKind::kProfile, 1000, 0, kNow + 999), facts(false, 0)));
    check("a DLL without a stamp is a whole translation from before stamps: nothing to do",
          !aot_stamp_needs_work(none, [] {
              AotStampFacts f = facts(true, 100000);
              f.version = 0;  // a stamp's version is 0 before stamps had one
              f.cpu = 0;
              return f;
          }()));
    AotStampFacts no_profile = facts(false);
    no_profile.profile_lines = nullptr;
    check("no way to ask for the profile counts as none", !aot_stamp_needs_work(stamp(AotStampKind::kWait, 0), no_profile));

    check("a first failure makes one", aot_stamp_failures_after(stamp(AotStampKind::kFull)) == 1 && aot_stamp_failures_after(none) == 1);
    check("a failure after two makes three", aot_stamp_failures_after(stamp(AotStampKind::kWait, 0, 2)) == 3);
    check("waiting for a profile was no failure", aot_stamp_failures_after(stamp(AotStampKind::kWait, 500)) == 1);
}

void test_launcher() {
    std::printf("the launcher\n");
    check("translated from its profile: by profile", aot_stamp_by_profile(stamp(AotStampKind::kProfile, 300)));
    check("waiting for its profile: by profile", aot_stamp_by_profile(stamp(AotStampKind::kWait, 0)));
    check("waiting after a failure: not", !aot_stamp_by_profile(stamp(AotStampKind::kWait, 0, 1, kNow)));
    check("whole, skipped, bad, none: not",
          !aot_stamp_by_profile(stamp(AotStampKind::kFull)) && !aot_stamp_by_profile(stamp(AotStampKind::kSkip)) &&
              !aot_stamp_by_profile(stamp(AotStampKind::kBad)) && !aot_stamp_by_profile(aot_parse_stamp("")));
}

}  // namespace

int main() {
    test_line();
    test_loader();
    test_worker();
    test_launcher();
    std::printf("%d failed\n", g_failures);
    return g_failures ? 1 : 0;
}
