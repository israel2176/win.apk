// A stand-in for a translation (<lib>.so.aot.dll) in aot_guard_test.cpp: a
// module the loader accepts (arm64_aot.cpp, load_dll) with no functions, so
// the test's process has "translated code loaded" through the real path.

#include "winapk/arm64_aot.h"

#include <cstddef>

using winapk::arm64::AotModule;
using winapk::arm64::AotRuntime;
using winapk::arm64::Cpu;

namespace {
uint64_t g_base;
AotRuntime g_runtime;
uint32_t g_off;
}  // namespace

// The text hash aot_guard_test.cpp passes to aot_load.
constexpr uint64_t kGuardTestTextHash = 0x6775617264746573ull;

extern "C" __declspec(dllexport) const AotModule winapk_aot_module = {
    winapk::arm64::kAotVersion,
    {offsetof(Cpu, sp), offsetof(Cpu, nzcv), offsetof(Cpu, tpidr), offsetof(Cpu, v)},
    kGuardTestTextHash,
    &g_base,
    &g_runtime,
    nullptr,
    0,
    &g_off,
};
