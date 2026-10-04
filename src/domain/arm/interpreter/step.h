#ifndef HEXLITH_DOMAIN_ARM_INTERPRETER_STEP_H
#define HEXLITH_DOMAIN_ARM_INTERPRETER_STEP_H

#include "domain/arm/decode_arm.h"
#include "domain/arm/decode_thumb.h"
#include "domain/arm/interpreter/execute_data.h"
#include "domain/arm/interpreter/execute_system.h"
#include "domain/arm/interpreter/execute_transfer.h"
#include "domain/arm/interpreter/execution.h"
#include "domain/arm/interpreter/interpreter.h"
#include "domain/arm/primitives/integer.h"

#include <cstdint>
#include <optional>
#include <variant>

// The definitions of Step and Run. A translation unit that instantiates the
// interpreter for a memory includes this; everything else includes
// interpreter.h.
namespace hexlith::domain::arm {
namespace interpreter {

// The instruction at r15, or the address that could not be fetched.
//
// A Thumb branch with link is two halfwords, and the second is only read when
// the first one says it is needed, so that a function ending at the last
// halfword of a page does not fault on the page after it.
template<class Memory>
std::variant<Instruction, Stop> Fetch(const CpuState& state, const Memory& memory) {
    const std::uint32_t address = state.registers.at(ProgramCounter);

    if (!state.thumb) {
        const std::optional<std::uint32_t> word = memory.Read32(address);
        if (!word.has_value()) {
            return Halt(StopReason::FetchFault, address);
        }
        return DecodeArm(word.value());
    }

    const std::optional<std::uint16_t> first = memory.Read16(address);
    if (!first.has_value()) {
        return Halt(StopReason::FetchFault, address);
    }

    // The first halfword of a branch with link is 11110 in its top five bits.
    constexpr std::uint16_t LinkPrefix = 0b11110;
    std::uint16_t second = 0;
    if ((first.value() >> 11U) == LinkPrefix) {
        const std::optional<std::uint16_t> next = memory.Read16(address + 2);
        if (!next.has_value()) {
            return Halt(StopReason::FetchFault, address + 2);
        }
        second = next.value();
    }
    return DecodeThumb(first.value(), second);
}

template<class Memory>
Stop Dispatch(Execution<Memory>& execution) {
    switch (execution.instruction.operation) {
    case Operation::And:
    case Operation::Eor:
    case Operation::Sub:
    case Operation::Rsb:
    case Operation::Add:
    case Operation::Adc:
    case Operation::Sbc:
    case Operation::Rsc:
    case Operation::Tst:
    case Operation::Teq:
    case Operation::Cmp:
    case Operation::Cmn:
    case Operation::Orr:
    case Operation::Mov:
    case Operation::Bic:
    case Operation::Mvn:
        return ExecuteDataProcessing(execution);
    case Operation::Mul:
    case Operation::Mla:
        return ExecuteMultiply(execution);
    case Operation::Umull:
    case Operation::Umlal:
    case Operation::Umaal:
    case Operation::Smull:
    case Operation::Smlal:
        return ExecuteLongMultiply(execution);
    case Operation::SmulHalf:
    case Operation::SmlaHalf:
    case Operation::SmlalHalf:
    case Operation::SmulwHalf:
    case Operation::SmlawHalf:
        return ExecuteHalfwordMultiply(execution);
    case Operation::Smuad:
    case Operation::Smusd:
    case Operation::Smlad:
    case Operation::Smlsd:
    case Operation::Smlald:
    case Operation::Smlsld:
    case Operation::Smmul:
    case Operation::Smmla:
    case Operation::Smmls:
        return ExecuteSignedMultiply(execution);
    case Operation::Qadd:
    case Operation::Qsub:
    case Operation::QdoubleAdd:
    case Operation::QdoubleSub:
        return ExecuteSaturatingArithmetic(execution);
    case Operation::Ssat:
    case Operation::Ssat16:
    case Operation::Usat:
    case Operation::Usat16:
        return ExecuteSaturate(execution);
    case Operation::Sxtab:
    case Operation::Sxtab16:
    case Operation::Sxtah:
    case Operation::Sxtb:
    case Operation::Sxtb16:
    case Operation::Sxth:
    case Operation::Uxtab:
    case Operation::Uxtab16:
    case Operation::Uxtah:
    case Operation::Uxtb:
    case Operation::Uxtb16:
    case Operation::Uxth:
        return ExecuteExtension(execution);
    case Operation::ParallelArithmetic:
    case Operation::Pkhbt:
    case Operation::Pkhtb:
    case Operation::Rev:
    case Operation::Rev16:
    case Operation::Revsh:
    case Operation::Sel:
    case Operation::Usad8:
    case Operation::Usada8:
    case Operation::CountLeadingZeros:
        return ExecuteMedia(execution);
    case Operation::MoveFromStatus:
    case Operation::MoveToStatus:
        return ExecuteStatusRegister(execution);
    case Operation::Branch:
    case Operation::BranchLink:
    case Operation::BranchExchange:
    case Operation::BranchLinkExchangeRegister:
    case Operation::BranchLinkExchangeImmediate:
        return ExecuteBranch(execution);
    case Operation::LoadWord:
    case Operation::LoadByte:
    case Operation::StoreWord:
    case Operation::StoreByte:
    case Operation::LoadWordUnprivileged:
    case Operation::LoadByteUnprivileged:
    case Operation::StoreWordUnprivileged:
    case Operation::StoreByteUnprivileged:
    case Operation::LoadHalf:
    case Operation::StoreHalf:
    case Operation::LoadSignedByte:
    case Operation::LoadSignedHalf:
    case Operation::LoadDouble:
    case Operation::StoreDouble:
    case Operation::LoadMultiple:
    case Operation::StoreMultiple:
    case Operation::Swap:
    case Operation::SwapByte:
    case Operation::LoadExclusive:
    case Operation::StoreExclusive:
    case Operation::LoadExclusiveByte:
    case Operation::StoreExclusiveByte:
    case Operation::LoadExclusiveHalf:
    case Operation::StoreExclusiveHalf:
    case Operation::LoadExclusiveDouble:
    case Operation::StoreExclusiveDouble:
    case Operation::ClearExclusive:
        return ExecuteTransfer(execution);
    case Operation::Undefined:
        return Halt(execution, StopReason::UndefinedInstruction);
    default:
        break;
    }

    if (IsFloatingPoint(execution.instruction.operation)) {
        return Halt(execution, StopReason::Unsupported);
    }
    return ExecuteSystem(execution);
}

}  // namespace interpreter

template<memory::GuestMemory Memory>
Stop Step(CpuState& state, Memory& memory) {
    using namespace interpreter;

    const std::variant<Instruction, Stop> fetched = Fetch(state, memory);
    if (const Stop* fault = std::get_if<Stop>(&fetched)) {
        return *fault;
    }

    const auto& instruction = std::get<Instruction>(fetched);
    const std::uint32_t address = state.registers.at(ProgramCounter);
    Execution<Memory> execution{
        .state = &state, .memory = &memory, .instruction = instruction, .address = address};

    // An undefined word stops whatever its condition says, rather than being
    // skipped when the condition fails. The architecture lets an implementation
    // do either, and stopping is the one that cannot hide data being executed.
    const bool passes = instruction.operation != Operation::Undefined &&
                        primitives::ConditionPassed(instruction.condition, Flags(execution));
    if (instruction.operation != Operation::Undefined && !passes) {
        state.registers.at(ProgramCounter) = execution.Next();
        return Completed();
    }

    const Stop stop = Dispatch(execution);
    if (stop.Stopped()) {
        return stop;
    }
    if (!execution.branched) {
        state.registers.at(ProgramCounter) = execution.Next();
    }
    return Completed();
}

template<memory::GuestMemory Memory>
Stop Run(CpuState& state, Memory& memory, std::uint64_t limit) {
    for (std::uint64_t executed = 0; executed < limit; ++executed) {
        const Stop stop = Step(state, memory);
        if (stop.Stopped()) {
            return stop;
        }
    }
    return Stop{.reason = StopReason::StepLimit, .detail = 0};
}

}  // namespace hexlith::domain::arm

#endif  // HEXLITH_DOMAIN_ARM_INTERPRETER_STEP_H
