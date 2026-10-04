#include "domain/arm/decode_thumb.h"

#include <array>

namespace hexlith::domain::arm {
namespace {

constexpr std::uint32_t Bits(std::uint32_t word, unsigned high, unsigned low) {
    const unsigned width = high - low + 1;
    const std::uint32_t mask = width == 32 ? ~0U : (1U << width) - 1U;
    return (word >> low) & mask;
}

constexpr bool Bit(std::uint32_t word, unsigned index) {
    return ((word >> index) & 1U) != 0;
}

// A Thumb register field is three bits, so eight registers are reachable without
// one of the instructions that was given room for a fourth bit.
constexpr RegisterIndex LowRegisterAt(std::uint32_t word, unsigned low) {
    return static_cast<RegisterIndex>(Bits(word, low + 2, low));
}

constexpr std::int32_t SignExtend(std::uint32_t value, unsigned width) {
    const std::uint32_t sign = 1U << (width - 1);
    return static_cast<std::int32_t>((value ^ sign) - sign);
}

constexpr unsigned WordScale = 4;
constexpr unsigned HalfwordScale = 2;
constexpr std::int32_t HalfwordStep = 2;
constexpr std::uint32_t FullShift = 32;

Instruction Undefined(std::uint16_t first) {
    return Instruction{.encoding = first,
                       .operation = Operation::Undefined,
                       .condition = Condition::Always,
                       .instruction_set = InstructionSet::Thumb,
                       .width = 2};
}

Instruction Start(std::uint16_t first, Operation operation) {
    Instruction instruction = Undefined(first);
    instruction.operation = operation;

    // Thumb has no field to suppress the flag update and no conditional
    // execution outside a branch, so almost everything that computes a value
    // sets the flags. The ones that do not say so for themselves.
    instruction.updates_flags = true;
    return instruction;
}

// A shift by an immediate, and the move that a shift by zero turns into.
Instruction DecodeShiftImmediate(std::uint16_t first) {
    const std::uint32_t kind = Bits(first, 12, 11);
    const std::uint32_t amount = Bits(first, 10, 6);

    Instruction instruction = Start(first, Operation::Mov);
    instruction.destination = LowRegisterAt(first, 0);
    instruction.second = LowRegisterAt(first, 3);
    instruction.shift = static_cast<ShiftKind>(kind);
    instruction.operand = OperandForm::ShiftedRegister;

    if (amount != 0) {
        instruction.shift_amount = static_cast<std::uint8_t>(amount);
        return instruction;
    }

    // As in ARM, a right shift by zero means by thirty-two, and a left shift by
    // zero is the plain move it looks like.
    if (instruction.shift == ShiftKind::LogicalLeft) {
        instruction.operand = OperandForm::Register;
        return instruction;
    }
    instruction.shift_amount = FullShift;
    return instruction;
}

Instruction DecodeAddSubtractRegister(std::uint16_t first) {
    const bool immediate = Bit(first, 10);
    const bool subtract = Bit(first, 9);

    Instruction instruction = Start(first, subtract ? Operation::Sub : Operation::Add);
    instruction.destination = LowRegisterAt(first, 0);
    instruction.first = LowRegisterAt(first, 3);

    if (immediate) {
        instruction.operand = OperandForm::Immediate;
        instruction.immediate = Bits(first, 8, 6);
    } else {
        instruction.operand = OperandForm::Register;
        instruction.second = LowRegisterAt(first, 6);
    }
    return instruction;
}

Instruction DecodeImmediateOperation(std::uint16_t first) {
    static constexpr std::array<Operation, 4> Operations = {
        Operation::Mov, Operation::Cmp, Operation::Add, Operation::Sub};

    Instruction instruction = Start(first, Operations.at(Bits(first, 12, 11)));
    instruction.operand = OperandForm::Immediate;
    instruction.immediate = Bits(first, 7, 0);

    const RegisterIndex register_index = LowRegisterAt(first, 8);
    if (instruction.operation == Operation::Cmp) {
        instruction.first = register_index;
    } else if (instruction.operation == Operation::Mov) {
        instruction.destination = register_index;
    } else {
        instruction.destination = register_index;
        instruction.first = register_index;
    }
    return instruction;
}

// The sixteen operations that work on two low registers. Four of them are
// shifts, which arrive as a move with a shift by a register, and one is a reverse
// subtract from zero that the assembler spells as a negation.
Instruction DecodeRegisterOperation(std::uint16_t first) {
    const std::uint32_t opcode = Bits(first, 9, 6);
    const RegisterIndex destination = LowRegisterAt(first, 0);
    const RegisterIndex source = LowRegisterAt(first, 3);

    static constexpr std::array<Operation, 16> Operations = {Operation::And,
                                                             Operation::Eor,
                                                             Operation::Mov,
                                                             Operation::Mov,
                                                             Operation::Mov,
                                                             Operation::Adc,
                                                             Operation::Sbc,
                                                             Operation::Mov,
                                                             Operation::Tst,
                                                             Operation::Rsb,
                                                             Operation::Cmp,
                                                             Operation::Cmn,
                                                             Operation::Orr,
                                                             Operation::Mul,
                                                             Operation::Bic,
                                                             Operation::Mvn};

    static constexpr std::array<ShiftKind, 16> Shifts = {ShiftKind::LogicalLeft,
                                                         ShiftKind::LogicalLeft,
                                                         ShiftKind::LogicalLeft,
                                                         ShiftKind::LogicalRight,
                                                         ShiftKind::ArithmeticRight,
                                                         ShiftKind::LogicalLeft,
                                                         ShiftKind::LogicalLeft,
                                                         ShiftKind::RotateRight,
                                                         ShiftKind::LogicalLeft,
                                                         ShiftKind::LogicalLeft,
                                                         ShiftKind::LogicalLeft,
                                                         ShiftKind::LogicalLeft,
                                                         ShiftKind::LogicalLeft,
                                                         ShiftKind::LogicalLeft,
                                                         ShiftKind::LogicalLeft,
                                                         ShiftKind::LogicalLeft};

    Instruction instruction = Start(first, Operations.at(opcode));
    instruction.second = source;

    const bool is_shift =
        opcode == 0b0010 || opcode == 0b0011 || opcode == 0b0100 || opcode == 0b0111;
    if (is_shift) {
        instruction.destination = destination;
        instruction.second = destination;
        instruction.shift = Shifts.at(opcode);
        instruction.shift_register = source;
        instruction.operand = OperandForm::ShiftedRegister;
        return instruction;
    }

    switch (instruction.operation) {
    case Operation::Tst:
    case Operation::Cmp:
    case Operation::Cmn:
        instruction.first = destination;
        instruction.operand = OperandForm::Register;
        return instruction;
    case Operation::Mvn:
        instruction.destination = destination;
        instruction.operand = OperandForm::Register;
        return instruction;
    case Operation::Rsb:
        // Negation: a reverse subtract from an immediate zero.
        instruction.destination = destination;
        instruction.first = source;
        instruction.second = NoRegister;
        instruction.operand = OperandForm::Immediate;
        instruction.immediate = 0;
        return instruction;
    case Operation::Mul:
        // The destination is also the second factor. The fields are those of an
        // ARM MUL, Rm in `second` and Rs in `third`, so that whatever executes a
        // multiply reads both encodings the same way.
        instruction.destination = destination;
        instruction.third = destination;
        return instruction;
    default:
        break;
    }

    instruction.destination = destination;
    instruction.first = destination;
    instruction.operand = OperandForm::Register;
    return instruction;
}

// The three operations that can reach the high registers, and the exchanging
// branches, which share a row because they all needed a fourth register bit.
Instruction DecodeHighRegisterOperation(std::uint16_t first) {
    const std::uint32_t opcode = Bits(first, 9, 8);

    if (opcode == 0b11) {
        // The three low bits are fixed, and a linking exchange cannot take the
        // program counter as its target: it would need to write the return
        // address into the register it is about to branch through.
        const auto target = static_cast<RegisterIndex>(Bits(first, 6, 3));
        const bool links = Bit(first, 7);
        if (Bits(first, 2, 0) != 0 || (links && target == ProgramCounter)) {
            return Undefined(first);
        }

        Instruction instruction =
            Start(first, links ? Operation::BranchLinkExchangeRegister : Operation::BranchExchange);
        instruction.updates_flags = false;
        instruction.second = target;
        return instruction;
    }

    const auto destination =
        static_cast<RegisterIndex>(Bits(first, 2, 0) | (Bit(first, 7) ? 8U : 0U));
    const auto source = static_cast<RegisterIndex>(Bits(first, 6, 3));

    static constexpr std::array<Operation, 3> Operations = {
        Operation::Add, Operation::Cmp, Operation::Mov};

    Instruction instruction = Start(first, Operations.at(opcode));
    instruction.second = source;
    instruction.operand = OperandForm::Register;

    // Only the comparison sets the flags. The other two were given the high
    // registers instead, which is why a Thumb add into a high register cannot be
    // followed by a conditional branch on its result.
    instruction.updates_flags = instruction.operation == Operation::Cmp;

    if (instruction.operation == Operation::Cmp) {
        instruction.first = destination;
        return instruction;
    }
    instruction.destination = destination;
    if (instruction.operation == Operation::Add) {
        instruction.first = destination;
    }
    return instruction;
}

Instruction DecodeLiteralLoad(std::uint16_t first) {
    Instruction instruction = Start(first, Operation::LoadWord);
    instruction.updates_flags = false;
    instruction.destination = LowRegisterAt(first, 8);
    instruction.first = ProgramCounter;
    instruction.preindexed = true;
    instruction.operand = OperandForm::Immediate;
    instruction.immediate = Bits(first, 7, 0) * WordScale;
    return instruction;
}

Instruction DecodeRegisterTransfer(std::uint16_t first) {
    static constexpr std::array<Operation, 8> Operations = {Operation::StoreWord,
                                                            Operation::StoreHalf,
                                                            Operation::StoreByte,
                                                            Operation::LoadSignedByte,
                                                            Operation::LoadWord,
                                                            Operation::LoadHalf,
                                                            Operation::LoadByte,
                                                            Operation::LoadSignedHalf};

    Instruction instruction = Start(first, Operations.at(Bits(first, 11, 9)));
    instruction.updates_flags = false;
    instruction.destination = LowRegisterAt(first, 0);
    instruction.first = LowRegisterAt(first, 3);
    instruction.second = LowRegisterAt(first, 6);
    instruction.preindexed = true;
    instruction.operand = OperandForm::Register;
    return instruction;
}

Instruction
DecodeImmediateTransfer(std::uint16_t first, Operation load, Operation store, unsigned scale) {
    Instruction instruction = Start(first, Bit(first, 11) ? load : store);
    instruction.updates_flags = false;
    instruction.destination = LowRegisterAt(first, 0);
    instruction.first = LowRegisterAt(first, 3);
    instruction.preindexed = true;
    instruction.operand = OperandForm::Immediate;
    instruction.immediate = Bits(first, 10, 6) * scale;
    return instruction;
}

Instruction DecodeStackTransfer(std::uint16_t first) {
    Instruction instruction =
        Start(first, Bit(first, 11) ? Operation::LoadWord : Operation::StoreWord);
    instruction.updates_flags = false;
    instruction.destination = LowRegisterAt(first, 8);
    instruction.first = StackPointer;
    instruction.preindexed = true;
    instruction.operand = OperandForm::Immediate;
    instruction.immediate = Bits(first, 7, 0) * WordScale;
    return instruction;
}

// Taking the address of something: a sum of the program counter or the stack
// pointer and a small constant. The first form is how Thumb code gets the
// address of a literal pool entry rather than its contents.
Instruction DecodeAddressOf(std::uint16_t first) {
    Instruction instruction = Start(first, Operation::Add);
    instruction.updates_flags = false;
    instruction.destination = LowRegisterAt(first, 8);
    instruction.first = Bit(first, 11) ? StackPointer : ProgramCounter;
    instruction.operand = OperandForm::Immediate;
    instruction.immediate = Bits(first, 7, 0) * WordScale;
    return instruction;
}

Instruction DecodeBlockTransfer(std::uint16_t first) {
    Instruction instruction =
        Start(first, Bit(first, 11) ? Operation::LoadMultiple : Operation::StoreMultiple);
    instruction.updates_flags = false;
    instruction.first = LowRegisterAt(first, 8);
    instruction.registers = static_cast<RegisterSet>(Bits(first, 7, 0));
    instruction.add_offset = true;
    instruction.writes_back = true;

    if (instruction.registers == 0) {
        return Undefined(first);
    }
    return instruction;
}

// Pushing and popping, which are a block transfer through the stack pointer with
// one register the encoding names by a single bit: the link register going down
// and the program counter coming back. That is what makes a pop a return.
Instruction DecodeStackBlockTransfer(std::uint16_t first) {
    const bool load = Bit(first, 11);
    const bool extra = Bit(first, 8);

    Instruction instruction =
        Start(first, load ? Operation::LoadMultiple : Operation::StoreMultiple);
    instruction.updates_flags = false;
    instruction.first = StackPointer;
    instruction.registers = static_cast<RegisterSet>(Bits(first, 7, 0));
    instruction.writes_back = true;
    instruction.preindexed = !load;
    instruction.add_offset = load;

    if (extra) {
        instruction.registers = static_cast<RegisterSet>(
            instruction.registers | (1U << (load ? ProgramCounter : LinkRegister)));
    }
    if (instruction.registers == 0) {
        return Undefined(first);
    }
    return instruction;
}

Instruction DecodeAdjustStack(std::uint16_t first) {
    Instruction instruction = Start(first, Bit(first, 7) ? Operation::Sub : Operation::Add);
    instruction.updates_flags = false;
    instruction.destination = StackPointer;
    instruction.first = StackPointer;
    instruction.operand = OperandForm::Immediate;
    instruction.immediate = Bits(first, 6, 0) * WordScale;
    return instruction;
}

Instruction DecodeExtend(std::uint16_t first) {
    static constexpr std::array<Operation, 4> Operations = {
        Operation::Sxth, Operation::Sxtb, Operation::Uxth, Operation::Uxtb};

    Instruction instruction = Start(first, Operations.at(Bits(first, 7, 6)));
    instruction.updates_flags = false;
    instruction.destination = LowRegisterAt(first, 0);
    instruction.second = LowRegisterAt(first, 3);
    return instruction;
}

Instruction DecodeReverse(std::uint16_t first) {
    const std::uint32_t kind = Bits(first, 7, 6);
    if (kind == 0b10) {
        return Undefined(first);
    }

    static constexpr std::array<Operation, 4> Operations = {
        Operation::Rev, Operation::Rev16, Operation::Undefined, Operation::Revsh};

    Instruction instruction = Start(first, Operations.at(kind));
    instruction.updates_flags = false;
    instruction.destination = LowRegisterAt(first, 0);
    instruction.second = LowRegisterAt(first, 3);
    return instruction;
}

Instruction DecodeThumbHint(std::uint16_t first) {
    if (Bits(first, 3, 0) != 0) {
        return Undefined(first);
    }

    static constexpr std::array<Operation, 5> Operations = {Operation::NoOperation,
                                                            Operation::Yield,
                                                            Operation::WaitForEvent,
                                                            Operation::WaitForInterrupt,
                                                            Operation::SendEvent};

    const std::uint32_t which = Bits(first, 7, 4);
    if (which >= Operations.size()) {
        return Undefined(first);
    }

    Instruction instruction = Start(first, Operations.at(which));
    instruction.updates_flags = false;
    return instruction;
}

// The corner of the encoding that holds everything that did not fit elsewhere:
// adjusting the stack, extending and reversing, pushing and popping, the
// breakpoint, the hints, and the two instructions that change processor state.
Instruction DecodeMiscellaneous(std::uint16_t first) {
    const std::uint32_t row = Bits(first, 11, 8);

    switch (row) {
    case 0b0000:
        return DecodeAdjustStack(first);
    case 0b0010:
        return DecodeExtend(first);
    case 0b1010:
        return DecodeReverse(first);
    case 0b1110: {
        Instruction instruction = Start(first, Operation::Breakpoint);
        instruction.updates_flags = false;
        instruction.immediate = Bits(first, 7, 0);
        return instruction;
    }
    case 0b1111:
        return DecodeThumbHint(first);
    case 0b0110: {
        // Setting the byte order and changing the interrupt mask, told apart by
        // one bit, with the rest of each fixed.
        if (Bits(first, 7, 4) == 0b0101 && Bits(first, 2, 0) == 0) {
            Instruction instruction = Start(first, Operation::SetEndianness);
            instruction.updates_flags = false;
            instruction.immediate = Bit(first, 3) ? 1 : 0;
            return instruction;
        }
        if (Bits(first, 7, 5) == 0b011 && !Bit(first, 3)) {
            Instruction instruction = Start(first, Operation::ChangeProcessorState);
            instruction.updates_flags = false;
            instruction.immediate = Bits(first, 2, 0);
            return instruction;
        }
        return Undefined(first);
    }
    default:
        break;
    }

    if (Bits(first, 10, 9) == 0b10) {
        return DecodeStackBlockTransfer(first);
    }

    return Undefined(first);
}

Instruction DecodeConditionalBranch(std::uint16_t first) {
    const std::uint32_t condition = Bits(first, 11, 8);

    if (condition == 0b1111) {
        Instruction instruction = Start(first, Operation::SoftwareInterrupt);
        instruction.updates_flags = false;
        instruction.immediate = Bits(first, 7, 0);
        return instruction;
    }
    if (condition == 0b1110) {
        // Permanently undefined, which is what a debugger uses for a breakpoint
        // it does not want the architecture's one for.
        return Undefined(first);
    }

    Instruction instruction = Start(first, Operation::Branch);
    instruction.updates_flags = false;
    instruction.condition = static_cast<Condition>(condition);
    instruction.branch_offset = SignExtend(Bits(first, 7, 0), 8) * HalfwordStep;
    return instruction;
}

// The branch with link, the one instruction in ARMv6K's Thumb that is four bytes
// long. The first halfword carries the top of the displacement and the second
// the bottom, and which of the two forms it is depends on the second halfword:
// staying in Thumb, or exchanging into ARM.
Instruction DecodeBranchWithLink(std::uint16_t first, std::uint16_t second) {
    const std::uint32_t suffix = Bits(second, 15, 11);
    const bool stays_in_thumb = suffix == 0b11111;
    const bool exchanges = suffix == 0b11101;

    if (!stays_in_thumb && !exchanges) {
        return Undefined(first);
    }

    // The target of an exchange has to be word aligned, and the bit that would
    // make it otherwise is part of the displacement.
    if (exchanges && Bit(second, 0)) {
        return Undefined(first);
    }

    Instruction instruction = Start(
        first, stays_in_thumb ? Operation::BranchLink : Operation::BranchLinkExchangeImmediate);
    instruction.updates_flags = false;
    instruction.width = 4;
    instruction.encoding = (static_cast<std::uint32_t>(second) << 16U) | first;

    const std::uint32_t high = Bits(first, 10, 0);
    const std::uint32_t low = Bits(second, 10, 0);
    instruction.branch_offset = SignExtend((high << 12U) | (low << 1U), 23);
    return instruction;
}

}  // namespace

Instruction DecodeThumb(std::uint16_t first, std::uint16_t second) {
    switch (Bits(first, 15, 13)) {
    case 0b000:
        if (Bits(first, 12, 11) == 0b11) {
            return DecodeAddSubtractRegister(first);
        }
        return DecodeShiftImmediate(first);
    case 0b001:
        return DecodeImmediateOperation(first);
    case 0b010:
        if (Bits(first, 12, 10) == 0b000) {
            return DecodeRegisterOperation(first);
        }
        if (Bits(first, 12, 10) == 0b001) {
            return DecodeHighRegisterOperation(first);
        }
        if (Bit(first, 12)) {
            return DecodeRegisterTransfer(first);
        }
        return DecodeLiteralLoad(first);
    case 0b011:
        if (Bit(first, 12)) {
            return DecodeImmediateTransfer(first, Operation::LoadByte, Operation::StoreByte, 1);
        }
        return DecodeImmediateTransfer(first, Operation::LoadWord, Operation::StoreWord, WordScale);
    case 0b100:
        if (!Bit(first, 12)) {
            return DecodeImmediateTransfer(
                first, Operation::LoadHalf, Operation::StoreHalf, HalfwordScale);
        }
        return DecodeStackTransfer(first);
    case 0b101:
        if (!Bit(first, 12)) {
            return DecodeAddressOf(first);
        }
        return DecodeMiscellaneous(first);
    case 0b110:
        if (!Bit(first, 12)) {
            return DecodeBlockTransfer(first);
        }
        return DecodeConditionalBranch(first);
    case 0b111:
        switch (Bits(first, 12, 11)) {
        case 0b00: {
            Instruction instruction = Start(first, Operation::Branch);
            instruction.updates_flags = false;
            instruction.branch_offset = SignExtend(Bits(first, 10, 0), 11) * HalfwordStep;
            return instruction;
        }
        case 0b10:
            return DecodeBranchWithLink(first, second);
        default:
            // The halfwords that complete a branch with link, met on their own.
            // ARMv6T2 gave this space the 32-bit instructions; this is not that
            // architecture.
            return Undefined(first);
        }
    default:
        break;
    }

    return Undefined(first);
}

}  // namespace hexlith::domain::arm
