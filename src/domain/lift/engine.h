#ifndef HEXLITH_DOMAIN_LIFT_ENGINE_H
#define HEXLITH_DOMAIN_LIFT_ENGINE_H

#include "domain/arm/cpu_state.h"
#include "domain/arm/interpreter/interpreter.h"
#include "domain/lift/runtime.h"
#include "domain/memory/process_memory.h"

#include <cstdint>

namespace hexlith::domain::lift {

// Runs a thread's code natively where it has been lifted and through the
// interpreter where it has not, and tells the caller what the interpreter alone
// would have.
//
// It is arm::Run with a faster path. Given the same state and the same limit it
// stops for the same reason, leaves the same registers and memory, and counts the
// same number of instructions executed, which is what the kernel's clock is made
// of. The lifted code sees to the counting by charging each block when it enters
// it and handing back what it did not run, and to the rest by leaving for the
// interpreter whenever it cannot do exactly what the interpreter would.
class Engine {
public:
    // The table must outlive the engine.
    explicit Engine(const runtime::EntryTable& entries) : entries_(&entries) {}

    arm::Stop Run(arm::CpuState& state,
                  memory::ProcessMemory& memory,
                  std::uint64_t limit,
                  std::uint64_t& executed) const;

private:
    const runtime::EntryTable* entries_;
};

}  // namespace hexlith::domain::lift

#endif  // HEXLITH_DOMAIN_LIFT_ENGINE_H
