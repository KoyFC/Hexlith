#include "domain/lift/engine.h"

namespace hexlith::domain::lift {

arm::Stop Engine::Run(arm::CpuState& state,
                      memory::ProcessMemory& memory,
                      std::uint64_t limit,
                      std::uint64_t& executed) const {
    runtime::Context context{.cpu = &state,
                             .memory = &memory,
                             .entries = entries_,
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
        if (const runtime::Function function = entries_->Find(pc, state.thumb)) {
            const runtime::Pc next = function(context, pc);
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
