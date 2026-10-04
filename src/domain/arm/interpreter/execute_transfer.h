#ifndef HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTE_TRANSFER_H
#define HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTE_TRANSFER_H

#include "domain/arm/instruction.h"
#include "domain/arm/interpreter/execute_data.h"
#include "domain/arm/interpreter/execution.h"
#include "domain/arm/primitives/integer.h"
#include "domain/arm/primitives/packing.h"

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

constexpr std::uint32_t WordSize = 4;

inline bool IsAligned(std::uint32_t address, std::uint32_t size) {
    return (address % size) == 0;
}

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

struct Addressing {
    // Where the transfer happens.
    std::uint32_t address = 0;

    // What the base register holds afterwards when the instruction writes back.
    std::uint32_t written_back = 0;
};

template<class Memory>
Addressing SingleAddressing(const Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const std::uint32_t base = TransferBase(execution);
    const std::uint32_t offset = TransferOffset(execution);
    const std::uint32_t offset_address = instruction.add_offset ? base + offset : base - offset;
    return {.address = instruction.preindexed ? offset_address : base,
            .written_back = offset_address};
}

// What the ARMv6 reference manual calls unpredictable about a single transfer's
// registers: an offset register of r15, writing back into r15, and writing back
// into the register the transfer loads or stores.
template<class Memory>
bool UnpredictableSingle(const Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    if (instruction.operand != OperandForm::Immediate && instruction.second == ProgramCounter) {
        return true;
    }
    if (!instruction.writes_back) {
        return false;
    }
    return instruction.first == ProgramCounter || instruction.first == instruction.destination;
}

enum class Width : std::uint8_t { Byte, SignedByte, Half, SignedHalf, Word };

inline Width WidthOf(Operation operation) {
    switch (operation) {
    case Operation::LoadByte:
    case Operation::StoreByte:
    case Operation::LoadByteUnprivileged:
    case Operation::StoreByteUnprivileged:
        return Width::Byte;
    case Operation::LoadSignedByte:
        return Width::SignedByte;
    case Operation::LoadHalf:
    case Operation::StoreHalf:
        return Width::Half;
    case Operation::LoadSignedHalf:
        return Width::SignedHalf;
    default:
        return Width::Word;
    }
}

template<class Memory>
std::optional<std::uint32_t> Load(const Memory& memory, std::uint32_t address, Width width) {
    switch (width) {
    case Width::Byte:
    case Width::SignedByte: {
        const std::optional<std::uint8_t> value = memory.Read8(address);
        if (!value.has_value()) {
            return std::nullopt;
        }
        return width == Width::Byte ? value.value() : primitives::SignExtendByte(value.value());
    }
    case Width::Half:
    case Width::SignedHalf: {
        const std::optional<std::uint16_t> value = memory.Read16(address);
        if (!value.has_value()) {
            return std::nullopt;
        }
        return width == Width::Half ? value.value() : primitives::SignExtendHalf(value.value());
    }
    case Width::Word:
        return memory.Read32(address);
    }
    return std::nullopt;
}

template<class Memory>
bool StoreValue(Memory& memory, std::uint32_t address, std::uint32_t value, Width width) {
    switch (width) {
    case Width::Byte:
    case Width::SignedByte:
        return memory.Write8(address, static_cast<std::uint8_t>(value));
    case Width::Half:
    case Width::SignedHalf:
        return memory.Write16(address, static_cast<std::uint16_t>(value));
    case Width::Word:
        return memory.Write32(address, value);
    }
    return false;
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
    if (UnpredictableSingle(execution)) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const Addressing addressing = SingleAddressing(execution);
    const Width width = WidthOf(instruction.operation);
    if (instruction.destination == ProgramCounter && !IsAligned(addressing.address, WordSize)) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const std::optional<std::uint32_t> value = Load(*execution.memory, addressing.address, width);
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
    if (UnpredictableSingle(execution)) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const Addressing addressing = SingleAddressing(execution);
    const std::uint32_t value = Read(execution, instruction.destination);
    if (!StoreValue(*execution.memory, addressing.address, value, WidthOf(instruction.operation))) {
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

    const bool register_offset = instruction.operand == OperandForm::Register;
    if (high == ProgramCounter ||
        (register_offset &&
         (instruction.second == ProgramCounter ||
          (load && (instruction.second == low || instruction.second == high)))) ||
        (instruction.writes_back && (instruction.first == ProgramCounter ||
                                     instruction.first == low || instruction.first == high))) {
        return Halt(execution, StopReason::Unpredictable);
    }

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

// The lowest address a block transfer touches, from its four addressing modes:
// increment after and before, decrement after and before.
template<class Memory>
std::uint32_t
BlockStart(const Execution<Memory>& execution, std::uint32_t base, std::uint32_t bytes) {
    const Instruction& instruction = execution.instruction;
    if (instruction.add_offset) {
        return instruction.preindexed ? base + WordSize : base;
    }
    return instruction.preindexed ? base - bytes : base - bytes + WordSize;
}

template<class Memory>
std::uint32_t
BlockWrittenBack(const Execution<Memory>& execution, std::uint32_t base, std::uint32_t bytes) {
    return execution.instruction.add_offset ? base + bytes : base - bytes;
}

// LDM, and POP in Thumb. All the words are read before any register is
// written, so that a fault leaves the registers alone.
template<class Memory>
Stop ExecuteLoadMultiple(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const RegisterSet registers = instruction.registers;
    const RegisterIndex base_register = instruction.first;
    const bool base_listed = Holds(registers, base_register);

    // The S bit loads user mode registers or restores the saved status, neither
    // of which means anything from user mode. A base of r15 is unpredictable
    // with or without write-back. In ARM, writing back into a
    // register the transfer also loads is unpredictable; Thumb defines it as no
    // write-back, and the loaded value wins.
    if (instruction.saved_status || base_register == ProgramCounter ||
        (instruction.writes_back && base_listed && !execution.Thumb())) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const std::uint32_t base = Read(execution, base_register);
    const auto bytes = static_cast<std::uint32_t>(std::popcount(registers)) * WordSize;
    const std::uint32_t start = BlockStart(execution, base, bytes);
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
        execution.state->registers.at(base_register) = BlockWrittenBack(execution, base, bytes);
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

    const auto below_base = static_cast<RegisterSet>((1U << base_register) - 1U);
    const bool base_not_lowest = Holds(registers, base_register) && (registers & below_base) != 0;
    if (instruction.saved_status || base_register == ProgramCounter ||
        (instruction.writes_back && base_not_lowest)) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const std::uint32_t base = Read(execution, base_register);
    const auto bytes = static_cast<std::uint32_t>(std::popcount(registers)) * WordSize;
    const std::uint32_t start = BlockStart(execution, base, bytes);
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
        execution.state->registers.at(base_register) = BlockWrittenBack(execution, base, bytes);
    }
    return Completed();
}

// SWP and SWPB: a load and a store to one address with nothing in between.
template<class Memory>
Stop ExecuteSwap(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const RegisterIndex base = instruction.first;
    if (AnyIsProgramCounter({base, instruction.destination, instruction.second}) ||
        base == instruction.destination || base == instruction.second) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const bool byte = instruction.operation == Operation::SwapByte;
    const Width width = byte ? Width::Byte : Width::Word;
    const std::uint32_t address = Read(execution, base);
    if (!byte && !IsAligned(address, WordSize)) {
        return Halt(StopReason::AlignmentFault, address);
    }

    const std::optional<std::uint32_t> old = Load(*execution.memory, address, width);
    if (!old.has_value()) {
        return Halt(StopReason::ReadFault, address);
    }
    if (!StoreValue(*execution.memory, address, Read(execution, instruction.second), width)) {
        return Halt(StopReason::WriteFault, address);
    }
    execution.state->registers.at(instruction.destination) = old.value();
    return Completed();
}

inline Width WidthOfSize(std::uint32_t size) {
    switch (size) {
    case 1:
        return Width::Byte;
    case 2:
        return Width::Half;
    default:
        return Width::Word;
    }
}

inline std::uint32_t ExclusiveSize(Operation operation) {
    switch (operation) {
    case Operation::LoadExclusiveByte:
    case Operation::StoreExclusiveByte:
        return 1;
    case Operation::LoadExclusiveHalf:
    case Operation::StoreExclusiveHalf:
        return 2;
    case Operation::LoadExclusiveDouble:
    case Operation::StoreExclusiveDouble:
        return 2 * WordSize;
    default:
        return WordSize;
    }
}

// LDREX and its byte, halfword and doubleword forms. The load opens the local
// monitor on its address.
template<class Memory>
Stop ExecuteLoadExclusive(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    const bool pair = instruction.operation == Operation::LoadExclusiveDouble;
    const RegisterIndex low = instruction.destination;
    if (AnyIsProgramCounter({instruction.first, low}) || (pair && low + 1 == ProgramCounter)) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const std::uint32_t size = ExclusiveSize(instruction.operation);
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
        const Width width = WidthOfSize(size);
        const std::optional<std::uint32_t> value = Load(*execution.memory, address, width);
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
    if (AnyIsProgramCounter({status, value, base}) || (pair && value + 1 == ProgramCounter) ||
        status == base || status == value || (pair && status == value + 1)) {
        return Halt(execution, StopReason::Unpredictable);
    }

    const std::uint32_t size = ExclusiveSize(instruction.operation);
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
            const Width width = WidthOfSize(size);
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
