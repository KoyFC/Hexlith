#ifndef HEXLITH_DOMAIN_ARM_INTERPRETER_INTERPRETER_H
#define HEXLITH_DOMAIN_ARM_INTERPRETER_INTERPRETER_H

#include "domain/arm/cpu_state.h"
#include "domain/memory/guest_memory.h"
#include "domain/memory/paged_memory.h"

#include <cstdint>
#include <string_view>

namespace hexlith::domain::arm {

// Why the interpreter stopped. Everything other than None and StepLimit is
// something the caller has to deal with: the kernel for a supervisor call, the
// test or the tool for everything else.
//
// The interpreter never guesses. An instruction it cannot execute exactly is a
// stop with a reason, and the state is left as it was before the instruction,
// so that the caller sees the instruction that stopped it and can retry it.
enum class StopReason : std::uint8_t {
    // The instruction completed. Step returns this; Run never does.
    None,

    // Run executed as many instructions as it was allowed to.
    StepLimit,

    // SVC. The program counter has already moved past it, because the kernel
    // returns to the next instruction. The detail is the immediate.
    SupervisorCall,

    // BKPT. The detail is the immediate.
    Breakpoint,

    // The word is not an instruction. The detail is the encoding.
    UndefinedInstruction,

    // The instruction is defined but this use of it is unpredictable, such as a
    // load multiple that writes back into a register it loads. The detail is the
    // encoding.
    Unpredictable,

    // The instruction needs a privileged mode, and the game runs in user mode.
    // The detail is the encoding.
    Privileged,

    // The instruction is defined and could run, but this interpreter does not
    // implement it, or not in the mode the processor is in: a coprocessor other
    // than the floating point unit, big-endian data, an FPSCR mode outside the
    // ones ADR 0010 handles. The detail is the encoding.
    Unsupported,

    // A memory access failed. The detail is the address that failed.
    FetchFault,
    ReadFault,
    WriteFault,

    // An access that the ARM11 requires to be aligned was not. The detail is the
    // address.
    AlignmentFault,
};

std::string_view Describe(StopReason reason);

struct Stop {
    StopReason reason = StopReason::None;
    std::uint32_t detail = 0;

    [[nodiscard]] bool Stopped() const {
        return reason != StopReason::None;
    }
};

// Executes the one instruction at r15.
//
// The interpreter is the executable reference of ADR 0003: every operation goes
// through the shared primitive library that generated code also calls, so the
// two agree by construction and a difference between them is a bug in which
// call one of them makes.
//
// It is not written to be fast. It decodes every instruction it executes, and
// it is meant to run a test, a function, or a scene for comparison, not a game.
template<memory::GuestMemory Memory>
Stop Step(CpuState& state, Memory& memory);

// Executes instructions until one stops or the limit is reached.
template<memory::GuestMemory Memory>
Stop Run(CpuState& state, Memory& memory, std::uint64_t limit);

// The interpreter is compiled once, in interpreter.cpp, for each memory it runs
// against, rather than in every file that calls it.
extern template Stop Step<memory::PagedMemory>(CpuState&, memory::PagedMemory&);
extern template Stop Run<memory::PagedMemory>(CpuState&, memory::PagedMemory&, std::uint64_t);

}  // namespace hexlith::domain::arm

#endif  // HEXLITH_DOMAIN_ARM_INTERPRETER_INTERPRETER_H
