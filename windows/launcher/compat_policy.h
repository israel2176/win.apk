// compat_policy.h -- R3 compatibility policy and crash-loop quarantine state for library apps.
#pragma once

#include <cstdint>
#include <string>

namespace winapk::launcher {

constexpr int kCompatEngineVersion = 1;
constexpr int kQuarantineCrashThreshold = 2;

struct CompatRecord {
    int schema = 1;
    int engine_version = 0;
    std::string verdict = "supported";  // supported | unopenable | unsupported | quarantined
    std::string reason_code;
    std::string reason_detail;
    int consecutive_startup_failures = 0;
    bool user_override = false;
};

inline bool should_display_in_library(const CompatRecord& r) {
    if (r.user_override) return true;
    return r.verdict == "supported";
}

inline std::string registry_state_for_compat(const CompatRecord& r) {
    if (r.verdict == "supported" || r.verdict == "unopenable") return "installed";
    if (r.verdict == "quarantined") return "quarantined";
    return "unsupported";
}

inline void record_startup_exit(CompatRecord* r, bool was_ready, bool runtime_failure) {
    if (!r) return;
    if (was_ready) {
        r->consecutive_startup_failures = 0;
        if (r->verdict == "quarantined") r->verdict = "supported";
        return;
    }
    if (!runtime_failure) {
        r->consecutive_startup_failures++;
        if (r->consecutive_startup_failures >= kQuarantineCrashThreshold) {
            r->verdict = "quarantined";
            r->reason_code = "CRASH_LOOP_BEFORE_WINDOW";
        }
    }
}

}  // namespace winapk::launcher
