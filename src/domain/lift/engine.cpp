#include "domain/lift/engine.h"

#include "domain/lift/runtime.h"

namespace hexlith::domain::lift {
namespace {

struct LiftedCode {
    runtime::Function function = nullptr;
    bool in_module = false;
};

// The lifted code at an address: the main executable's, or the code of a module
// that is loaded there, in which case the context is given its base.
LiftedCode FindLiftedCode(const runtime::EntryTable& entries,
                          const runtime::LoadedModules* modules,
                          runtime::Context& context,
                          std::uint32_t pc,
                          bool thumb) {
    if (entries.MayCover(pc)) {
        if (const runtime::Function function = entries.Find(pc, thumb)) {
            return {.function = function, .in_module = false};
        }
    }
    if (modules != nullptr) {
        if (const auto found = modules->Find(pc, thumb)) {
            context.base = found->base;
            return {.function = found->function, .in_module = true};
        }
    }
    return {};
}

}  // namespace

arm::Stop Engine::Run(arm::CpuState& state,
                      memory::ProcessMemory& memory,
                      std::uint64_t limit,
                      std::uint64_t& executed) const {
    runtime::Context context{.cpu = &state,
                             .memory = &memory,
                             .entries = entries_,
                             .modules = modules_,
                             .budget = static_cast<std::int64_t>(limit),
                             .stop = {}};

    const auto finish = [&](arm::Stop stop) {
        executed = limit - static_cast<std::uint64_t>(context.budget);
        return stop;
    };

    while (context.budget > 0) {
        const std::uint32_t pc = state.registers.at(arm::ProgramCounter);

        // Lifted code runs until the guest leaves it, and answers where the
        // guest continues. An address is somewhere to continue; the two other
        // answers say the lifted code gave up, and have the interpreter run the
        // instruction at r15 or report why it stopped.
        const LiftedCode lifted = FindLiftedCode(*entries_, modules_, context, pc, state.thumb);
        if (lifted.function != nullptr) {
            ++statistics_.entries;
            statistics_.module_entries += lifted.in_module ? 1 : 0;
            const runtime::Pc next = lifted.function(context, pc);
            if (next == runtime::Stopped) {
                return finish(context.stop);
            }
            if (next != runtime::Interpret) {
                state.registers.at(arm::ProgramCounter) = next;
                continue;
            }
            // The block that gave up may have been entered with nothing left.
            if (context.budget <= 0) {
                break;
            }
        }

        const arm::Stop stop = arm::Step(state, memory);
        ++statistics_.interpreted;
        if (stop.Stopped()) {
            // A supervisor call has completed: r15 is already past it.
            if (stop.reason == arm::StopReason::SupervisorCall) {
                --context.budget;
            }
            return finish(stop);
        }
        --context.budget;
    }
    return finish(arm::Stop{.reason = arm::StopReason::StepLimit, .detail = 0});
}

}  // namespace hexlith::domain::lift
