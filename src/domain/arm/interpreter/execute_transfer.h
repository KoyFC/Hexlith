#ifndef HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTE_TRANSFER_H
#define HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTE_TRANSFER_H

#include "domain/arm/instruction.h"
#include "domain/arm/interpreter/execute_data.h"
#include "domain/arm/interpreter/execution.h"
#include "domain/arm/primitives/integer.h"
#include "domain/arm/primitives/packing.h"
#include "domain/arm/primitives/transfer.h"
#include "domain/arm/primitives/unpredictable.h"

#include <array>
#include <bit>
#include <cstdint>
#include <optional>

// Loads and stores: single, halfword, doubleword, block, swap and exclusive.
//
// Alignment follows ARMv6 with unaligned access support enabled, the U bit of
// the system control register, which is the configuration the ARM11 runs the
// game in as far as anything here has observed: a word or halfword load or
// store may be unaligned, and a block, doubleword, swap or exclusive transfer
// that is not word aligned faults. Source: the ARMv6 reference manual (DDI
// 0100I), section A2.8, "Unaligned access support". docs/arm-interpreter.md
// records what this has and has not been checked against.
//
// A load that faults leaves every register as it was. A store multiple that
// faults part of the way through has already written the words before the
// fault, which is what the hardware does too; its registers are left as they
// were.
//
// Internal to the interpreter.
namespace hexlith::domain::arm::interpreter {

using primitives::Addressing;
using primitives::IsAligned;
using primitives::StoreValue;
using primitives::Width;
using primitives::WordSize;

// The base of a transfer as the instruction reads it. A literal load in Thumb
// reads r15 rounded down to a word.
template<class Memory>
std::uint32_t TransferBase(const Execution<Memory>& execution) {
    return ReadAligned(execution, execution.instruction.first);
}

// The offset of a single transfer: an immediate, a register, or a register
// shifted by an immediate. The carry the shifter produces goes nowhere.
template<class Memory>
std::uint32_t TransferOffset(const Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    switch (instruction.operand) {
    case OperandForm::Immediate:
        return instruction.immediate;
    case OperandForm::Register:
        return Read(execution, instruction.second);
    case OperandForm::ShiftedRegister:
        return primitives::Shift(instruction.shift,
                                 Read(execution, instruction.second),
                                 instruction.shift_amount,
                                 execution.state->carry)
            .value;
    case OperandForm::None:
        break;
    }
    return 0;
}

template<class Memory>
Addressing SingleAddressing(const Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const std::uint32_t base = TransferBase(execution);
    const std::uint32_t offset = TransferOffset(execution);
    return primitives::SingleAddress(base, offset, instruction.add_offset, instruction.preindexed);
}

template<class Memory>
void WriteBack(Execution<Memory>& execution, std::uint32_t value) {
    if (execution.instruction.writes_back) {
        execution.state->registers.at(execution.instruction.first) = value;
    }
}

// LDR, LDRB, LDRH, LDRSB, LDRSH and the unprivileged forms, which in user mode
// are the same thing. A word loaded into r15 is a branch that may change the
// instruction set, as BX would.
template<class Memory>
Stop ExecuteLoad(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const Addressing addressing = SingleAddressing(execution);
    const Width width = primitives::WidthOf(instruction.operation);
    if (instruction.destination == ProgramCounter && !IsAligned(addressing.address, WordSize)) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const std::optional<std::uint32_t> value =
        primitives::LoadValue(*execution.memory, addressing.address, width);
    if (!value.has_value()) {
        return Halt(StopReason::ReadFault, addressing.address);
    }

    if (instruction.destination == ProgramCounter) {
        const Stop stop = ExchangeTo(execution, value.value());
        if (stop.Stopped()) {
            return stop;
        }
        WriteBack(execution, addressing.written_back);
        return Completed();
    }

    WriteBack(execution, addressing.written_back);
    execution.state->registers.at(instruction.destination) = value.value();
    return Completed();
}

// STR, STRB, STRH and the unprivileged forms. A store of r15 stores the
// instruction's address plus eight, which is what the ARM11 does with the
// choice the architecture leaves it.
template<class Memory>
Stop ExecuteStore(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const Addressing addressing = SingleAddressing(execution);
    const std::uint32_t value = Read(execution, instruction.destination);
    if (!StoreValue(*execution.memory,
                    addressing.address,
                    value,
                    primitives::WidthOf(instruction.operation))) {
        return Halt(StopReason::WriteFault, addressing.address);
    }
    WriteBack(execution, addressing.written_back);
    return Completed();
}

// LDRD and STRD: an even register and the one above it, at a word-aligned
// address.
template<class Memory>
Stop ExecuteDoubleTransfer(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const RegisterIndex low = instruction.destination;
    const auto high = static_cast<RegisterIndex>(low + 1);
    const bool load = instruction.operation == Operation::LoadDouble;

    const Addressing addressing = SingleAddressing(execution);
    if (!IsAligned(addressing.address, WordSize)) {
        return Halt(StopReason::AlignmentFault, addressing.address);
    }

    if (load) {
        const std::optional<std::uint32_t> first = execution.memory->Read32(addressing.address);
        const std::optional<std::uint32_t> second =
            execution.memory->Read32(addressing.address + WordSize);
        if (!first.has_value() || !second.has_value()) {
            return Halt(StopReason::ReadFault,
                        first.has_value() ? addressing.address + WordSize : addressing.address);
        }
        WriteBack(execution, addressing.written_back);
        execution.state->registers.at(low) = first.value();
        execution.state->registers.at(high) = second.value();
        return Completed();
    }

    if (!execution.memory->Write32(addressing.address, Read(execution, low))) {
        return Halt(StopReason::WriteFault, addressing.address);
    }
    if (!execution.memory->Write32(addressing.address + WordSize, Read(execution, high))) {
        return Halt(StopReason::WriteFault, addressing.address + WordSize);
    }
    WriteBack(execution, addressing.written_back);
    return Completed();
}

// LDM, and POP in Thumb. All the words are read before any register is
// written, so that a fault leaves the registers alone.
template<class Memory>
Stop ExecuteLoadMultiple(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const RegisterSet registers = instruction.registers;
    const RegisterIndex base_register = instruction.first;
    const bool base_listed = Holds(registers, base_register);
    const std::uint32_t base = Read(execution, base_register);
    const auto bytes = static_cast<std::uint32_t>(std::popcount(registers)) * WordSize;
    const std::uint32_t start =
        primitives::BlockStart(instruction.add_offset, instruction.preindexed, base, bytes);
    if (!IsAligned(start, WordSize)) {
        return Halt(StopReason::AlignmentFault, start);
    }

    std::array<std::uint32_t, RegisterCount> loaded{};
    std::uint32_t address = start;
    for (RegisterIndex index = 0; index < RegisterCount; ++index) {
        if (!Holds(registers, index)) {
            continue;
        }
        const std::optional<std::uint32_t> value = execution.memory->Read32(address);
        if (!value.has_value()) {
            return Halt(StopReason::ReadFault, address);
        }
        loaded.at(index) = value.value();
        address += WordSize;
    }

    if (Holds(registers, ProgramCounter)) {
        const Stop stop = ExchangeTo(execution, loaded.at(ProgramCounter));
        if (stop.Stopped()) {
            return stop;
        }
    }
    if (instruction.writes_back && !base_listed) {
        execution.state->registers.at(base_register) =
            primitives::BlockWrittenBack(instruction.add_offset, base, bytes);
    }
    for (RegisterIndex index = 0; index < ProgramCounter; ++index) {
        if (Holds(registers, index)) {
            execution.state->registers.at(index) = loaded.at(index);
        }
    }
    return Completed();
}

// STM, and PUSH in Thumb. Writing back into a register the transfer also stores
// is defined when it is the lowest one, which stores the original value, and
// unpredictable otherwise. A base of r15 is unpredictable either way.
template<class Memory>
Stop ExecuteStoreMultiple(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const RegisterSet registers = instruction.registers;
    const RegisterIndex base_register = instruction.first;

    const std::uint32_t base = Read(execution, base_register);
    const auto bytes = static_cast<std::uint32_t>(std::popcount(registers)) * WordSize;
    const std::uint32_t start =
        primitives::BlockStart(instruction.add_offset, instruction.preindexed, base, bytes);
    if (!IsAligned(start, WordSize)) {
        return Halt(StopReason::AlignmentFault, start);
    }

    std::uint32_t address = start;
    for (RegisterIndex index = 0; index < RegisterCount; ++index) {
        if (!Holds(registers, index)) {
            continue;
        }
        if (!execution.memory->Write32(address, Read(execution, index))) {
            return Halt(StopReason::WriteFault, address);
        }
        address += WordSize;
    }

    if (instruction.writes_back) {
        execution.state->registers.at(base_register) =
            primitives::BlockWrittenBack(instruction.add_offset, base, bytes);
    }
    return Completed();
}

// SWP and SWPB: a load and a store to one address with nothing in between.
template<class Memory>
Stop ExecuteSwap(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const RegisterIndex base = instruction.first;
    const bool byte = instruction.operation == Operation::SwapByte;
    const Width width = byte ? Width::Byte : Width::Word;
    const std::uint32_t address = Read(execution, base);
    if (!byte && !IsAligned(address, WordSize)) {
        return Halt(StopReason::AlignmentFault, address);
    }

    const std::optional<std::uint32_t> old =
        primitives::LoadValue(*execution.memory, address, width);
    if (!old.has_value()) {
        return Halt(StopReason::ReadFault, address);
    }
    if (!StoreValue(*execution.memory, address, Read(execution, instruction.second), width)) {
        return Halt(StopReason::WriteFault, address);
    }
    execution.state->registers.at(instruction.destination) = old.value();
    return Completed();
}

// LDREX and its byte, halfword and doubleword forms. The load opens the local
// monitor on its address.
template<class Memory>
Stop ExecuteLoadExclusive(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const bool pair = instruction.operation == Operation::LoadExclusiveDouble;
    const RegisterIndex low = instruction.destination;

    const std::uint32_t size = primitives::ExclusiveSize(instruction.operation);
    const std::uint32_t address = Read(execution, instruction.first);
    if (!IsAligned(address, size)) {
        return Halt(StopReason::AlignmentFault, address);
    }

    if (pair) {
        const std::optional<std::uint64_t> value = execution.memory->Read64(address);
        if (!value.has_value()) {
            return Halt(StopReason::ReadFault, address);
        }
        execution.state->registers.at(low) = static_cast<std::uint32_t>(value.value());
        execution.state->registers.at(low + 1) = static_cast<std::uint32_t>(value.value() >> 32U);
    } else {
        const Width width = primitives::WidthOfSize(size);
        const std::optional<std::uint32_t> value =
            primitives::LoadValue(*execution.memory, address, width);
        if (!value.has_value()) {
            return Halt(StopReason::ReadFault, address);
        }
        execution.state->registers.at(low) = value.value();
    }

    execution.state->exclusive_open = true;
    execution.state->exclusive_address = address;
    return Completed();
}

// STREX and its forms. The store happens only while the monitor is open on this
// address, the status register says whether it did, and the monitor closes
// either way.
template<class Memory>
Stop ExecuteStoreExclusive(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const bool pair = instruction.operation == Operation::StoreExclusiveDouble;
    const RegisterIndex status = instruction.destination;
    const RegisterIndex value = instruction.second;
    const RegisterIndex base = instruction.first;
    const std::uint32_t size = primitives::ExclusiveSize(instruction.operation);
    const std::uint32_t address = Read(execution, base);
    if (!IsAligned(address, size)) {
        return Halt(StopReason::AlignmentFault, address);
    }

    const bool passes =
        execution.state->exclusive_open && execution.state->exclusive_address == address;
    if (passes) {
        bool written = false;
        if (pair) {
            const std::uint64_t doubleword =
                (static_cast<std::uint64_t>(Read(execution, value + 1)) << 32U) |
                Read(execution, value);
            written = execution.memory->Write64(address, doubleword);
        } else {
            const Width width = primitives::WidthOfSize(size);
            written = StoreValue(*execution.memory, address, Read(execution, value), width);
        }
        if (!written) {
            return Halt(StopReason::WriteFault, address);
        }
    }

    execution.state->registers.at(status) = passes ? 0 : 1;
    execution.state->exclusive_open = false;
    return Completed();
}

template<class Memory>
Stop ExecuteTransfer(Execution<Memory>& execution) {
    switch (execution.instruction.operation) {
    case Operation::LoadWord:
    case Operation::LoadByte:
    case Operation::LoadWordUnprivileged:
    case Operation::LoadByteUnprivileged:
    case Operation::LoadHalf:
    case Operation::LoadSignedByte:
    case Operation::LoadSignedHalf:
        return ExecuteLoad(execution);
    case Operation::StoreWord:
    case Operation::StoreByte:
    case Operation::StoreWordUnprivileged:
    case Operation::StoreByteUnprivileged:
    case Operation::StoreHalf:
        return ExecuteStore(execution);
    case Operation::LoadDouble:
    case Operation::StoreDouble:
        return ExecuteDoubleTransfer(execution);
    case Operation::LoadMultiple:
        return ExecuteLoadMultiple(execution);
    case Operation::StoreMultiple:
        return ExecuteStoreMultiple(execution);
    case Operation::Swap:
    case Operation::SwapByte:
        return ExecuteSwap(execution);
    case Operation::LoadExclusive:
    case Operation::LoadExclusiveByte:
    case Operation::LoadExclusiveHalf:
    case Operation::LoadExclusiveDouble:
        return ExecuteLoadExclusive(execution);
    case Operation::StoreExclusive:
    case Operation::StoreExclusiveByte:
    case Operation::StoreExclusiveHalf:
    case Operation::StoreExclusiveDouble:
        return ExecuteStoreExclusive(execution);
    case Operation::ClearExclusive:
        execution.state->exclusive_open = false;
        return Completed();
    default:
        break;
    }
    return Halt(execution, StopReason::UndefinedInstruction);
}

}  // namespace hexlith::domain::arm::interpreter

#endif  // HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTE_TRANSFER_H
