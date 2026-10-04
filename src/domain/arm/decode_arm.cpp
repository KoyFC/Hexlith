#include "domain/arm/decode_arm.h"

#include "domain/arm/decode_vfp.h"

#include <array>
#include <bit>
#include <optional>

namespace hexlith::domain::arm {
namespace {

// The field extractors every line below is written in terms of. Named so that a
// reader can check an encoding against the architecture reference by eye: the
// reference numbers the bits, so the code names the bits.
constexpr std::uint32_t Bits(std::uint32_t word, unsigned high, unsigned low) {
    const unsigned width = high - low + 1;
    const std::uint32_t mask = width == 32 ? ~0U : (1U << width) - 1U;
    return (word >> low) & mask;
}

constexpr bool Bit(std::uint32_t word, unsigned index) {
    return ((word >> index) & 1U) != 0;
}

constexpr RegisterIndex RegisterAt(std::uint32_t word, unsigned low) {
    return static_cast<RegisterIndex>(Bits(word, low + 3, low));
}

// Sign extends a field of the given width, which is how every displacement in
// the encoding is stored.
constexpr std::int32_t SignExtend(std::uint32_t value, unsigned width) {
    const std::uint32_t sign = 1U << (width - 1);
    const std::uint32_t extended = (value ^ sign) - sign;
    return static_cast<std::int32_t>(extended);
}

// The architecture fixes some fields to all zeroes or all ones. A word that
// does not respect them is unpredictable rather than the instruction it looks
// like, so it is refused.
//
// This is not pedantry, it is what makes the decoder useful for telling code
// from data. A compiler emits those fields correctly without exception, so a
// word that gets them wrong is far more likely to be data that happens to look
// like an instruction, and accepting it would plant code in the middle of a
// table.
constexpr bool AllZero(std::uint32_t word, unsigned high, unsigned low) {
    return Bits(word, high, low) == 0;
}

constexpr bool AllOnes(std::uint32_t word, unsigned high, unsigned low) {
    const unsigned width = high - low + 1;
    return Bits(word, high, low) == (1U << width) - 1U;
}

constexpr unsigned RegisterFieldWidth = 4;
constexpr unsigned BranchOffsetWidth = 24;
constexpr unsigned BranchOffsetShift = 2;
constexpr unsigned FullShift = 32;

// The condition is the top four bits of every ARM word, with 0b1111 meaning the
// word belongs to the group that is not conditional at all.
Condition ConditionOf(std::uint32_t word) {
    return static_cast<Condition>(Bits(word, 31, 28));
}

Instruction Undefined(std::uint32_t word) {
    return Instruction{.encoding = word,
                       .operation = Operation::Undefined,
                       .condition = ConditionOf(word),
                       .instruction_set = InstructionSet::Arm,
                       .width = 4};
}

Instruction Start(std::uint32_t word, Operation operation) {
    Instruction instruction = Undefined(word);
    instruction.operation = operation;
    return instruction;
}

// The sixteen data-processing operations, in encoding order.
constexpr std::array<Operation, 16> DataProcessingOperations = {
    Operation::And,
    Operation::Eor,
    Operation::Sub,
    Operation::Rsb,
    Operation::Add,
    Operation::Adc,
    Operation::Sbc,
    Operation::Rsc,
    Operation::Tst,
    Operation::Teq,
    Operation::Cmp,
    Operation::Cmn,
    Operation::Orr,
    Operation::Mov,
    Operation::Bic,
    Operation::Mvn,
};

// Reads the shifter operand of a data-processing instruction or the scaled
// register offset of a transfer.
//
// Three shifts are spelt with an amount of zero and do not mean a shift by zero.
// LSR and ASR by zero mean by 32, and ROR by zero is RRX, a rotate through the
// carry flag by one. Only LSL by zero is really no shift, which is how a plain
// register operand is encoded.
void ReadScaledRegister(Instruction& instruction, std::uint32_t word) {
    instruction.second = RegisterAt(word, 0);
    instruction.shift = static_cast<ShiftKind>(Bits(word, 6, 5));

    if (Bit(word, 4)) {
        instruction.operand = OperandForm::ShiftedRegister;
        instruction.shift_register = RegisterAt(word, 8);
        return;
    }

    const std::uint32_t amount = Bits(word, 11, 7);
    if (amount != 0) {
        instruction.operand = OperandForm::ShiftedRegister;
        instruction.shift_amount = static_cast<std::uint8_t>(amount);
        return;
    }

    switch (instruction.shift) {
    case ShiftKind::LogicalLeft:
        instruction.operand = OperandForm::Register;
        return;
    case ShiftKind::LogicalRight:
    case ShiftKind::ArithmeticRight:
        instruction.operand = OperandForm::ShiftedRegister;
        instruction.shift_amount = FullShift;
        return;
    case ShiftKind::RotateRight:
        instruction.operand = OperandForm::ShiftedRegister;
        instruction.shift = ShiftKind::RotateRightExtended;
        instruction.shift_amount = 1;
        return;
    case ShiftKind::RotateRightExtended:
        break;
    }

    instruction.operand = OperandForm::ShiftedRegister;
}

// The immediate operand of a data-processing instruction: an eight-bit value
// rotated right by twice a four-bit field. That is how a 32-bit machine with
// 32-bit instructions spells a constant, and it is why not every constant can be
// spelt and why a literal pool exists.
std::uint32_t RotatedImmediate(std::uint32_t word) {
    const std::uint32_t value = Bits(word, 7, 0);
    const unsigned rotation = Bits(word, 11, 8) * 2;
    return std::rotr(value, static_cast<int>(rotation));
}

Instruction DecodeDataProcessing(std::uint32_t word, bool immediate_form) {
    const std::uint32_t opcode = Bits(word, 24, 21);
    Instruction instruction = Start(word, DataProcessingOperations.at(opcode));

    instruction.updates_flags = Bit(word, 20);
    instruction.first = RegisterAt(word, 16);
    instruction.destination = RegisterAt(word, 12);

    if (immediate_form) {
        instruction.operand = OperandForm::Immediate;
        instruction.immediate = RotatedImmediate(word);
    } else {
        ReadScaledRegister(instruction, word);
    }

    // The comparisons have no destination and the two moves have no first
    // operand, so the fields that do not apply are left unset rather than
    // holding whatever the encoding put there.
    switch (instruction.operation) {
    case Operation::Tst:
    case Operation::Teq:
    case Operation::Cmp:
    case Operation::Cmn:
        if (!AllZero(word, 15, 12)) {
            return Undefined(word);
        }
        instruction.destination = NoRegister;
        break;
    case Operation::Mov:
    case Operation::Mvn:
        if (!AllZero(word, 19, 16)) {
            return Undefined(word);
        }
        instruction.first = NoRegister;
        break;
    default:
        break;
    }

    return instruction;
}

Instruction DecodeMultiply(std::uint32_t word) {
    const std::uint32_t opcode = Bits(word, 23, 21);
    const bool accumulates = opcode == 1;

    // RdHi and RdLo of a long multiply sit in the fields a short multiply uses
    // for its destination and its accumulator.
    const RegisterIndex high = RegisterAt(word, 16);
    const RegisterIndex low = RegisterAt(word, 12);

    Operation operation = Operation::Undefined;
    switch (opcode) {
    case 0:
    case 1:
        operation = accumulates ? Operation::Mla : Operation::Mul;
        break;
    case 2:
        if (Bit(word, 20)) {
            return Undefined(word);
        }
        operation = Operation::Umaal;
        break;
    case 4:
        operation = Operation::Umull;
        break;
    case 5:
        operation = Operation::Umlal;
        break;
    case 6:
        operation = Operation::Smull;
        break;
    case 7:
        operation = Operation::Smlal;
        break;
    default:
        return Undefined(word);
    }

    if (operation == Operation::Mul && !AllZero(word, 15, 12)) {
        return Undefined(word);
    }

    Instruction instruction = Start(word, operation);
    instruction.updates_flags = Bit(word, 20);
    instruction.second = RegisterAt(word, 0);
    instruction.third = RegisterAt(word, 8);

    const bool long_result = operation != Operation::Mul && operation != Operation::Mla;
    if (long_result) {
        instruction.destination = low;
        instruction.destination_high = high;
    } else {
        instruction.destination = high;
        if (accumulates) {
            instruction.first = low;
        }
    }

    return instruction;
}

Instruction DecodeSynchronisation(std::uint32_t word) {
    const std::uint32_t opcode = Bits(word, 23, 20);

    Operation operation = Operation::Undefined;
    switch (opcode) {
    case 0b0000:
        operation = Operation::Swap;
        break;
    case 0b0100:
        operation = Operation::SwapByte;
        break;
    case 0b1000:
        operation = Operation::StoreExclusive;
        break;
    case 0b1001:
        operation = Operation::LoadExclusive;
        break;
    case 0b1010:
        operation = Operation::StoreExclusiveDouble;
        break;
    case 0b1011:
        operation = Operation::LoadExclusiveDouble;
        break;
    case 0b1100:
        operation = Operation::StoreExclusiveByte;
        break;
    case 0b1101:
        operation = Operation::LoadExclusiveByte;
        break;
    case 0b1110:
        operation = Operation::StoreExclusiveHalf;
        break;
    case 0b1111:
        operation = Operation::LoadExclusiveHalf;
        break;
    default:
        return Undefined(word);
    }

    const bool swap = operation == Operation::Swap || operation == Operation::SwapByte;
    if (swap ? !AllZero(word, 11, 8) : !AllOnes(word, 11, 8)) {
        return Undefined(word);
    }

    // A load leaves nothing to read from a register, so the field a store would
    // take its value from is fixed.
    const bool loads =
        operation == Operation::LoadExclusive || operation == Operation::LoadExclusiveByte ||
        operation == Operation::LoadExclusiveHalf || operation == Operation::LoadExclusiveDouble;
    if (loads && !AllOnes(word, 3, 0)) {
        return Undefined(word);
    }

    // The pair forms name an even register and use the one above it, so an odd
    // register names a pair that does not exist and r14 names one that would run
    // into the program counter.
    const bool pair =
        operation == Operation::LoadExclusiveDouble || operation == Operation::StoreExclusiveDouble;
    if (pair) {
        const std::uint32_t low =
            operation == Operation::LoadExclusiveDouble ? Bits(word, 15, 12) : Bits(word, 3, 0);
        if ((low % 2) != 0 || low == LinkRegister) {
            return Undefined(word);
        }
    }

    Instruction instruction = Start(word, operation);
    instruction.first = RegisterAt(word, 16);
    instruction.destination = RegisterAt(word, 12);
    instruction.second = RegisterAt(word, 0);
    return instruction;
}

// The halfword and doubleword transfers, which the encoding puts in the
// data-processing space rather than with the other transfers because their
// offset field had nowhere else to go.
Instruction DecodeExtraTransfer(std::uint32_t word) {
    const std::uint32_t kind = Bits(word, 6, 5);
    const bool load = Bit(word, 20);

    Operation operation = Operation::Undefined;
    switch (kind) {
    case 0b01:
        operation = load ? Operation::LoadHalf : Operation::StoreHalf;
        break;
    case 0b10:
        operation = load ? Operation::LoadSignedByte : Operation::LoadDouble;
        break;
    case 0b11:
        operation = load ? Operation::LoadSignedHalf : Operation::StoreDouble;
        break;
    default:
        return Undefined(word);
    }

    // ARMv6K has no unprivileged form of these transfers: with no pre-indexing
    // the write-back bit is unpredictable rather than meaningful. ARMv7 gave it
    // a meaning, which is why a newer disassembler reads these as LDRHT and
    // friends.
    const bool preindexed = Bit(word, 24);
    if (!preindexed && Bit(word, 21)) {
        return Undefined(word);
    }

    // A doubleword transfer names an even register and uses the one above it as
    // well, so an odd register names a pair that does not exist.
    const bool pair = operation == Operation::LoadDouble || operation == Operation::StoreDouble;
    if (pair && (Bits(word, 15, 12) % 2) != 0) {
        return Undefined(word);
    }

    Instruction instruction = Start(word, operation);
    instruction.preindexed = preindexed;
    instruction.add_offset = Bit(word, 23);
    instruction.writes_back = !instruction.preindexed || Bit(word, 21);
    instruction.first = RegisterAt(word, 16);
    instruction.destination = RegisterAt(word, 12);

    if (Bit(word, 22)) {
        instruction.operand = OperandForm::Immediate;
        instruction.immediate = (Bits(word, 11, 8) << RegisterFieldWidth) | Bits(word, 3, 0);
        return instruction;
    }

    // The register form leaves bits 11 to 8 empty, where the immediate form
    // keeps the top half of its offset, and anything in them is unpredictable.
    if (!AllZero(word, 11, 8)) {
        return Undefined(word);
    }
    instruction.operand = OperandForm::Register;
    instruction.second = RegisterAt(word, 0);
    return instruction;
}

Instruction DecodeStatusRegisterMove(std::uint32_t word) {
    const bool to_status = Bit(word, 21);
    if (to_status ? !AllOnes(word, 15, 12) || !AllZero(word, 11, 4)
                  : !AllOnes(word, 19, 16) || !AllZero(word, 11, 0)) {
        return Undefined(word);
    }

    Instruction instruction =
        Start(word, to_status ? Operation::MoveToStatus : Operation::MoveFromStatus);
    instruction.saved_status = Bit(word, 22);
    if (to_status) {
        instruction.immediate = Bits(word, 19, 16);
        instruction.second = RegisterAt(word, 0);
    } else {
        instruction.destination = RegisterAt(word, 12);
    }
    return instruction;
}

// The two exchanging branches and the leading zero count, which share a row of
// the table because all three take one register and produce one result.
Instruction DecodeExchangeOrCount(std::uint32_t word, std::uint32_t operation_bits) {
    const bool counts = Bits(word, 7, 4) == 0b0001 && operation_bits == 0b11;
    if (counts) {
        if (!AllOnes(word, 19, 16) || !AllOnes(word, 11, 8)) {
            return Undefined(word);
        }
        Instruction instruction = Start(word, Operation::CountLeadingZeros);
        instruction.destination = RegisterAt(word, 12);
        instruction.second = RegisterAt(word, 0);
        return instruction;
    }

    if (operation_bits != 0b01 || !AllOnes(word, 19, 8)) {
        return Undefined(word);
    }

    Instruction instruction =
        Start(word,
              Bits(word, 7, 4) == 0b0011 ? Operation::BranchLinkExchangeRegister
                                         : Operation::BranchExchange);
    instruction.second = RegisterAt(word, 0);
    return instruction;
}

Instruction DecodeSaturatingArithmetic(std::uint32_t word, std::uint32_t operation_bits) {
    if (!AllZero(word, 11, 8)) {
        return Undefined(word);
    }

    static constexpr std::array<Operation, 4> Saturating = {
        Operation::Qadd, Operation::Qsub, Operation::QdoubleAdd, Operation::QdoubleSub};

    Instruction instruction = Start(word, Saturating.at(operation_bits));
    instruction.destination = RegisterAt(word, 12);
    instruction.second = RegisterAt(word, 0);
    instruction.first = RegisterAt(word, 16);
    return instruction;
}

Instruction DecodeBreakpoint(std::uint32_t word) {
    // A breakpoint is unconditional. The encoding carries a condition field like
    // every other and a value in it other than always is unpredictable.
    if (ConditionOf(word) != Condition::Always) {
        return Undefined(word);
    }

    Instruction instruction = Start(word, Operation::Breakpoint);
    instruction.immediate = (Bits(word, 19, 8) << RegisterFieldWidth) | Bits(word, 3, 0);
    return instruction;
}

// Calling the secure monitor, which the security extensions added and which only
// code running in a privileged mode can issue. Whether anything in the title
// reaches it is a question for the kernel reimplementation, not for the decoder.
Instruction DecodeSecureMonitorCall(std::uint32_t word) {
    if (!AllZero(word, 19, 8)) {
        return Undefined(word);
    }

    Instruction instruction = Start(word, Operation::SecureMonitorCall);
    instruction.immediate = Bits(word, 3, 0);
    return instruction;
}

// The halfword multiplies, where bits 5 and 6 say which half of each source is
// read. These are the DSP instructions ARMv5TE added, and the title's audio and
// fixed-point code is where they would turn up.
Instruction DecodeHalfwordMultiply(std::uint32_t word, std::uint32_t operation_bits) {
    Instruction instruction = Undefined(word);
    instruction.right_half_high = Bit(word, 5);
    instruction.left_half_high = Bit(word, 6);
    instruction.second = RegisterAt(word, 0);
    instruction.third = RegisterAt(word, 8);

    switch (operation_bits) {
    case 0b00:
        instruction.operation = Operation::SmlaHalf;
        instruction.destination = RegisterAt(word, 16);
        instruction.first = RegisterAt(word, 12);
        return instruction;
    case 0b01:
        // Here bit 5 picks the operation instead of a half, so only bit 6
        // selects one.
        instruction.right_half_high = false;
        instruction.destination = RegisterAt(word, 16);
        if (Bit(word, 5)) {
            if (!AllZero(word, 15, 12)) {
                return Undefined(word);
            }
            instruction.operation = Operation::SmulwHalf;
            return instruction;
        }
        instruction.operation = Operation::SmlawHalf;
        instruction.first = RegisterAt(word, 12);
        return instruction;
    case 0b10:
        instruction.operation = Operation::SmlalHalf;
        instruction.destination = RegisterAt(word, 12);
        instruction.destination_high = RegisterAt(word, 16);
        return instruction;
    case 0b11:
        if (!AllZero(word, 15, 12)) {
            return Undefined(word);
        }
        instruction.operation = Operation::SmulHalf;
        instruction.destination = RegisterAt(word, 16);
        return instruction;
    default:
        break;
    }

    return Undefined(word);
}

// Saturating arithmetic, the halfword multiplies, and the instructions the
// encoding hides in the space a data-processing comparison would occupy if it did
// not set the flags. Bits 7 to 4 pick the row.
Instruction DecodeMiscellaneous(std::uint32_t word) {
    const std::uint32_t operation_bits = Bits(word, 22, 21);

    switch (Bits(word, 7, 4)) {
    case 0b0000:
        return DecodeStatusRegisterMove(word);
    case 0b0001:
    case 0b0010:
    case 0b0011:
        return DecodeExchangeOrCount(word, operation_bits);
    case 0b0101:
        return DecodeSaturatingArithmetic(word, operation_bits);
    case 0b0111:
        if (operation_bits == 0b01) {
            return DecodeBreakpoint(word);
        }
        if (operation_bits == 0b11) {
            return DecodeSecureMonitorCall(word);
        }
        return Undefined(word);
    default:
        break;
    }

    if (Bit(word, 7) && !Bit(word, 4)) {
        return DecodeHalfwordMultiply(word, operation_bits);
    }

    return Undefined(word);
}

// The 000 space: data processing with a register operand, and everything the
// encoding tucked in beside it.
Instruction DecodeRegisterSpace(std::uint32_t word) {
    const bool misc_space = Bits(word, 24, 23) == 0b10 && !Bit(word, 20);

    if (Bits(word, 7, 4) == 0b1001) {
        if (Bits(word, 27, 24) == 0b0000) {
            return DecodeMultiply(word);
        }
        return DecodeSynchronisation(word);
    }

    if (Bit(word, 7) && Bit(word, 4)) {
        return DecodeExtraTransfer(word);
    }

    if (misc_space) {
        return DecodeMiscellaneous(word);
    }

    return DecodeDataProcessing(word, false);
}

// The hint instructions, which ARMv6K put in the corner of the status register
// space that writes no field of it. A write of nothing is a plausible place to
// hide an instruction that does nothing.
//
// There are sixteen thousand of them in the reference image's text segment,
// which is what a compiler leaves behind when it aligns code.
Instruction DecodeHint(std::uint32_t word) {
    // The field that would rotate an immediate is part of the hint number, and
    // the architecture fixes it to zero.
    if (!AllZero(word, 11, 8)) {
        return Undefined(word);
    }

    Operation operation = Operation::NoOperation;

    switch (Bits(word, 7, 0)) {
    case 0:
        operation = Operation::NoOperation;
        break;
    case 1:
        operation = Operation::Yield;
        break;
    case 2:
        operation = Operation::WaitForEvent;
        break;
    case 3:
        operation = Operation::WaitForInterrupt;
        break;
    case 4:
        operation = Operation::SendEvent;
        break;
    default:
        // ARMv6K allocates five hints and leaves the rest of the field alone.
        // Later architectures require an unallocated hint to execute as a
        // no-operation, but this is not one of them, and refusing a word the
        // architecture does not define is also what keeps a table of bytes from
        // being read as a run of instructions that do nothing.
        return Undefined(word);
    }

    return Start(word, operation);
}

Instruction DecodeImmediateSpace(std::uint32_t word) {
    // The space a comparison that does not set the flags would occupy holds the
    // immediate form of a write to the status register, and nothing else in
    // ARMv6K.
    if (Bits(word, 24, 23) == 0b10 && !Bit(word, 20)) {
        if (!Bit(word, 21)) {
            return Undefined(word);
        }
        if (!AllOnes(word, 15, 12)) {
            return Undefined(word);
        }
        // Writing no field of the status register is how a hint is spelt.
        if (AllZero(word, 19, 16)) {
            return Bit(word, 22) ? Undefined(word) : DecodeHint(word);
        }
        Instruction instruction = Start(word, Operation::MoveToStatus);
        instruction.saved_status = Bit(word, 22);
        instruction.operand = OperandForm::Immediate;
        instruction.immediate = RotatedImmediate(word);
        instruction.third = static_cast<RegisterIndex>(Bits(word, 19, 16));
        return instruction;
    }

    return DecodeDataProcessing(word, true);
}

Instruction DecodeSingleTransfer(std::uint32_t word, bool register_offset) {
    const bool load = Bit(word, 20);
    const bool byte = Bit(word, 22);
    const bool preindexed = Bit(word, 24);
    const bool write_back = Bit(word, 21);

    // With no pre-indexing, the write-back bit means the transfer runs with user
    // mode privileges instead.
    const bool unprivileged = !preindexed && write_back;

    Operation operation = Operation::Undefined;
    if (unprivileged) {
        if (load) {
            operation = byte ? Operation::LoadByteUnprivileged : Operation::LoadWordUnprivileged;
        } else {
            operation = byte ? Operation::StoreByteUnprivileged : Operation::StoreWordUnprivileged;
        }
    } else if (load) {
        operation = byte ? Operation::LoadByte : Operation::LoadWord;
    } else {
        operation = byte ? Operation::StoreByte : Operation::StoreWord;
    }

    Instruction instruction = Start(word, operation);
    instruction.preindexed = preindexed;
    instruction.add_offset = Bit(word, 23);
    instruction.writes_back = !preindexed || write_back;
    instruction.first = RegisterAt(word, 16);
    instruction.destination = RegisterAt(word, 12);

    if (register_offset) {
        ReadScaledRegister(instruction, word);
    } else {
        instruction.operand = OperandForm::Immediate;
        instruction.immediate = Bits(word, 11, 0);
    }

    return instruction;
}

Instruction DecodeBlockTransfer(std::uint32_t word) {
    // A transfer of no registers is not an instruction. The encoding allows the
    // list to be empty and the architecture calls the result unpredictable.
    if (AllZero(word, 15, 0)) {
        return Undefined(word);
    }

    Instruction instruction =
        Start(word, Bit(word, 20) ? Operation::LoadMultiple : Operation::StoreMultiple);
    instruction.preindexed = Bit(word, 24);
    instruction.add_offset = Bit(word, 23);
    instruction.saved_status = Bit(word, 22);
    instruction.writes_back = Bit(word, 21);
    instruction.first = RegisterAt(word, 16);
    instruction.registers = static_cast<RegisterSet>(Bits(word, 15, 0));
    return instruction;
}

Instruction DecodeBranch(std::uint32_t word) {
    Instruction instruction =
        Start(word, Bit(word, 24) ? Operation::BranchLink : Operation::Branch);
    instruction.branch_offset = SignExtend(Bits(word, BranchOffsetWidth - 1, 0), BranchOffsetWidth)
                                << BranchOffsetShift;
    return instruction;
}

// The signed multiplies and the packing, extension, saturation and reversal
// group that ARMv6 added, which share the space the encoding gives to a transfer
// with a register offset and a one in bit four.
//
// The three families are told apart by bits 24 and 23, and within each one by
// bits 22 to 20 and bits 7 to 5. The architecture reference lays them out as a
// table of those two fields, so this does too, because the only way to check a
// decoder of this group against the reference is to be able to read the two
// fields off it.
std::optional<ParallelPrefix> ParallelPrefixOf(std::uint32_t bits) {
    switch (bits) {
    case 0b001:
        return ParallelPrefix::Signed;
    case 0b010:
        return ParallelPrefix::SignedSaturating;
    case 0b011:
        return ParallelPrefix::SignedHalving;
    case 0b101:
        return ParallelPrefix::Unsigned;
    case 0b110:
        return ParallelPrefix::UnsignedSaturating;
    case 0b111:
        return ParallelPrefix::UnsignedHalving;
    default:
        // The two values that are not a prefix belong to nothing in this
        // quarter of the media space.
        return std::nullopt;
    }
}

std::optional<ParallelShape> ParallelShapeOf(std::uint32_t bits) {
    switch (bits) {
    case 0b000:
        return ParallelShape::Add16;
    case 0b001:
        return ParallelShape::AddSubtractExchange;
    case 0b010:
        return ParallelShape::SubtractAddExchange;
    case 0b011:
        return ParallelShape::Subtract16;
    case 0b100:
        return ParallelShape::Add8;
    case 0b111:
        return ParallelShape::Subtract8;
    default:
        // The two rows the table leaves blank are not instructions.
        return std::nullopt;
    }
}

Instruction DecodeParallelArithmetic(std::uint32_t word) {
    const std::optional<ParallelPrefix> prefix = ParallelPrefixOf(Bits(word, 22, 20));
    const std::optional<ParallelShape> shape = ParallelShapeOf(Bits(word, 7, 5));
    if (!prefix.has_value() || !shape.has_value() || !AllOnes(word, 11, 8)) {
        return Undefined(word);
    }

    Instruction instruction = Start(word, Operation::ParallelArithmetic);
    instruction.parallel_prefix = prefix.value();
    instruction.parallel_shape = shape.value();
    instruction.destination = RegisterAt(word, 12);
    instruction.first = RegisterAt(word, 16);
    instruction.second = RegisterAt(word, 0);
    return instruction;
}

// Sign and zero extension, with and without an addend. The register that would
// hold the addend is all ones when there is none, which is what separates SXTB
// from SXTAB.
Instruction DecodeExtension(std::uint32_t word, Operation extending, Operation accumulating) {
    const RegisterIndex addend = RegisterAt(word, 16);
    const bool accumulates = addend != ProgramCounter;

    if (!AllZero(word, 9, 8)) {
        return Undefined(word);
    }

    Instruction instruction = Start(word, accumulates ? accumulating : extending);
    instruction.destination = RegisterAt(word, 12);
    instruction.second = RegisterAt(word, 0);
    instruction.first = accumulates ? addend : NoRegister;

    // The rotation is a count of bytes, so the encoding holds a quarter of it.
    instruction.shift = ShiftKind::RotateRight;
    instruction.shift_amount = static_cast<std::uint8_t>(Bits(word, 11, 10) * 8);
    return instruction;
}

Instruction DecodeSaturate(std::uint32_t word, Operation operation) {
    Instruction instruction = Start(word, operation);
    instruction.destination = RegisterAt(word, 12);
    instruction.second = RegisterAt(word, 0);

    // The bound is a bit count, one more than the field holds for the signed
    // form and exactly what it holds for the unsigned one. Both are reported as
    // the field, because the difference belongs to the operation.
    instruction.immediate = Bits(word, 20, 16);
    instruction.shift = Bit(word, 6) ? ShiftKind::ArithmeticRight : ShiftKind::LogicalLeft;
    instruction.shift_amount = static_cast<std::uint8_t>(Bits(word, 11, 7));
    return instruction;
}

Instruction DecodeSaturateHalfword(std::uint32_t word, Operation operation) {
    if (!AllOnes(word, 11, 8)) {
        return Undefined(word);
    }
    Instruction instruction = Start(word, operation);
    instruction.destination = RegisterAt(word, 12);
    instruction.second = RegisterAt(word, 0);
    instruction.immediate = Bits(word, 19, 16);
    return instruction;
}

// Packing a halfword from each source into one register. The shift applies to
// the second source before the halves are taken.
Instruction DecodePackHalfword(std::uint32_t word) {
    const bool from_the_top = Bit(word, 6);

    Instruction instruction = Start(word, from_the_top ? Operation::Pkhtb : Operation::Pkhbt);
    instruction.destination = RegisterAt(word, 12);
    instruction.first = RegisterAt(word, 16);
    instruction.second = RegisterAt(word, 0);
    instruction.shift = from_the_top ? ShiftKind::ArithmeticRight : ShiftKind::LogicalLeft;
    instruction.shift_amount = static_cast<std::uint8_t>(Bits(word, 11, 7));
    return instruction;
}

// Picking each byte of the result from one source or the other, by the flags a
// parallel addition left behind. Unlike a reversal it reads two registers.
Instruction DecodeSelect(std::uint32_t word) {
    if (!AllOnes(word, 11, 8)) {
        return Undefined(word);
    }

    Instruction instruction = Start(word, Operation::Sel);
    instruction.destination = RegisterAt(word, 12);
    instruction.first = RegisterAt(word, 16);
    instruction.second = RegisterAt(word, 0);
    return instruction;
}

Instruction DecodeReversal(std::uint32_t word, Operation operation) {
    if (!AllOnes(word, 19, 16) || !AllOnes(word, 11, 8)) {
        return Undefined(word);
    }
    Instruction instruction = Start(word, operation);
    instruction.destination = RegisterAt(word, 12);
    instruction.second = RegisterAt(word, 0);
    return instruction;
}

// The signed half of the packing table: rows 000, 010 and 011.
Instruction DecodePackingSigned(std::uint32_t word, std::uint32_t op1, std::uint32_t op2) {
    switch (op1) {
    case 0b000:
        if (op2 == 0b011) {
            return DecodeExtension(word, Operation::Sxtb16, Operation::Sxtab16);
        }
        if (op2 == 0b101) {
            return DecodeSelect(word);
        }
        // The rest of the row is packing, which like saturation occupies only the
        // entries with an even second field.
        if ((op2 & 0b001) == 0) {
            return DecodePackHalfword(word);
        }
        return Undefined(word);
    case 0b010:
        if (op2 == 0b001) {
            return DecodeSaturateHalfword(word, Operation::Ssat16);
        }
        if (op2 == 0b011) {
            return DecodeExtension(word, Operation::Sxtb, Operation::Sxtab);
        }
        return Undefined(word);
    case 0b011:
        if (op2 == 0b001) {
            return DecodeReversal(word, Operation::Rev);
        }
        if (op2 == 0b011) {
            return DecodeExtension(word, Operation::Sxth, Operation::Sxtah);
        }
        if (op2 == 0b101) {
            return DecodeReversal(word, Operation::Rev16);
        }
        return Undefined(word);
    default:
        break;
    }
    return Undefined(word);
}

// The unsigned half: rows 100, 110 and 111.
Instruction DecodePackingUnsigned(std::uint32_t word, std::uint32_t op1, std::uint32_t op2) {
    switch (op1) {
    case 0b100:
        if (op2 == 0b011) {
            return DecodeExtension(word, Operation::Uxtb16, Operation::Uxtab16);
        }
        return Undefined(word);
    case 0b110:
        if (op2 == 0b001) {
            return DecodeSaturateHalfword(word, Operation::Usat16);
        }
        if (op2 == 0b011) {
            return DecodeExtension(word, Operation::Uxtb, Operation::Uxtab);
        }
        return Undefined(word);
    case 0b111:
        if (op2 == 0b011) {
            return DecodeExtension(word, Operation::Uxth, Operation::Uxtah);
        }
        if (op2 == 0b101) {
            return DecodeReversal(word, Operation::Revsh);
        }
        return Undefined(word);
    default:
        break;
    }
    return Undefined(word);
}

Instruction DecodePacking(std::uint32_t word) {
    const std::uint32_t op1 = Bits(word, 22, 20);
    const std::uint32_t op2 = Bits(word, 7, 5);

    // Saturation to an arbitrary width takes every row with bit 21 set and an
    // even second field, which is why it is tested before the rows themselves.
    if ((op2 & 0b001) == 0 && (op1 & 0b010) != 0) {
        return DecodeSaturate(word, (op1 & 0b100) != 0 ? Operation::Usat : Operation::Ssat);
    }

    return (op1 & 0b100) == 0 ? DecodePackingSigned(word, op1, op2)
                              : DecodePackingUnsigned(word, op1, op2);
}

// Which of the dual and most-significant-word multiplies a row names, or nothing
// when the row is not one of the three ARMv6 filled in.
Operation SignedMultiplyOperation(std::uint32_t op1, std::uint32_t op2, bool accumulates) {
    const bool subtracts = (op2 & 0b010) != 0;

    switch (op1) {
    case 0b000:
        if ((op2 & 0b100) != 0) {
            return Operation::Undefined;
        }
        if (subtracts) {
            return accumulates ? Operation::Smlsd : Operation::Smusd;
        }
        return accumulates ? Operation::Smlad : Operation::Smuad;
    case 0b100:
        if ((op2 & 0b100) != 0) {
            return Operation::Undefined;
        }
        return subtracts ? Operation::Smlsld : Operation::Smlald;
    case 0b101:
        // SMMLS has no form without an accumulator, so the all-ones field that
        // turns SMMLA into SMMUL makes it an unpredictable SMMLS instead.
        if ((op2 & 0b110) == 0b110) {
            return accumulates ? Operation::Smmls : Operation::Undefined;
        }
        if ((op2 & 0b110) == 0b000) {
            return accumulates ? Operation::Smmla : Operation::Smmul;
        }
        return Operation::Undefined;
    default:
        break;
    }

    return Operation::Undefined;
}

Instruction DecodeSignedMultiply(std::uint32_t word) {
    const RegisterIndex addend = RegisterAt(word, 12);

    // All ones in the field that would name the addend means there is none, which
    // is how SMUAD and SMLAD share an encoding.
    const bool accumulates = addend != ProgramCounter;
    const Operation operation =
        SignedMultiplyOperation(Bits(word, 22, 20), Bits(word, 7, 5), accumulates);
    if (operation == Operation::Undefined) {
        return Undefined(word);
    }

    Instruction instruction = Start(word, operation);
    instruction.second = RegisterAt(word, 0);
    instruction.third = RegisterAt(word, 8);

    if (operation == Operation::Smlald || operation == Operation::Smlsld) {
        instruction.destination = addend;
        instruction.destination_high = RegisterAt(word, 16);
    } else {
        instruction.destination = RegisterAt(word, 16);
        instruction.first = accumulates ? addend : NoRegister;
    }
    return instruction;
}

// The sum of absolute differences, which is the one instruction ARMv6 put in the
// last quarter of the media space. The rest of that quarter is the bitfield
// group ARMv6T2 added and the encoding that is permanently undefined.
Instruction DecodeAbsoluteDifference(std::uint32_t word) {
    if (Bits(word, 22, 20) != 0b000 || Bits(word, 7, 5) != 0b000) {
        return Undefined(word);
    }

    const RegisterIndex addend = RegisterAt(word, 12);
    const bool accumulates = addend != ProgramCounter;

    Instruction instruction = Start(word, accumulates ? Operation::Usada8 : Operation::Usad8);
    instruction.destination = RegisterAt(word, 16);
    instruction.first = accumulates ? addend : NoRegister;
    instruction.third = RegisterAt(word, 8);
    instruction.second = RegisterAt(word, 0);
    return instruction;
}

Instruction DecodeMedia(std::uint32_t word) {
    switch (Bits(word, 24, 23)) {
    case 0b00:
        return DecodeParallelArithmetic(word);
    case 0b01:
        return DecodePacking(word);
    case 0b10:
        return DecodeSignedMultiply(word);
    case 0b11:
        return DecodeAbsoluteDifference(word);
    default:
        break;
    }

    return Undefined(word);
}

Instruction DecodeCoprocessorTransfer(std::uint32_t word) {
    // Two registers at a time, which ARMv5TE added and which sits in the space
    // a transfer with no pre-indexing and no write-back would use.
    if (Bits(word, 24, 21) == 0b0010) {
        Instruction instruction = Start(word,
                                        Bit(word, 20) ? Operation::MoveFromCoprocessorPair
                                                      : Operation::MoveToCoprocessorPair);
        instruction.first = RegisterAt(word, 16);
        instruction.destination = RegisterAt(word, 12);
        instruction.coprocessor = static_cast<std::uint8_t>(Bits(word, 11, 8));
        instruction.coprocessor_opcode1 = static_cast<std::uint8_t>(Bits(word, 7, 4));
        instruction.second = RegisterAt(word, 0);
        return instruction;
    }

    // Of the four combinations of pre-indexing and write-back, the one with
    // neither is the unindexed form, whose eight low bits are an option for the
    // coprocessor rather than an offset. That form requires the add bit set, and
    // with it clear the encoding is not an instruction.
    const bool unindexed = !Bit(word, 24) && !Bit(word, 21);
    if (unindexed && !Bit(word, 23)) {
        return Undefined(word);
    }

    Instruction instruction =
        Start(word, Bit(word, 20) ? Operation::LoadCoprocessor : Operation::StoreCoprocessor);
    instruction.preindexed = Bit(word, 24);
    instruction.add_offset = Bit(word, 23);
    instruction.saved_status = Bit(word, 22);
    instruction.writes_back = Bit(word, 21);
    instruction.first = RegisterAt(word, 16);
    instruction.destination = RegisterAt(word, 12);
    instruction.coprocessor = static_cast<std::uint8_t>(Bits(word, 11, 8));
    instruction.operand = OperandForm::Immediate;
    instruction.immediate = unindexed ? Bits(word, 7, 0) : Bits(word, 7, 0) * 4;
    return instruction;
}

Instruction DecodeCoprocessorOperation(std::uint32_t word) {
    if (Bit(word, 24)) {
        Instruction instruction = Start(word, Operation::SoftwareInterrupt);
        instruction.immediate = Bits(word, 23, 0);
        return instruction;
    }

    if (!Bit(word, 4)) {
        Instruction instruction = Start(word, Operation::CoprocessorDataOperation);
        instruction.coprocessor_opcode1 = static_cast<std::uint8_t>(Bits(word, 23, 20));
        instruction.first = RegisterAt(word, 16);
        instruction.destination = RegisterAt(word, 12);
        instruction.coprocessor = static_cast<std::uint8_t>(Bits(word, 11, 8));
        instruction.coprocessor_opcode2 = static_cast<std::uint8_t>(Bits(word, 7, 5));
        instruction.second = RegisterAt(word, 0);
        return instruction;
    }

    Instruction instruction =
        Start(word, Bit(word, 20) ? Operation::MoveFromCoprocessor : Operation::MoveToCoprocessor);
    instruction.coprocessor_opcode1 = static_cast<std::uint8_t>(Bits(word, 23, 21));
    instruction.first = RegisterAt(word, 16);
    instruction.destination = RegisterAt(word, 12);
    instruction.coprocessor = static_cast<std::uint8_t>(Bits(word, 11, 8));
    instruction.coprocessor_opcode2 = static_cast<std::uint8_t>(Bits(word, 7, 5));
    instruction.second = RegisterAt(word, 0);
    return instruction;
}

// Branching with a link to a displacement, which is how ARM code calls Thumb
// code at an address it knows. The halfword of the displacement is in bit 24,
// which is why this one encoding needs a condition field it cannot use.
Instruction DecodeBranchLinkExchange(std::uint32_t word) {
    Instruction instruction = Start(word, Operation::BranchLinkExchangeImmediate);
    instruction.condition = Condition::Unconditional;

    const std::int32_t words = SignExtend(Bits(word, BranchOffsetWidth - 1, 0), BranchOffsetWidth);
    instruction.branch_offset = (words << BranchOffsetShift) | (Bit(word, 24) ? 2 : 0);
    return instruction;
}

// Clearing the exclusive monitor, and warming the cache. Neither reads or writes
// a general register, and they share the space a transfer would use.
Instruction DecodeCacheAndMonitor(std::uint32_t word, std::uint32_t group) {
    // One encoding, no operands.
    constexpr std::uint32_t ClearExclusiveEncoding = 0xF57FF01F;
    if (word == ClearExclusiveEncoding) {
        return Start(word, Operation::ClearExclusive);
    }

    const bool preload = Bit(word, 24) && Bits(word, 22, 20) == 0b101 && AllOnes(word, 15, 12) &&
                         (group == 0b010 || !Bit(word, 4));
    if (!preload) {
        return Undefined(word);
    }

    Instruction instruction = Start(word, Operation::Preload);
    instruction.first = RegisterAt(word, 16);
    instruction.add_offset = Bit(word, 23);
    if (group == 0b010) {
        instruction.operand = OperandForm::Immediate;
        instruction.immediate = Bits(word, 11, 0);
    } else {
        ReadScaledRegister(instruction, word);
    }
    return instruction;
}

// Setting the byte order, and changing the interrupt mask and the processor
// mode. They share a row, told apart by the field that would name a register.
Instruction DecodeProcessorState(std::uint32_t word) {
    if (Bits(word, 27, 20) != 0b00010000 || !AllZero(word, 7, 5)) {
        return Undefined(word);
    }

    if (Bits(word, 19, 16) == 0b0001) {
        if (!AllZero(word, 15, 10) || !AllZero(word, 8, 0)) {
            return Undefined(word);
        }
        Instruction instruction = Start(word, Operation::SetEndianness);
        instruction.immediate = Bit(word, 9) ? 1 : 0;
        return instruction;
    }

    if (Bit(word, 16) || !AllZero(word, 15, 9) || Bit(word, 4)) {
        return Undefined(word);
    }

    Instruction instruction = Start(word, Operation::ChangeProcessorState);
    instruction.immediate = Bits(word, 19, 0);
    return instruction;
}

// Saving and restoring the state an exception has to return to, which only
// privileged code reaches. Both are written as one shape with eleven bits of
// nothing in the middle.
Instruction DecodeExceptionReturn(std::uint32_t word) {
    const std::uint32_t kind = Bits(word, 22, 20);

    if ((kind == 0b100 || kind == 0b110) && Bits(word, 19, 16) == StackPointer &&
        Bits(word, 15, 5) == 0b00000101000) {
        Instruction instruction = Start(word, Operation::StoreReturnState);
        instruction.first = StackPointer;
        instruction.preindexed = Bit(word, 24);
        instruction.add_offset = Bit(word, 23);
        instruction.writes_back = Bit(word, 21);
        instruction.immediate = Bits(word, 4, 0);
        return instruction;
    }

    if ((kind == 0b001 || kind == 0b011) && Bits(word, 15, 0) == 0x0A00) {
        Instruction instruction = Start(word, Operation::ReturnFromException);
        instruction.first = RegisterAt(word, 16);
        instruction.preindexed = Bit(word, 24);
        instruction.add_offset = Bit(word, 23);
        instruction.writes_back = Bit(word, 21);
        return instruction;
    }

    return Undefined(word);
}

// The group the encoding reaches by setting the condition field to all ones.
// From ARMv5 that is not a condition that never holds, it is a second opcode
// space, and the instructions in it are unconditional.
Instruction DecodeUnconditional(std::uint32_t word) {
    const std::uint32_t group = Bits(word, 27, 25);

    switch (group) {
    case 0b000:
        return DecodeProcessorState(word);
    case 0b010:
    case 0b011:
        return DecodeCacheAndMonitor(word, group);
    case 0b100:
        return DecodeExceptionReturn(word);
    case 0b101:
        return DecodeBranchLinkExchange(word);
    case 0b110:
    case 0b111:
        break;
    default:
        return Undefined(word);
    }

    // The coprocessor instructions have a second encoding in this space, which
    // means the same thing without a condition. The quarter a software interrupt
    // would occupy is not one of them.
    if (group == 0b111 && Bit(word, 24)) {
        return Undefined(word);
    }

    // Floating point is conditional only. The unconditional encodings on
    // coprocessors 10 and 11 are where ARMv7 put Advanced SIMD, which the ARM11
    // does not have, and on an ARM11 they reach the floating point unit as
    // instructions it does not define.
    if (AddressesFloatingPoint(word)) {
        return Undefined(word);
    }

    Instruction instruction =
        group == 0b110 ? DecodeCoprocessorTransfer(word) : DecodeCoprocessorOperation(word);
    instruction.condition = Condition::Unconditional;
    return instruction;
}

}  // namespace

Instruction DecodeArm(std::uint32_t word) {
    if (ConditionOf(word) == Condition::Unconditional) {
        return DecodeUnconditional(word);
    }

    switch (Bits(word, 27, 25)) {
    case 0b000:
        return DecodeRegisterSpace(word);
    case 0b001:
        return DecodeImmediateSpace(word);
    case 0b010:
        return DecodeSingleTransfer(word, false);
    case 0b011:
        return Bit(word, 4) ? DecodeMedia(word) : DecodeSingleTransfer(word, true);
    case 0b100:
        return DecodeBlockTransfer(word);
    case 0b101:
        return DecodeBranch(word);
    case 0b110:
        return AddressesFloatingPoint(word) ? DecodeVfp(word) : DecodeCoprocessorTransfer(word);
    case 0b111:
        // Bit 24 makes the word a software interrupt, whatever the coprocessor
        // field would have said.
        if (!Bit(word, 24) && AddressesFloatingPoint(word)) {
            return DecodeVfp(word);
        }
        return DecodeCoprocessorOperation(word);
    default:
        break;
    }

    return Undefined(word);
}

}  // namespace hexlith::domain::arm
