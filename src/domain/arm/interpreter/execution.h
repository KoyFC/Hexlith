#ifndef HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTION_H
#define HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTION_H

#include "domain/arm/cpu_state.h"
#include "domain/arm/instruction.h"
#include "domain/arm/interpreter/interpreter.h"

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

// How far ahead of an instruction r15 reads: two instructions in ARM, one in
// Thumb, for the pipeline reason instruction.h gives.
constexpr std::uint32_t ArmReadOffset = 8;
constexpr std::uint32_t ThumbReadOffset = 4;

// The value an instruction reads from a register. r15 reads as the
// instruction's address plus the pipeline offset.
template<class Memory>
std::uint32_t Read(const Execution<Memory>& execution, RegisterIndex index) {
    if (index == ProgramCounter) {
        return execution.address + (execution.Thumb() ? ThumbReadOffset : ArmReadOffset);
    }
    return execution.state->registers.at(index);
}

// The same, rounded down to a word, which is how Thumb reads r15 to take the
// address of a literal: a Thumb instruction can sit at any halfword.
template<class Memory>
std::uint32_t ReadAligned(const Execution<Memory>& execution, RegisterIndex index) {
    constexpr std::uint32_t WordMask = ~3U;
    const std::uint32_t value = Read(execution, index);
    return index == ProgramCounter && execution.Thumb() ? value & WordMask : value;
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
    constexpr std::uint32_t ArmMask = ~3U;
    constexpr std::uint32_t ThumbMask = ~1U;
    execution.state->registers.at(ProgramCounter) =
        target & (execution.state->thumb ? ThumbMask : ArmMask);
    execution.branched = true;
}

// BXWritePC: a branch whose target's low bit chooses the instruction set. An
// ARM target with bit 1 set is not word aligned, and the architecture calls that
// unpredictable.
template<class Memory>
Stop ExchangeTo(Execution<Memory>& execution, std::uint32_t target) {
    constexpr std::uint32_t ThumbBit = 1;
    constexpr std::uint32_t HalfwordBit = 2;
    const bool to_thumb = (target & ThumbBit) != 0;
    if (!to_thumb && (target & HalfwordBit) != 0) {
        return Halt(execution, StopReason::Unpredictable);
    }
    execution.state->thumb = to_thumb;
    BranchTo(execution, target);
    return Completed();
}

// What a data-processing instruction does when it writes r15. In ARMv6 that is
// a plain branch in both instruction sets, not an exchange: ARMv7 made the ARM
// form interwork, and the ARM11 predates it. An ARM target that is not word
// aligned is unpredictable.
template<class Memory>
Stop ComputedBranchTo(Execution<Memory>& execution, std::uint32_t target) {
    constexpr std::uint32_t LowBits = 3;
    if (!execution.Thumb() && (target & LowBits) != 0) {
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
    execution.state->negative = (result >> 31U) != 0;
    execution.state->zero = result == 0;
}

}  // namespace hexlith::domain::arm::interpreter

#endif  // HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTION_H
