#ifndef HEXLITH_DOMAIN_LIFT_ENGINE_H
#define HEXLITH_DOMAIN_LIFT_ENGINE_H

#include "domain/arm/cpu_state.h"
#include "domain/arm/interpreter/interpreter.h"
#include "domain/memory/process_memory.h"

#include <cstdint>

namespace hexlith::domain::lift {

namespace runtime {
class EntryTable;
class LoadedModules;
}  // namespace runtime

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
    // What the engine did, for knowing how much of a run was lifted.
    struct Statistics {
        // Instructions the interpreter executed because nothing lifted covered
        // them, or because lifted code gave up at them.
        std::uint64_t interpreted = 0;

        // Times lifted code was entered, in the main executable or a module.
        std::uint64_t entries = 0;

        // The part of those that were into a module.
        std::uint64_t module_entries = 0;
    };

    // The table, and the modules when there are lifted ones, must outlive the
    // engine. A module's code is found through what the loader says is loaded.
    explicit Engine(const runtime::EntryTable& entries,
                    const runtime::LoadedModules* modules = nullptr)
        : entries_(&entries), modules_(modules) {}

    arm::Stop Run(arm::CpuState& state,
                  memory::ProcessMemory& memory,
                  std::uint64_t limit,
                  std::uint64_t& executed) const;

    [[nodiscard]] const Statistics& Counted() const {
        return statistics_;
    }

private:
    const runtime::EntryTable* entries_;
    const runtime::LoadedModules* modules_;
    mutable Statistics statistics_;
};

}  // namespace hexlith::domain::lift

#endif  // HEXLITH_DOMAIN_LIFT_ENGINE_H
