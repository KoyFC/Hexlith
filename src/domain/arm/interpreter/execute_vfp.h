#ifndef HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTE_VFP_H
#define HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTE_VFP_H

#include "domain/arm/cpu_state.h"
#include "domain/arm/instruction.h"
#include "domain/arm/interpreter/execute_transfer.h"
#include "domain/arm/interpreter/execution.h"
#include "domain/arm/primitives/vfp.h"

#include <array>
#include <cstdint>
#include <optional>

// Floating point: VFPv2 arithmetic, comparison, conversion, moves and
// transfers, through the primitives of ADR 0010.
//
// Before anything computes, FPSCR has to hold a mode the primitives implement.
// A write of FPSCR that would leave that set stops at the write, and an
// operation started in such a mode stops too, so nothing runs in a mode that
// would give a different answer from the hardware.
//
// Internal to the interpreter.
namespace hexlith::domain::arm::interpreter {

namespace vfp {
// The bits of FPSCR the VFP11 implements; the rest read as zero. Source: the
// FPSCR description in the VFP chapter of the ARM1176JZF-S technical reference
// manual: the flags, DN, FZ, the rounding mode, STRIDE, LEN, the trap enables
// and the cumulative exception flags.
constexpr std::uint32_t ImplementedFpscrBits = 0xF3F79F9F;

// The system register numbers of VMRS and VMSR. Only FPSCR is reachable from
// user mode.
constexpr std::uint32_t Fpscr = 0b0001;
}  // namespace vfp

template<class Float>
primitives::FloatBits<Float> ReadFloat(const CpuState& state, RegisterIndex index) {
    if constexpr (std::is_same_v<Float, double>) {
        return ReadDouble(state, index);
    } else {
        return state.float_registers.at(index);
    }
}

template<class Float>
void WriteFloat(CpuState& state, RegisterIndex index, primitives::FloatBits<Float> value) {
    if constexpr (std::is_same_v<Float, double>) {
        WriteDouble(state, index, value);
    } else {
        state.float_registers.at(index) = value;
    }
}

// The operations whose operands and result share one precision.
template<class Float, class Memory>
Stop ExecuteFloatArithmetic(Execution<Memory>& execution) {
    namespace p = primitives;
    const Instruction& instruction = execution.instruction;
    CpuState& state = *execution.state;
    const std::uint32_t mode = state.fpscr;

    const auto source = [&state](RegisterIndex index) {
        return index == NoRegister ? p::FloatBits<Float>{0} : ReadFloat<Float>(state, index);
    };
    const p::FloatBits<Float> n = source(instruction.vfp_first);
    const p::FloatBits<Float> m = source(instruction.vfp_second);
    const p::FloatBits<Float> d = source(instruction.vfp_destination);

    p::FloatBits<Float> result = 0;
    switch (instruction.operation) {
    case Operation::VfpMultiplyAccumulate:
        result = p::FloatMultiplyAccumulate<Float>(d, n, m, false, false, mode);
        break;
    case Operation::VfpMultiplySubtract:
        result = p::FloatMultiplyAccumulate<Float>(d, n, m, true, false, mode);
        break;
    case Operation::VfpNegatedMultiplyAccumulate:
        result = p::FloatMultiplyAccumulate<Float>(d, n, m, true, true, mode);
        break;
    case Operation::VfpNegatedMultiplySubtract:
        result = p::FloatMultiplyAccumulate<Float>(d, n, m, false, true, mode);
        break;
    case Operation::VfpMultiply:
        result = p::FloatMultiply<Float>(n, m, mode);
        break;
    case Operation::VfpNegatedMultiply:
        result = p::FloatNegate<Float>(p::FloatMultiply<Float>(n, m, mode));
        break;
    case Operation::VfpAdd:
        result = p::FloatAdd<Float>(n, m, mode);
        break;
    case Operation::VfpSubtract:
        result = p::FloatSubtract<Float>(n, m, mode);
        break;
    case Operation::VfpDivide:
        result = p::FloatDivide<Float>(n, m, mode);
        break;
    case Operation::VfpCopy:
        result = m;
        break;
    case Operation::VfpAbsolute:
        result = p::FloatAbsolute<Float>(m);
        break;
    case Operation::VfpNegate:
        result = p::FloatNegate<Float>(m);
        break;
    case Operation::VfpSquareRoot:
        result = p::FloatSquareRoot<Float>(m, mode);
        break;
    case Operation::VfpCompare:
    case Operation::VfpCompareSignalling:
        // A comparison writes the flags and no register. The signalling form
        // differs only in the exception flag it raises for a quiet NaN, which
        // ADR 0010 leaves unmodelled.
        state.fpscr = (state.fpscr & ~p::fpscr::FlagsMask) | p::FloatCompare<Float>(n, m, mode);
        return Completed();
    default:
        return Halt(execution, StopReason::UndefinedInstruction);
    }

    WriteFloat<Float>(state, instruction.vfp_destination, result);
    return Completed();
}

template<class Memory>
Stop ExecuteFloatConversion(Execution<Memory>& execution) {
    namespace p = primitives;
    const Instruction& instruction = execution.instruction;
    CpuState& state = *execution.state;
    const std::uint32_t mode = state.fpscr;
    const RegisterIndex to = instruction.vfp_destination;
    const RegisterIndex from = instruction.vfp_second;

    switch (instruction.operation) {
    case Operation::VfpConvertPrecision:
        if (instruction.vfp_sources_double) {
            state.float_registers.at(to) = p::FloatNarrow(ReadDouble(state, from), mode);
        } else {
            WriteDouble(state, to, p::FloatWiden(state.float_registers.at(from), mode));
        }
        return Completed();
    case Operation::VfpConvertFromInteger: {
        const std::uint32_t integer = state.float_registers.at(from);
        if (instruction.vfp_destination_double) {
            WriteDouble(state, to, p::IntegerToFloat<double>(integer, instruction.integer_signed));
        } else {
            state.float_registers.at(to) =
                p::IntegerToFloat<float>(integer, instruction.integer_signed);
        }
        return Completed();
    }
    case Operation::VfpConvertToInteger:
        state.float_registers.at(to) =
            instruction.vfp_sources_double
                ? p::FloatToInteger<double>(ReadDouble(state, from),
                                            instruction.integer_signed,
                                            instruction.rounds_towards_zero,
                                            mode)
                : p::FloatToInteger<float>(state.float_registers.at(from),
                                           instruction.integer_signed,
                                           instruction.rounds_towards_zero,
                                           mode);
        return Completed();
    default:
        break;
    }
    return Halt(execution, StopReason::UndefinedInstruction);
}

// Moves between general and floating point registers. None of them is
// arithmetic, so none of them looks at FPSCR.
template<class Memory>
Stop ExecuteFloatMove(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    CpuState& state = *execution.state;
    std::uint32_t& core = state.registers.at(instruction.destination);
    const RegisterIndex floating = instruction.vfp_destination;

    switch (instruction.operation) {
    case Operation::VfpMoveToSingle:
        state.float_registers.at(floating) = core;
        return Completed();
    case Operation::VfpMoveFromSingle:
        core = state.float_registers.at(floating);
        return Completed();
    case Operation::VfpMoveToHalf:
        state.float_registers.at((floating * 2) + instruction.immediate) = core;
        return Completed();
    case Operation::VfpMoveFromHalf:
        core = state.float_registers.at((floating * 2) + instruction.immediate);
        return Completed();
    default:
        break;
    }

    // A pair is two consecutive singles, or the two halves of a double, which
    // are the same two words of the register file.
    const std::size_t low = instruction.vfp_destination_double ? floating * 2U : floating;
    std::uint32_t& high_core = state.registers.at(instruction.destination_high);
    if (instruction.operation == Operation::VfpMoveToPair) {
        state.float_registers.at(low) = core;
        state.float_registers.at(low + 1) = high_core;
        return Completed();
    }
    if (instruction.operation == Operation::VfpMoveFromPair) {
        core = state.float_registers.at(low);
        high_core = state.float_registers.at(low + 1);
        return Completed();
    }
    return Halt(execution, StopReason::UndefinedInstruction);
}

// VMRS and VMSR. User mode reaches FPSCR and nothing else, and reading it into
// r15 copies its flags into the CPSR's.
template<class Memory>
Stop ExecuteFloatSystem(Execution<Memory>& execution) {
    namespace p = primitives;
    const Instruction& instruction = execution.instruction;
    CpuState& state = *execution.state;
    if (instruction.immediate != vfp::Fpscr) {
        return Halt(execution, StopReason::Privileged);
    }

    if (instruction.operation == Operation::VfpMoveFromSystem) {
        if (instruction.updates_flags) {
            state.negative = ((state.fpscr >> p::fpscr::NegativeBit) & 1U) != 0;
            state.zero = ((state.fpscr >> p::fpscr::ZeroBit) & 1U) != 0;
            state.carry = ((state.fpscr >> p::fpscr::CarryBit) & 1U) != 0;
            state.overflow = ((state.fpscr >> p::fpscr::OverflowBit) & 1U) != 0;
        } else {
            state.registers.at(instruction.destination) = state.fpscr;
        }
        return Completed();
    }

    const std::uint32_t written =
        state.registers.at(instruction.destination) & vfp::ImplementedFpscrBits;
    if (!p::IsSupportedFloatMode(written)) {
        return Halt(execution, StopReason::Unsupported);
    }
    state.fpscr = written;
    return Completed();
}

// Where a floating point transfer starts and what its base holds afterwards.
// VLDR and VSTR add an offset and never write back; VLDM and VSTM increment
// after or decrement before, by the size of the whole block.
template<class Memory>
Addressing FloatTransferAddressing(const Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const std::uint32_t base = Read(execution, instruction.first);
    const std::uint32_t moved =
        instruction.add_offset ? base + instruction.immediate : base - instruction.immediate;

    const bool single_transfer =
        instruction.operation == Operation::VfpLoad || instruction.operation == Operation::VfpStore;
    if (single_transfer) {
        return {.address = moved, .written_back = base};
    }
    return {.address = instruction.preindexed ? moved : base, .written_back = moved};
}

// Single registers are one word each and doubles two, so a transfer moves a run
// of consecutive words of the register file either way.
struct FloatWords {
    std::size_t first = 0;
    std::size_t count = 0;
};

inline FloatWords WordsOf(const Instruction& instruction) {
    const bool single_transfer =
        instruction.operation == Operation::VfpLoad || instruction.operation == Operation::VfpStore;
    const std::size_t registers = single_transfer ? 1 : instruction.vfp_count;
    if (instruction.vfp_destination_double) {
        return {.first = static_cast<std::size_t>(instruction.vfp_destination) * 2,
                .count = registers * 2};
    }
    return {.first = instruction.vfp_destination, .count = registers};
}

// All the words are read before any register is written, so that a fault
// leaves the registers alone.
template<class Memory>
Stop LoadFloatWords(Execution<Memory>& execution, std::uint32_t start, FloatWords words) {
    std::array<std::uint32_t, SingleRegisterCount> loaded{};
    for (std::size_t index = 0; index < words.count; ++index) {
        const auto address = static_cast<std::uint32_t>(start + (index * WordSize));
        const std::optional<std::uint32_t> value = execution.memory->Read32(address);
        if (!value.has_value()) {
            return Halt(StopReason::ReadFault, address);
        }
        loaded.at(index) = value.value();
    }
    for (std::size_t index = 0; index < words.count; ++index) {
        execution.state->float_registers.at(words.first + index) = loaded.at(index);
    }
    return Completed();
}

template<class Memory>
Stop StoreFloatWords(Execution<Memory>& execution, std::uint32_t start, FloatWords words) {
    for (std::size_t index = 0; index < words.count; ++index) {
        const auto address = static_cast<std::uint32_t>(start + (index * WordSize));
        if (!execution.memory->Write32(address,
                                       execution.state->float_registers.at(words.first + index))) {
            return Halt(StopReason::WriteFault, address);
        }
    }
    return Completed();
}

// VLDR, VSTR, VLDM and VSTM. Every floating point transfer is word aligned, a
// double being two words with the low one first.
template<class Memory>
Stop ExecuteFloatTransfer(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const Addressing addressing = FloatTransferAddressing(execution);
    if (!IsAligned(addressing.address, WordSize)) {
        return Halt(StopReason::AlignmentFault, addressing.address);
    }

    const bool load = instruction.operation == Operation::VfpLoad ||
                      instruction.operation == Operation::VfpLoadMultiple;
    const Stop stop = load ? LoadFloatWords(execution, addressing.address, WordsOf(instruction))
                           : StoreFloatWords(execution, addressing.address, WordsOf(instruction));
    if (stop.Stopped()) {
        return stop;
    }
    if (instruction.writes_back) {
        execution.state->registers.at(instruction.first) = addressing.written_back;
    }
    return Completed();
}

template<class Memory>
Stop ExecuteFloatingPoint(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    switch (instruction.operation) {
    case Operation::VfpMoveToSingle:
    case Operation::VfpMoveFromSingle:
    case Operation::VfpMoveToHalf:
    case Operation::VfpMoveFromHalf:
    case Operation::VfpMoveToPair:
    case Operation::VfpMoveFromPair:
        return ExecuteFloatMove(execution);
    case Operation::VfpMoveToSystem:
    case Operation::VfpMoveFromSystem:
        return ExecuteFloatSystem(execution);
    case Operation::VfpLoad:
    case Operation::VfpStore:
    case Operation::VfpLoadMultiple:
    case Operation::VfpStoreMultiple:
        return ExecuteFloatTransfer(execution);
    default:
        break;
    }

    // Everything from here computes, and needs a mode the primitives implement.
    if (!primitives::IsSupportedFloatMode(execution.state->fpscr)) {
        return Halt(execution, StopReason::Unsupported);
    }

    switch (instruction.operation) {
    case Operation::VfpConvertPrecision:
    case Operation::VfpConvertFromInteger:
    case Operation::VfpConvertToInteger:
        return ExecuteFloatConversion(execution);
    default:
        break;
    }
    return instruction.vfp_sources_double ? ExecuteFloatArithmetic<double>(execution)
                                          : ExecuteFloatArithmetic<float>(execution);
}

}  // namespace hexlith::domain::arm::interpreter

#endif  // HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTE_VFP_H
