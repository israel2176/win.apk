// aot_stamp.h -- <lib>.so.aot.stamp: where one library's arm64 translation stands.
//
// One line, "kind lines version failures retry cpu lib" (the last two in hex), read and written by three
// programs: the app's loader (arm64_aot.cpp), its background worker (runtime_main.cpp, --aot-translate)
// and the launcher (the details page). This header is the line's one reader and writer and the rules
// that go by it, with no file and no Windows call in it, so that test/aot_stamp_test.cpp checks them.
// The file itself is written whole or not at all (aot_files.h).
//
//   kind       what it means                                              who writes it
//   kNone      no stamp. With a DLL in place: a whole translation from     -
//              before stamps
//   kFull      translated whole (or taken from the shared cache)           worker
//   kProfile   translated where the app's code ran, by its profile of      worker
//              `lines` lines; not again before `retry_after`
//   kWait      failures == 0: too large to translate whole, waiting for    worker
//              a profile to grow past `lines`. failures > 0: the last
//              `failures` tries failed; the next is at `retry_after`,
//              and after the worker's limit there is none
//   kSkip      not an arm64 ELF file (packed, encrypted): never translated worker
//   kBad       its translation crashed: the JIT runs the library           loader
//   kWritten   the library rewrites its own code: the JIT runs it          loader
//   kOther     a word this build does not know (a newer build's)           -
//
// A stamp is about one build of the library (`lib`, aot_lib_id), one translator (`version`,
// kAotVersion) and one CPU (`cpu`, aot_cpu_signature). When the library changes, every kind starts
// over; when the translator changes, a translation in place, kBad and a failed kWait do.
//
//   from                      to         by       when
//   kNone, any, not current   kSkip      worker   the library is not an arm64 ELF file
//   kNone, kWait, kProfile    kFull      worker   translated whole, or its translation was in the cache
//   kNone                     kWait(0)   worker   too large, and no profile yet
//   kWait(0), kProfile        kProfile   worker   translated from a profile that grew enough
//   any the worker works on   kWait(n+1) worker   winapk-aot or clang failed (n from a kWait before)
//   any                       kBad       loader   a fault in the translation
//   any                       kWritten   loader   the library wrote to its own code
//
// aot_stamp_needs_work is the worker's side of that table (is there a step to take now?),
// aot_stamp_refuses the loader's (may the translation in place be loaded?).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

namespace winapk::arm64 {

enum class AotStampKind { kNone, kFull, kProfile, kWait, kSkip, kBad, kWritten, kOther };

struct AotStamp {
    AotStampKind kind = AotStampKind::kNone;
    size_t lines = 0;         // the profile's line count when it was written
    unsigned version = 0;     // the kAotVersion it was made with; 0 before stamps had one
    unsigned failures = 0;    // kWait after a failed translation: how many in a row
    int64_t retry_after = 0;  // kWait after a failure, kProfile: no new try before this (Unix seconds)
    uint64_t cpu = 0;         // aot_cpu_signature() of the CPU it was compiled for; 0 before stamps had one
    uint64_t lib = 0;         // aot_lib_id() of the library it is about; 0 before stamps had one (and DLLs a seal)
    int fields = 0;           // how many of the line's seven fields were read, in order
};

// The word a kind is written as; kNone and kOther have none.
inline const char* aot_stamp_word(AotStampKind kind) {
    switch (kind) {
        case AotStampKind::kFull: return "full";
        case AotStampKind::kProfile: return "profile";
        case AotStampKind::kWait: return "wait";
        case AotStampKind::kSkip: return "skip";
        case AotStampKind::kBad: return "bad";
        case AotStampKind::kWritten: return "written";
        case AotStampKind::kNone:
        case AotStampKind::kOther: break;
    }
    return "";
}

// Reads a stamp's text. Fields are read in order up to the first that is missing or not a number; the
// rest stay 0 (an old stamp is a short one). No text, or no word: kNone.
inline AotStamp aot_parse_stamp(const std::string& text) {
    AotStamp s;
    char word[16] = {};
    unsigned long long lines = 0, cpu = 0, lib = 0;
    unsigned version = 0, failures = 0;
    long long retry_after = 0;
    const int n = std::sscanf(text.c_str(), "%15s %llu %u %u %lld %llx %llx", word, &lines, &version, &failures,
                              &retry_after, &cpu, &lib);
    if (n < 1) return s;
    s.fields = n;
    s.kind = AotStampKind::kOther;
    for (const AotStampKind k : {AotStampKind::kFull, AotStampKind::kProfile, AotStampKind::kWait, AotStampKind::kSkip,
                                 AotStampKind::kBad, AotStampKind::kWritten}) {
        if (std::strcmp(word, aot_stamp_word(k)) == 0) s.kind = k;
    }
    s.lines = static_cast<size_t>(lines);
    s.version = version;
    s.failures = failures;
    s.retry_after = retry_after;
    s.cpu = cpu;
    s.lib = lib;
    return s;
}

// A stamp's line, as every writer writes it.
inline std::string aot_stamp_line(AotStampKind kind, size_t lines, unsigned version, unsigned failures,
                                  int64_t retry_after, uint64_t cpu, uint64_t lib) {
    char line[160];
    std::snprintf(line, sizeof line, "%s %zu %u %u %lld %llx %llx\n", aot_stamp_word(kind), lines, version, failures,
                  static_cast<long long>(retry_after), static_cast<unsigned long long>(cpu),
                  static_cast<unsigned long long>(lib));
    return line;
}

// The loader: why the library's translation is not to be loaded, or nullptr. `this_cpu` is
// aot_cpu_signature(). *sealed: the stamp names the library (aot_files.h), so its DLLs carry a seal and
// one without is cut short.
inline const char* aot_stamp_refuses(const AotStamp& s, uint64_t this_cpu, bool* sealed) {
    *sealed = false;
    if (s.kind == AotStampKind::kBad) return "its translation crashed before";
    if (s.kind == AotStampKind::kWritten) return "it rewrites its own code";
    if (s.fields < 6) return nullptr;  // a stamp from before it named a CPU
    if (s.cpu != 0 && s.cpu != this_cpu) return "it was translated for another CPU";
    *sealed = s.fields >= 7 && s.lib != 0;
    return nullptr;
}

// The launcher: a library too large to translate whole, translated only where the app's code ran
// (or waiting for the profile that says where), not one waiting after a failure.
inline bool aot_stamp_by_profile(const AotStamp& s) {
    return s.kind == AotStampKind::kProfile || (s.kind == AotStampKind::kWait && s.failures == 0);
}

// What the worker knows beside the stamp when it asks whether a library needs work.
struct AotStampFacts {
    bool have = false;            // a translation of this build of the library is in place (or beside, to swap in)
    bool current = false;         // the stamp is about this build of the library
    unsigned version = 0;         // kAotVersion
    uint64_t cpu = 0;             // aot_cpu_signature()
    int64_t now = 0;              // Unix seconds
    bool pass_to_finish = false;  // a pass was interrupted: its plan is still in the work folder
    unsigned max_failures = 0;    // failures in a row after which a library is left to the JIT
    std::function<size_t()> profile_lines;  // the profile's line count now (asked only when it matters)
};

// The worker: whether the library needs a (new) translation now.
inline bool aot_stamp_needs_work(const AotStamp& s, const AotStampFacts& f) {
    // Made by an older winapk-aot: the loader ignores that DLL, and a "bad"
    // one may have been the translator's fault (City Car's libflutter.so).
    if (f.have && s.version != f.version) return true;
    if (s.kind == AotStampKind::kBad && s.version != f.version) return true;
    // A library that rewrites its own code runs on the JIT (arm64_aot.h) until it changes.
    if (s.kind == AotStampKind::kSkip || s.kind == AotStampKind::kBad || s.kind == AotStampKind::kWritten) {
        return !f.current;
    }
    // Compiled for another CPU (-march=native): the loader leaves it alone.
    if (f.have && s.cpu != f.cpu) return true;
    if (s.kind == AotStampKind::kWait && s.failures > 0) {
        if (s.version != f.version || !f.current) return true;  // a new chance
        return s.failures < f.max_failures && f.now >= s.retry_after;
    }
    if (f.pass_to_finish) return true;
    auto grown = [&] {
        const size_t p = f.profile_lines ? f.profile_lines() : 0;
        return p >= s.lines + std::max<size_t>(200, s.lines / 4);
    };
    if (!f.have) return s.kind != AotStampKind::kWait || grown();
    if (s.kind == AotStampKind::kNone || s.kind == AotStampKind::kFull) return false;
    // From a profile: not again within the worker's interval (the stamp's retry
    // field), however fast the profile grows in an app's first sessions.
    if (s.kind == AotStampKind::kProfile && f.now < s.retry_after) return false;
    return grown();
}

// The worker, after a failure: how many in a row that makes.
inline unsigned aot_stamp_failures_after(const AotStamp& before) {
    return (before.kind == AotStampKind::kWait ? before.failures : 0) + 1;
}

}  // namespace winapk::arm64
