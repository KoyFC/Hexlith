#ifndef HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTION_H
#define HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTION_H

#include "domain/arm/cpu_state.h"
#include "domain/arm/instruction.h"
#include "domain/arm/interpreter/interpreter.h"
#include "domain/arm/primitives/branch.h"

#include <cstdint>

// What every part of the interpreter shares: the instruction being executed,
// where it is, and how a register is read and the program counter written.
//
// Internal to the interpreter. Nothing outside src/domain/arm/interpreter
// includes it.
namespace hexlith::domain::arm::interpreter {

template<class Memory>
struct Execution {
    // Pointers rather than references, so that the structure stays assignable.
    // Neither is ever null. The instruction is a copy: it is small, and the
    // fetch that produced it does not outlive the step.
    CpuState* state = nullptr;
    Memory* memory = nullptr;
    Instruction instruction;

    // The address of the instruction, which r15 held when it was fetched.
    std::uint32_t address = 0;

    // Set by anything that writes r15, so that the step does not then move it on
    // to the next instruction.
    bool branched = false;

    [[nodiscard]] std::uint32_t Next() const {
        return address + instruction.width;
    }

    [[nodiscard]] bool Thumb() const {
        return instruction.instruction_set == InstructionSet::Thumb;
    }
};

// The value an instruction reads from a register. r15 reads as the
// instruction's address plus the pipeline offset.
template<class Memory>
std::uint32_t Read(const Execution<Memory>& execution, RegisterIndex index) {
    if (index == ProgramCounter) {
        return primitives::ReadProgramCounter(execution.address, execution.Thumb());
    }
    return execution.state->registers.at(index);
}

// The same, rounded down to a word, which is how Thumb reads r15 to take the
// address of a literal: a Thumb instruction can sit at any halfword.
template<class Memory>
std::uint32_t ReadAligned(const Execution<Memory>& execution, RegisterIndex index) {
    if (index == ProgramCounter) {
        return primitives::ReadAlignedProgramCounter(execution.address, execution.Thumb());
    }
    return Read(execution, index);
}

inline Stop Halt(StopReason reason, std::uint32_t detail) {
    return Stop{.reason = reason, .detail = detail};
}

template<class Memory>
Stop Halt(const Execution<Memory>& execution, StopReason reason) {
    return Halt(reason, execution.instruction.encoding);
}

inline Stop Completed() {
    return Stop{};
}

// BranchWritePC: a branch that keeps the instruction set. The bits below the
// instruction size are ignored, which is what makes a Thumb `mov pc, rX` with an
// odd value land on the even address below it.
template<class Memory>
void BranchTo(Execution<Memory>& execution, std::uint32_t target) {
    execution.state->registers.at(ProgramCounter) =
        primitives::BranchWriteTarget(target, execution.state->thumb);
    execution.branched = true;
}

// BXWritePC: a branch whose target's low bit chooses the instruction set. An
// ARM target with bit 1 set is not word aligned, and the architecture calls that
// unpredictable.
template<class Memory>
Stop ExchangeTo(Execution<Memory>& execution, std::uint32_t target) {
    if (primitives::ExchangeTargetUnpredictable(target)) {
        return Halt(execution, StopReason::Unpredictable);
    }
    execution.state->thumb = primitives::ExchangeToThumb(target);
    BranchTo(execution, target);
    return Completed();
}

// What a data-processing instruction does when it writes r15. In ARMv6 that is
// a plain branch in both instruction sets, not an exchange: ARMv7 made the ARM
// form interwork, and the ARM11 predates it. An ARM target that is not word
// aligned is unpredictable.
template<class Memory>
Stop ComputedBranchTo(Execution<Memory>& execution, std::uint32_t target) {
    if (primitives::ComputedBranchUnpredictable(target, execution.Thumb())) {
        return Halt(execution, StopReason::Unpredictable);
    }
    BranchTo(execution, target);
    return Completed();
}

// Writes a general register, treating r15 as a computed branch.
template<class Memory>
Stop Write(Execution<Memory>& execution, RegisterIndex index, std::uint32_t value) {
    if (index == ProgramCounter) {
        return ComputedBranchTo(execution, value);
    }
    execution.state->registers.at(index) = value;
    return Completed();
}

template<class Memory>
void SetResultFlags(Execution<Memory>& execution, std::uint32_t result) {
    // NOLINTNEXTLINE(readability-magic-numbers): a bit position
    execution.state->negative = (result >> 31U) != 0;
    execution.state->zero = result == 0;
}

}  // namespace hexlith::domain::arm::interpreter

#endif  // HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTION_H
