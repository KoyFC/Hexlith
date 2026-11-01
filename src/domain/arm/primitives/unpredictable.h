#ifndef HEXLITH_DOMAIN_ARM_PRIMITIVES_UNPREDICTABLE_H
#define HEXLITH_DOMAIN_ARM_PRIMITIVES_UNPREDICTABLE_H

#include "domain/arm/instruction.h"
#include "domain/arm/operation.h"
#include "domain/arm/registers.h"

#include <algorithm>
#include <bit>
#include <initializer_list>

// The combinations of registers and bits that the ARMv6 reference manual calls
// unpredictable, decided from the decoded instruction alone.
//
// The interpreter stops on these rather than guessing. The lifter has to stop
// on exactly the same ones, so the rule is written once, here. The checks that
// depend on a value, such as an address that is not aligned, cannot be decided
// from the instruction and stay with whoever computes the value.
namespace hexlith::domain::arm::primitives {

[[nodiscard]] inline bool AnyIsProgramCounter(std::initializer_list<RegisterIndex> registers) {
    return std::ranges::any_of(registers,
                               [](RegisterIndex index) { return index == ProgramCounter; });
}

// A single transfer's offset register of r15, writing back into r15, and
// writing back into the register the transfer loads or stores.
[[nodiscard]] inline bool UnpredictableSingleTransfer(const Instruction& instruction) {
    if (instruction.operand != OperandForm::Immediate && instruction.second == ProgramCounter) {
        return true;
    }
    if (!instruction.writes_back) {
        return false;
    }
    return instruction.first == ProgramCounter || instruction.first == instruction.destination;
}

[[nodiscard]] inline bool UnpredictableDoubleTransfer(const Instruction& instruction) {
    const RegisterIndex low = instruction.destination;
    const auto high = static_cast<RegisterIndex>(low + 1);
    const bool load = instruction.operation == Operation::LoadDouble;
    const bool register_offset = instruction.operand == OperandForm::Register;
    return high == ProgramCounter ||
           (register_offset &&
            (instruction.second == ProgramCounter ||
             (load && (instruction.second == low || instruction.second == high)))) ||
           (instruction.writes_back && (instruction.first == ProgramCounter ||
                                        instruction.first == low || instruction.first == high));
}

// The S bit loads user mode registers or restores the saved status, neither of
// which means anything from user mode. A base of r15 is unpredictable with or
// without write-back. In ARM, writing back into a register the transfer also
// loads is unpredictable; Thumb defines it as no write-back, and the loaded
// value wins.
[[nodiscard]] inline bool UnpredictableLoadMultiple(const Instruction& instruction) {
    const bool base_listed = Holds(instruction.registers, instruction.first);
    const bool thumb = instruction.instruction_set == InstructionSet::Thumb;
    return instruction.saved_status || instruction.first == ProgramCounter ||
           (instruction.writes_back && base_listed && !thumb);
}

// Writing back into a register the transfer also stores is defined when it is
// the lowest one, which stores the original value, and unpredictable otherwise.
[[nodiscard]] inline bool UnpredictableStoreMultiple(const Instruction& instruction) {
    const RegisterIndex base = instruction.first;
    const auto below_base = static_cast<RegisterSet>((1U << base) - 1U);
    const bool base_not_lowest =
        Holds(instruction.registers, base) && (instruction.registers & below_base) != 0;
    return instruction.saved_status || base == ProgramCounter ||
           (instruction.writes_back && base_not_lowest);
}

[[nodiscard]] inline bool UnpredictableSwap(const Instruction& instruction) {
    const RegisterIndex base = instruction.first;
    return AnyIsProgramCounter({base, instruction.destination, instruction.second}) ||
           base == instruction.destination || base == instruction.second;
}

[[nodiscard]] inline bool UnpredictableLoadExclusive(const Instruction& instruction) {
    const bool pair = instruction.operation == Operation::LoadExclusiveDouble;
    const RegisterIndex low = instruction.destination;
    return AnyIsProgramCounter({instruction.first, low}) || (pair && low + 1 == ProgramCounter);
}

[[nodiscard]] inline bool UnpredictableStoreExclusive(const Instruction& instruction) {
    const bool pair = instruction.operation == Operation::StoreExclusiveDouble;
    const RegisterIndex status = instruction.destination;
    const RegisterIndex value = instruction.second;
    const RegisterIndex base = instruction.first;
    return AnyIsProgramCounter({status, value, base}) || (pair && value + 1 == ProgramCounter) ||
           status == base || status == value || (pair && status == value + 1);
}

// Whether the instruction is unpredictable whatever the values in its
// registers are. False for an operation this does not classify, which is the
// floating point and system classes, whose checks live with their execution.
[[nodiscard]] inline bool HasUnpredictableOperands(const Instruction& instruction) {
    switch (instruction.operation) {
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
        // A shift by a register reads r15 as twelve bytes ahead on some cores and
        // eight on others. With the S bit, a write to r15 also copies the saved
        // status register into the CPSR, and user mode has none to copy.
        return (instruction.shift_register != NoRegister &&
                AnyIsProgramCounter({instruction.destination,
                                     instruction.first,
                                     instruction.second,
                                     instruction.shift_register})) ||
               (instruction.destination == ProgramCounter && instruction.updates_flags);
    case Operation::Mul:
    case Operation::Mla:
        return AnyIsProgramCounter(
            {instruction.destination, instruction.first, instruction.second, instruction.third});
    case Operation::Umull:
    case Operation::Umlal:
    case Operation::Umaal:
    case Operation::Smull:
    case Operation::Smlal:
        return AnyIsProgramCounter({instruction.destination,
                                    instruction.destination_high,
                                    instruction.second,
                                    instruction.third}) ||
               instruction.destination == instruction.destination_high;
    case Operation::SmulHalf:
    case Operation::SmlaHalf:
    case Operation::SmlalHalf:
    case Operation::SmulwHalf:
    case Operation::SmlawHalf:
        return AnyIsProgramCounter({instruction.destination,
                                    instruction.destination_high,
                                    instruction.first,
                                    instruction.second,
                                    instruction.third}) ||
               (instruction.operation == Operation::SmlalHalf &&
                instruction.destination == instruction.destination_high);
    case Operation::Smuad:
    case Operation::Smusd:
    case Operation::Smlad:
    case Operation::Smlsd:
    case Operation::Smlald:
    case Operation::Smlsld:
    case Operation::Smmul:
    case Operation::Smmla:
    case Operation::Smmls:
        return AnyIsProgramCounter({instruction.destination,
                                    instruction.destination_high,
                                    instruction.first,
                                    instruction.second,
                                    instruction.third}) ||
               ((instruction.operation == Operation::Smlald ||
                 instruction.operation == Operation::Smlsld) &&
                instruction.destination == instruction.destination_high);
    case Operation::Qadd:
    case Operation::Qsub:
    case Operation::QdoubleAdd:
    case Operation::QdoubleSub:
        return AnyIsProgramCounter(
            {instruction.destination, instruction.first, instruction.second});
    case Operation::Ssat:
    case Operation::Ssat16:
    case Operation::Usat:
    case Operation::Usat16:
        return AnyIsProgramCounter({instruction.destination, instruction.second});
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
        return AnyIsProgramCounter({instruction.destination, instruction.second}) ||
               instruction.first == ProgramCounter;
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
        return AnyIsProgramCounter(
            {instruction.destination, instruction.first, instruction.second, instruction.third});
    case Operation::MoveFromStatus:
        return instruction.saved_status || instruction.destination == ProgramCounter;
    case Operation::MoveToStatus:
        return instruction.saved_status || (instruction.operand != OperandForm::Immediate &&
                                            instruction.second == ProgramCounter);
    case Operation::BranchLinkExchangeRegister:
        return instruction.second == ProgramCounter;
    case Operation::LoadWord:
    case Operation::LoadByte:
    case Operation::LoadWordUnprivileged:
    case Operation::LoadByteUnprivileged:
    case Operation::LoadHalf:
    case Operation::LoadSignedByte:
    case Operation::LoadSignedHalf:
    case Operation::StoreWord:
    case Operation::StoreByte:
    case Operation::StoreWordUnprivileged:
    case Operation::StoreByteUnprivileged:
    case Operation::StoreHalf:
        return UnpredictableSingleTransfer(instruction);
    case Operation::LoadDouble:
    case Operation::StoreDouble:
        return UnpredictableDoubleTransfer(instruction);
    case Operation::LoadMultiple:
        return UnpredictableLoadMultiple(instruction);
    case Operation::StoreMultiple:
        return UnpredictableStoreMultiple(instruction);
    case Operation::Swap:
    case Operation::SwapByte:
        return UnpredictableSwap(instruction);
    case Operation::LoadExclusive:
    case Operation::LoadExclusiveByte:
    case Operation::LoadExclusiveHalf:
    case Operation::LoadExclusiveDouble:
        return UnpredictableLoadExclusive(instruction);
    case Operation::StoreExclusive:
    case Operation::StoreExclusiveByte:
    case Operation::StoreExclusiveHalf:
    case Operation::StoreExclusiveDouble:
        return UnpredictableStoreExclusive(instruction);
    default:
        break;
    }
    return false;
}

}  // namespace hexlith::domain::arm::primitives

#endif  // HEXLITH_DOMAIN_ARM_PRIMITIVES_UNPREDICTABLE_H
