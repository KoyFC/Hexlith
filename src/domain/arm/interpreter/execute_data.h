#ifndef HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTE_DATA_H
#define HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTE_DATA_H

#include "domain/arm/cpu_state.h"
#include "domain/arm/instruction.h"
#include "domain/arm/interpreter/execution.h"
#include "domain/arm/primitives/integer.h"
#include "domain/arm/primitives/multiply.h"
#include "domain/arm/primitives/packing.h"
#include "domain/arm/primitives/parallel.h"
#include "domain/arm/primitives/saturate.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>

// Everything that computes in registers: data processing, the multiplies, the
// media instructions, the status register, and the branches. Each one reads its
// operands, calls the primitive library, and writes its result.
//
// Internal to the interpreter.
namespace hexlith::domain::arm::interpreter {

template<class Memory>
primitives::ConditionFlags Flags(const Execution<Memory>& execution) {
    return {.negative = execution.state->negative,
            .zero = execution.state->zero,
            .carry = execution.state->carry,
            .overflow = execution.state->overflow};
}

// Whether any of the registers is r15, for the instructions where reading or
// writing it is unpredictable.
inline bool AnyIsProgramCounter(std::initializer_list<RegisterIndex> registers) {
    return std::ranges::any_of(registers,
                               [](RegisterIndex index) { return index == ProgramCounter; });
}

// The second operand of a data-processing instruction and the carry the
// shifter leaves behind.
template<class Memory>
primitives::ShiftResult ShifterOperand(const Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const bool carry = execution.state->carry;

    switch (instruction.operand) {
    case OperandForm::Immediate: {
        // An ARM immediate rotated by a nonzero amount sets the carry from its
        // top bit. The rotation is bits 11 to 8 of the encoding. Thumb
        // immediates are never rotated.
        const bool rotated = !execution.Thumb() && ((instruction.encoding >> 8U) & 0xFU) != 0;
        return {.value = instruction.immediate,
                .carry = primitives::ImmediateCarry(instruction.immediate, rotated, carry)};
    }
    case OperandForm::Register:
        return {.value = Read(execution, instruction.second), .carry = carry};
    case OperandForm::ShiftedRegister: {
        const std::uint32_t amount = instruction.shift_register == NoRegister
                                         ? instruction.shift_amount
                                         : Read(execution, instruction.shift_register) & 0xFFU;
        return primitives::Shift(
            instruction.shift, Read(execution, instruction.second), amount, carry);
    }
    case OperandForm::None:
        break;
    }
    return {.value = 0, .carry = carry};
}

struct AluResult {
    std::uint32_t value = 0;
    bool carry = false;
    bool overflow = false;

    // Logical operations leave V alone and set C from the shifter.
    bool arithmetic = false;
};

inline AluResult Arithmetic(primitives::AddResult sum) {
    return {.value = sum.value, .carry = sum.carry, .overflow = sum.overflow, .arithmetic = true};
}

inline AluResult Logical(std::uint32_t value, bool shifter_carry) {
    return {.value = value, .carry = shifter_carry, .overflow = false, .arithmetic = false};
}

inline AluResult
Compute(Operation operation, std::uint32_t first, primitives::ShiftResult second, bool carry) {
    using primitives::AddWithCarry;
    const std::uint32_t operand = second.value;

    switch (operation) {
    case Operation::And:
    case Operation::Tst:
        return Logical(first & operand, second.carry);
    case Operation::Eor:
    case Operation::Teq:
        return Logical(first ^ operand, second.carry);
    case Operation::Orr:
        return Logical(first | operand, second.carry);
    case Operation::Bic:
        return Logical(first & ~operand, second.carry);
    case Operation::Mov:
        return Logical(operand, second.carry);
    case Operation::Mvn:
        return Logical(~operand, second.carry);
    case Operation::Sub:
    case Operation::Cmp:
        return Arithmetic(AddWithCarry(first, ~operand, true));
    case Operation::Rsb:
        return Arithmetic(AddWithCarry(operand, ~first, true));
    case Operation::Add:
    case Operation::Cmn:
        return Arithmetic(AddWithCarry(first, operand, false));
    case Operation::Adc:
        return Arithmetic(AddWithCarry(first, operand, carry));
    case Operation::Sbc:
        return Arithmetic(AddWithCarry(first, ~operand, carry));
    case Operation::Rsc:
        return Arithmetic(AddWithCarry(operand, ~first, carry));
    default:
        break;
    }
    return {};
}

template<class Memory>
Stop ExecuteDataProcessing(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;

    // A shift by a register reads r15 as twelve bytes ahead on some cores and
    // eight on others, and the architecture calls it unpredictable.
    if (instruction.shift_register != NoRegister &&
        AnyIsProgramCounter({instruction.destination,
                             instruction.first,
                             instruction.second,
                             instruction.shift_register})) {
        return Halt(execution, StopReason::Unpredictable);
    }

    // With the S bit, a write to r15 also copies the saved status register into
    // the CPSR. That is how an exception handler returns, and user mode has no
    // saved status register to copy.
    if (instruction.destination == ProgramCounter && instruction.updates_flags) {
        return Halt(execution, StopReason::Unpredictable);
    }

    // Thumb's `add rd, pc, #imm` takes an address, and reads r15 rounded down
    // to a word the way a literal load does. Every other read of r15 is plain.
    const bool takes_address = execution.Thumb() && instruction.operand == OperandForm::Immediate;
    std::uint32_t first = 0;
    if (instruction.first != NoRegister) {
        first = takes_address ? ReadAligned(execution, instruction.first)
                              : Read(execution, instruction.first);
    }
    const AluResult result =
        Compute(instruction.operation, first, ShifterOperand(execution), execution.state->carry);

    if (instruction.destination != NoRegister) {
        const Stop written = Write(execution, instruction.destination, result.value);
        if (written.Stopped()) {
            return written;
        }
    }

    if (instruction.updates_flags) {
        SetResultFlags(execution, result.value);
        execution.state->carry = result.carry;
        if (result.arithmetic) {
            execution.state->overflow = result.overflow;
        }
    }
    return Completed();
}

// MUL and MLA, and the four long multiplies. With S they set N and Z from the
// result and leave C and V alone, which ARMv6 defines and earlier
// architectures did not.
template<class Memory>
Stop ExecuteMultiply(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    if (AnyIsProgramCounter(
            {instruction.destination, instruction.first, instruction.second, instruction.third})) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const std::uint32_t x = Read(execution, instruction.second);
    const std::uint32_t y = Read(execution, instruction.third);
    std::uint32_t product = x * y;
    if (instruction.operation == Operation::Mla) {
        product += Read(execution, instruction.first);
    }

    execution.state->registers.at(instruction.destination) = product;
    if (instruction.updates_flags) {
        SetResultFlags(execution, product);
    }
    return Completed();
}

template<class Memory>
Stop ExecuteLongMultiply(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const RegisterIndex low = instruction.destination;
    const RegisterIndex high = instruction.destination_high;
    if (AnyIsProgramCounter({low, high, instruction.second, instruction.third}) || low == high) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const std::uint32_t x = Read(execution, instruction.second);
    const std::uint32_t y = Read(execution, instruction.third);
    const std::uint64_t accumulator =
        (static_cast<std::uint64_t>(Read(execution, high)) << 32U) | Read(execution, low);

    std::uint64_t result = 0;
    switch (instruction.operation) {
    case Operation::Umull:
        result = static_cast<std::uint64_t>(x) * y;
        break;
    case Operation::Umlal:
        result = (static_cast<std::uint64_t>(x) * y) + accumulator;
        break;
    case Operation::Smull:
        result = static_cast<std::uint64_t>(primitives::Signed(x) * primitives::Signed(y));
        break;
    case Operation::Smlal:
        result =
            static_cast<std::uint64_t>(primitives::Signed(x) * primitives::Signed(y)) + accumulator;
        break;
    case Operation::Umaal:
        result = primitives::MultiplyAccumulateAccumulate(
            x, y, Read(execution, low), Read(execution, high));
        break;
    default:
        return Halt(execution, StopReason::UndefinedInstruction);
    }

    execution.state->registers.at(low) = static_cast<std::uint32_t>(result);
    execution.state->registers.at(high) = static_cast<std::uint32_t>(result >> 32U);
    if (instruction.updates_flags) {
        execution.state->negative = (result >> 63U) != 0;
        execution.state->zero = result == 0;
    }
    return Completed();
}

// Writes a result that may have set Q, for the multiplies and saturating
// operations that report it.
template<class Memory>
Stop WriteReportingQ(Execution<Memory>& execution, std::uint32_t value, bool sets_q) {
    execution.state->registers.at(execution.instruction.destination) = value;
    if (sets_q) {
        execution.state->saturated = true;
    }
    return Completed();
}

// The halfword multiplies of ARMv5TE.
template<class Memory>
Stop ExecuteHalfwordMultiply(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    if (AnyIsProgramCounter({instruction.destination,
                             instruction.destination_high,
                             instruction.first,
                             instruction.second,
                             instruction.third})) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const std::uint32_t x = Read(execution, instruction.second);
    const std::uint32_t y = Read(execution, instruction.third);
    const bool x_high = instruction.right_half_high;
    const bool y_high = instruction.left_half_high;

    switch (instruction.operation) {
    case Operation::SmulHalf:
        return WriteReportingQ(execution, primitives::MultiplyHalves(x, x_high, y, y_high), false);
    case Operation::SmlaHalf: {
        const primitives::AccumulateResult result = primitives::MultiplyHalvesAccumulate(
            x, x_high, y, y_high, Read(execution, instruction.first));
        return WriteReportingQ(execution, result.value, result.overflowed);
    }
    case Operation::SmulwHalf:
        return WriteReportingQ(execution, primitives::MultiplyWordByHalf(x, y, y_high), false);
    case Operation::SmlawHalf: {
        const primitives::AccumulateResult result = primitives::MultiplyWordByHalfAccumulate(
            x, y, y_high, Read(execution, instruction.first));
        return WriteReportingQ(execution, result.value, result.overflowed);
    }
    case Operation::SmlalHalf: {
        const RegisterIndex low = instruction.destination;
        const RegisterIndex high = instruction.destination_high;
        if (low == high) {
            return Halt(execution, StopReason::Unpredictable);
        }
        const std::uint64_t accumulator =
            (static_cast<std::uint64_t>(Read(execution, high)) << 32U) | Read(execution, low);
        const std::uint64_t result =
            primitives::MultiplyHalvesAccumulateLong(x, x_high, y, y_high, accumulator);
        execution.state->registers.at(low) = static_cast<std::uint32_t>(result);
        execution.state->registers.at(high) = static_cast<std::uint32_t>(result >> 32U);
        return Completed();
    }
    default:
        break;
    }
    return Halt(execution, StopReason::UndefinedInstruction);
}

// The dual and most significant word multiplies of ARMv6. The X and R suffixes
// are bit 5 of the encoding in all of them; these exist only in ARM.
template<class Memory>
Stop ExecuteSignedMultiply(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    if (AnyIsProgramCounter({instruction.destination,
                             instruction.destination_high,
                             instruction.first,
                             instruction.second,
                             instruction.third})) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const bool bit5 = ((instruction.encoding >> 5U) & 1U) != 0;
    const std::uint32_t x = Read(execution, instruction.second);
    const std::uint32_t y = Read(execution, instruction.third);
    const std::uint32_t accumulator =
        instruction.first == NoRegister ? 0 : Read(execution, instruction.first);

    switch (instruction.operation) {
    case Operation::Smuad:
    case Operation::Smlad: {
        const auto result = primitives::DualMultiplyAdd(x, y, bit5, accumulator);
        return WriteReportingQ(execution, result.value, result.overflowed);
    }
    case Operation::Smusd:
    case Operation::Smlsd: {
        const auto result = primitives::DualMultiplySubtract(x, y, bit5, accumulator);
        return WriteReportingQ(execution, result.value, result.overflowed);
    }
    case Operation::Smlald:
    case Operation::Smlsld: {
        const RegisterIndex low = instruction.destination;
        const RegisterIndex high = instruction.destination_high;
        if (low == high) {
            return Halt(execution, StopReason::Unpredictable);
        }
        const std::uint64_t wide =
            (static_cast<std::uint64_t>(Read(execution, high)) << 32U) | Read(execution, low);
        const std::uint64_t result = instruction.operation == Operation::Smlald
                                         ? primitives::DualMultiplyAddLong(x, y, bit5, wide)
                                         : primitives::DualMultiplySubtractLong(x, y, bit5, wide);
        execution.state->registers.at(low) = static_cast<std::uint32_t>(result);
        execution.state->registers.at(high) = static_cast<std::uint32_t>(result >> 32U);
        return Completed();
    }
    case Operation::Smmul:
    case Operation::Smmla:
    case Operation::Smmls:
        return WriteReportingQ(
            execution,
            primitives::MostSignificantWord(
                x, y, accumulator, instruction.operation == Operation::Smmls, bit5),
            false);
    default:
        break;
    }
    return Halt(execution, StopReason::UndefinedInstruction);
}

// QADD, QSUB, QDADD and QDSUB, which take Rm as the first operand and Rn as the
// one that is doubled.
template<class Memory>
Stop ExecuteSaturatingArithmetic(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    if (AnyIsProgramCounter({instruction.destination, instruction.first, instruction.second})) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const std::uint32_t x = Read(execution, instruction.second);
    const std::uint32_t y = Read(execution, instruction.first);
    primitives::SaturateResult result;
    switch (instruction.operation) {
    case Operation::Qadd:
        result = primitives::SaturatingAdd(x, y);
        break;
    case Operation::Qsub:
        result = primitives::SaturatingSubtract(x, y);
        break;
    case Operation::QdoubleAdd:
        result = primitives::SaturatingDoubleAdd(x, y);
        break;
    case Operation::QdoubleSub:
        result = primitives::SaturatingDoubleSubtract(x, y);
        break;
    default:
        return Halt(execution, StopReason::UndefinedInstruction);
    }
    return WriteReportingQ(execution, result.value, result.saturated);
}

// SSAT, USAT, SSAT16 and USAT16. The signed forms saturate to one more bit than
// the field holds.
template<class Memory>
Stop ExecuteSaturate(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    if (AnyIsProgramCounter({instruction.destination, instruction.second})) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const std::uint32_t value = Read(execution, instruction.second);
    const unsigned field = instruction.immediate;

    switch (instruction.operation) {
    case Operation::Ssat:
    case Operation::Usat: {
        // An arithmetic shift right by zero means by thirty-two here too.
        constexpr std::uint32_t FullShift = 32;
        const bool right = instruction.shift == ShiftKind::ArithmeticRight;
        const std::uint32_t amount =
            right && instruction.shift_amount == 0 ? FullShift : instruction.shift_amount;
        const std::int64_t operand =
            primitives::Signed(primitives::Shift(instruction.shift, value, amount, false).value);
        const primitives::SaturateResult result =
            instruction.operation == Operation::Ssat
                ? primitives::SignedSaturate(operand, field + 1)
                : primitives::UnsignedSaturate(operand, field);
        return WriteReportingQ(execution, result.value, result.saturated);
    }
    case Operation::Ssat16: {
        const auto result = primitives::SignedSaturateHalves(value, field + 1);
        return WriteReportingQ(execution, result.value, result.saturated);
    }
    case Operation::Usat16: {
        const auto result = primitives::UnsignedSaturateHalves(value, field);
        return WriteReportingQ(execution, result.value, result.saturated);
    }
    default:
        break;
    }
    return Halt(execution, StopReason::UndefinedInstruction);
}

// The extensions, with and without an addend. The rotation is the shift amount.
template<class Memory>
Stop ExecuteExtension(Execution<Memory>& execution) {
    namespace p = primitives;
    const Instruction& instruction = execution.instruction;
    if (AnyIsProgramCounter({instruction.destination, instruction.second}) ||
        (instruction.first == ProgramCounter)) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const std::uint32_t rotated =
        p::Rotate(Read(execution, instruction.second), instruction.shift_amount);
    const std::uint32_t addend =
        instruction.first == NoRegister ? 0 : Read(execution, instruction.first);

    std::uint32_t result = 0;
    switch (instruction.operation) {
    case Operation::Sxtb:
    case Operation::Sxtab:
        result = addend + p::SignExtendByte(rotated);
        break;
    case Operation::Sxth:
    case Operation::Sxtah:
        result = addend + p::SignExtendHalf(rotated);
        break;
    case Operation::Uxtb:
    case Operation::Uxtab:
        result = addend + p::ZeroExtendByte(rotated);
        break;
    case Operation::Uxth:
    case Operation::Uxtah:
        result = addend + p::ZeroExtendHalf(rotated);
        break;
    case Operation::Sxtb16:
    case Operation::Sxtab16:
        result = p::AddHalves(addend, p::SignExtendBytePair(rotated));
        break;
    case Operation::Uxtb16:
    case Operation::Uxtab16:
        result = p::AddHalves(addend, p::ZeroExtendBytePair(rotated));
        break;
    default:
        return Halt(execution, StopReason::UndefinedInstruction);
    }
    execution.state->registers.at(instruction.destination) = result;
    return Completed();
}

// Packing, reversal, the leading zero count, selection, the sum of absolute
// differences and the parallel arithmetic: everything else in the media space
// that takes registers and writes one.
template<class Memory>
Stop ExecuteMedia(Execution<Memory>& execution) {
    namespace p = primitives;
    const Instruction& instruction = execution.instruction;
    if (AnyIsProgramCounter(
            {instruction.destination, instruction.first, instruction.second, instruction.third})) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const auto operand = [&execution](RegisterIndex index) {
        return index == NoRegister ? 0U : Read(execution, index);
    };
    const std::uint32_t first = operand(instruction.first);
    const std::uint32_t second = operand(instruction.second);

    std::uint32_t result = 0;
    switch (instruction.operation) {
    case Operation::Pkhbt:
        result = p::PackBottomTop(
            first, p::Shift(ShiftKind::LogicalLeft, second, instruction.shift_amount, false).value);
        break;
    case Operation::Pkhtb: {
        // An arithmetic shift by zero means by thirty-two, as elsewhere.
        constexpr std::uint32_t FullShift = 32;
        const std::uint32_t amount =
            instruction.shift_amount == 0 ? FullShift : instruction.shift_amount;
        result = p::PackTopBottom(
            first, p::Shift(ShiftKind::ArithmeticRight, second, amount, false).value);
        break;
    }
    case Operation::Rev:
        result = p::ReverseBytes(second);
        break;
    case Operation::Rev16:
        result = p::ReverseBytesInHalves(second);
        break;
    case Operation::Revsh:
        result = p::ReverseSignedHalf(second);
        break;
    case Operation::CountLeadingZeros:
        result = p::CountLeadingZeros(second);
        break;
    case Operation::Sel:
        result = p::Select(first, second, execution.state->greater_or_equal);
        break;
    case Operation::Usad8:
    case Operation::Usada8: {
        // USADA8 Rd, Rn, Rm, Ra: the addend is in `first`, which reads as zero
        // for USAD8.
        const std::uint32_t accumulator = first;
        result = p::SumOfAbsoluteDifferences(second, operand(instruction.third), accumulator);
        break;
    }
    case Operation::ParallelArithmetic: {
        const p::ParallelResult parallel = p::ParallelArithmetic(
            instruction.parallel_prefix, instruction.parallel_shape, first, second);
        result = parallel.value;
        if (p::SetsGreaterOrEqual(instruction.parallel_prefix)) {
            execution.state->greater_or_equal = parallel.greater_or_equal;
        }
        break;
    }
    default:
        return Halt(execution, StopReason::UndefinedInstruction);
    }
    execution.state->registers.at(instruction.destination) = result;
    return Completed();
}

// MRS and MSR, on the CPSR only: user mode has no saved status register.
template<class Memory>
Stop ExecuteStatusRegister(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    if (instruction.saved_status) {
        return Halt(execution, StopReason::Unpredictable);
    }

    if (instruction.operation == Operation::MoveFromStatus) {
        if (instruction.destination == ProgramCounter) {
            return Halt(execution, StopReason::Unpredictable);
        }
        execution.state->registers.at(instruction.destination) = PackCpsr(*execution.state);
        return Completed();
    }

    // The register form keeps the field mask in the immediate; the immediate
    // form needs the immediate for the value and keeps the mask in `third`.
    const bool immediate = instruction.operand == OperandForm::Immediate;
    if (!immediate && instruction.second == ProgramCounter) {
        return Halt(execution, StopReason::Unpredictable);
    }
    const std::uint32_t mask = immediate ? instruction.third : instruction.immediate;
    const std::uint32_t value =
        immediate ? instruction.immediate : Read(execution, instruction.second);
    WriteCpsrFromUserMode(*execution.state, value, mask);
    return Completed();
}

// B, BL and BLX to an immediate, and BX and BLX through a register.
template<class Memory>
Stop ExecuteBranch(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const std::uint32_t link = execution.Next() | (execution.Thumb() ? 1U : 0U);

    switch (instruction.operation) {
    case Operation::Branch:
        BranchTo(execution, BranchTarget(instruction, execution.address));
        return Completed();
    case Operation::BranchLink:
        execution.state->registers.at(LinkRegister) = link;
        BranchTo(execution, BranchTarget(instruction, execution.address));
        return Completed();
    case Operation::BranchLinkExchangeImmediate:
        execution.state->registers.at(LinkRegister) = link;
        execution.state->thumb = !execution.Thumb();
        BranchTo(execution, BranchTarget(instruction, execution.address));
        return Completed();
    case Operation::BranchExchange:
        return ExchangeTo(execution, Read(execution, instruction.second));
    case Operation::BranchLinkExchangeRegister: {
        if (instruction.second == ProgramCounter) {
            return Halt(execution, StopReason::Unpredictable);
        }
        // The target is read before the link register is written, because the
        // target may be the link register.
        const std::uint32_t target = Read(execution, instruction.second);
        const Stop stop = ExchangeTo(execution, target);
        if (stop.Stopped()) {
            return stop;
        }
        execution.state->registers.at(LinkRegister) = link;
        return Completed();
    }
    default:
        break;
    }
    return Halt(execution, StopReason::UndefinedInstruction);
}

}  // namespace hexlith::domain::arm::interpreter

#endif  // HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTE_DATA_H
