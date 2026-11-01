#ifndef HEXLITH_DOMAIN_LIFT_EVALUATE_H
#define HEXLITH_DOMAIN_LIFT_EVALUATE_H

#include "domain/arm/cpu_state.h"
#include "domain/arm/interpreter/interpreter.h"
#include "domain/arm/primitives/alu.h"
#include "domain/arm/primitives/integer.h"
#include "domain/arm/primitives/transfer.h"
#include "domain/lift/ir.h"
#include "domain/memory/guest_memory.h"
#include "domain/memory/paged_memory.h"
#include "domain/memory/process_memory.h"

#include <array>
#include <cstdint>

namespace hexlith::domain::lift {

// What running an instruction's IR did.
enum class EvaluationKind : std::uint8_t {
    // The instruction ran. Its effect on the state is whatever its operations
    // wrote, which does not include moving r15 to the next instruction: that is
    // the block's business.
    Completed,

    // The lifted code would leave for the interpreter here. The state is as it
    // was before the instruction.
    Bailed,

    // The interpreter executed the instruction and stopped, which for the
    // supervisor call is the point.
    Stopped,
};

struct Evaluation {
    EvaluationKind kind = EvaluationKind::Completed;
    arm::Stop stop;
};

// Runs one instruction's operations against a processor state and a memory.
//
// This is the reference meaning of the IR. It calls the same primitives the
// interpreter does, one operation at a time, so that a lowering can be checked
// against the interpreter without building any C++: run the instruction both
// ways from the same state and compare. The generated code is checked against
// the interpreter separately, because it is a second translation.
template<memory::GuestMemory Memory>
Evaluation Evaluate(const ir::Instruction& instruction, arm::CpuState& state, Memory& memory);

extern template Evaluation
Evaluate<memory::PagedMemory>(const ir::Instruction&, arm::CpuState&, memory::PagedMemory&);
extern template Evaluation
Evaluate<memory::ProcessMemory>(const ir::Instruction&, arm::CpuState&, memory::ProcessMemory&);

}  // namespace hexlith::domain::lift

#endif  // HEXLITH_DOMAIN_LIFT_EVALUATE_H
