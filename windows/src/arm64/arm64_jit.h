// arm64_jit.h -- the arm64 JIT: dynarmic (0BSD) translating A64 to x86-64,
// behind the same Cpu state and Hooks as the interpreter (arm64_cpu.h).
//
// Built only with -DWINAPK_ARM64_JIT=ON (CMakeLists.txt fetches dynarmic);
// otherwise make_jit_engine() returns nullptr and the interpreter runs
// everything. Even with the JIT, the interpreter stays: dynarmic hands it the
// odd instruction it does not translate (InterpreterFallback), and the
// differential test checks the JIT against it.

#pragma once

#include "winapk/arm64_cpu.h"

#include <memory>

namespace winapk::arm64 {

class JitEngine {
public:
    virtual ~JitEngine() = default;
    // Runs from c.pc until c.pc == c.stop_pc, like run(c): branches out of
    // guest code go to Hooks::host_call, SVC to Hooks::svc.
    virtual void run(Cpu& c) = 0;
};

// A JIT for one host thread, whose TPIDR_EL0 is c.tpidr (read in place, so
// `c` must outlive the engine). nullptr if the JIT is not built in or all
// exclusive-monitor slots are taken; the caller then interprets.
std::unique_ptr<JitEngine> make_jit_engine(Cpu& c);

// Whether this build has the JIT.
bool jit_built();

}  // namespace winapk::arm64
