#include "domain/lift/lower.h"

#include "domain/arm/primitives/branch.h"
#include "domain/arm/primitives/media.h"
#include "domain/arm/primitives/transfer.h"
#include "domain/arm/primitives/unpredictable.h"

#include <array>
#include <bit>

namespace hexlith::domain::lift {
namespace {

namespace arm = domain::arm;
namespace primitives = arm::primitives;
using ir::Flag;
using ir::Op;
using ir::Operand;
using ir::OpKind;
using ir::Type;

constexpr std::uint32_t WordMask = ~3U;
constexpr std::uint32_t HalfwordMask = ~1U;
constexpr std::uint32_t LowTwoBits = 3U;
constexpr std::uint32_t LowBit = 1U;
constexpr std::uint32_t HalfwordBit = 2U;
constexpr std::uint32_t ByteMask = 0xFFU;

// The operations whose result the flags see as arithmetic: V is theirs to set.
bool IsArithmetic(arm::Operation operation) {
    switch (operation) {
    case arm::Operation::Sub:
    case arm::Operation::Rsb:
    case arm::Operation::Add:
    case arm::Operation::Adc:
    case arm::Operation::Sbc:
    case arm::Operation::Rsc:
    case arm::Operation::Cmp:
    case arm::Operation::Cmn:
        return true;
    default:
        return false;
    }
}

// The operations HandledByTheInterpreter takes. The survey of what lifted code
// left to the interpreter, weighed by a run, put these at the top.
bool IsHandledByTheInterpreter(arm::Operation operation) {
    using arm::Operation;
    if (arm::IsFloatingPoint(operation)) {
        return true;
    }
    switch (operation) {
    case Operation::LoadExclusive:
    case Operation::StoreExclusive:
    case Operation::LoadExclusiveByte:
    case Operation::StoreExclusiveByte:
    case Operation::LoadExclusiveHalf:
    case Operation::StoreExclusiveHalf:
    case Operation::LoadExclusiveDouble:
    case Operation::StoreExclusiveDouble:
    case Operation::ClearExclusive:
    case Operation::LoadDouble:
    case Operation::StoreDouble:
    case Operation::Umull:
    case Operation::Umlal:
    case Operation::Umaal:
    case Operation::Smull:
    case Operation::Smlal:
    case Operation::SmulHalf:
    case Operation::SmlaHalf:
    case Operation::SmlalHalf:
    case Operation::SmulwHalf:
    case Operation::SmlawHalf:
    case Operation::CountLeadingZeros:
        return true;
    default:
        return false;
    }
}

class Lowerer {
public:
    explicit Lowerer(const PlacedInstruction& placed)
        : instruction_(placed.instruction),
          address_(placed.address),
          thumb_(placed.instruction.instruction_set == arm::InstructionSet::Thumb) {
        out_.address = placed.address;
        out_.width = placed.instruction.width;
        out_.instruction_set = placed.instruction.instruction_set;
        out_.condition = instruction_.condition == arm::Condition::Unconditional
                             ? arm::Condition::Always
                             : instruction_.condition;
    }

    ir::Instruction Run() {
        if (arm::IsDefined(instruction_) && primitives::HasUnpredictableOperands(instruction_)) {
            return Fallback();
        }
        if (!Translate()) {
            return Fallback();
        }
        out_.temporaries = temporaries_;
        return std::move(out_);
    }

private:
    // Everything the interpreter does for this instruction, including checking
    // its condition and moving on.
    [[nodiscard]] ir::Instruction Fallback() const {
        ir::Instruction fallback;
        fallback.address = address_;
        fallback.width = out_.width;
        fallback.instruction_set = out_.instruction_set;
        fallback.condition = arm::Condition::Always;
        fallback.ops.push_back(Op{.kind = OpKind::Interpret});
        return fallback;
    }

    // Whether the class of instruction is translated. False sends it to the
    // interpreter.
    bool Translate() {
        using arm::Operation;
        switch (instruction_.operation) {
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
            DataProcessing();
            return true;
        case Operation::Mul:
        case Operation::Mla:
            Multiply();
            return true;
        case Operation::LoadWord:
        case Operation::LoadByte:
        case Operation::LoadWordUnprivileged:
        case Operation::LoadByteUnprivileged:
        case Operation::LoadHalf:
        case Operation::LoadSignedByte:
        case Operation::LoadSignedHalf:
            Load();
            return true;
        case Operation::StoreWord:
        case Operation::StoreByte:
        case Operation::StoreWordUnprivileged:
        case Operation::StoreByteUnprivileged:
        case Operation::StoreHalf:
            Store();
            return true;
        case Operation::LoadMultiple:
            LoadMultiple();
            return true;
        case Operation::StoreMultiple:
            StoreMultiple();
            return true;
        case Operation::Sxtab:
        case Operation::Sxtah:
        case Operation::Sxtb:
        case Operation::Sxth:
        case Operation::Uxtab:
        case Operation::Uxtah:
        case Operation::Uxtb:
        case Operation::Uxth:
            Extension();
            return true;
        case Operation::Branch:
            out_.branch_target = arm::BranchTarget(instruction_, address_);
            return true;
        case Operation::BranchLink:
            WriteLink();
            out_.branch_target = arm::BranchTarget(instruction_, address_);
            return true;
        case Operation::BranchLinkExchangeImmediate:
            WriteLink();
            Emit(OpKind::WriteThumb, {.a = Operand::Bit(!thumb_)});
            out_.branch_target = arm::BranchTarget(instruction_, address_);
            return true;
        case Operation::BranchExchange:
            ExchangeTo(Read(instruction_.second));
            return true;
        case Operation::BranchLinkExchangeRegister: {
            // The target is read before the link register is written, because the
            // target may be the link register.
            const Operand target = Read(instruction_.second);
            ExchangeTo(target);
            WriteLink();
            return true;
        }
        case Operation::NoOperation:
        case Operation::Yield:
        case Operation::WaitForEvent:
        case Operation::WaitForInterrupt:
        case Operation::SendEvent:
        case Operation::Preload:
            return true;
        case Operation::MoveFromCoprocessor:
        case Operation::MoveToCoprocessor:
            return ThreadIdAccess();
        default:
            return HandledByTheInterpreter();
        }
    }

    // An instruction whose handler in the interpreter does the work, called
    // directly with the instruction as a constant: floating point, the
    // exclusive pair, the doubleword transfers, the long and halfword
    // multiplies and the count of leading zeros. Not one that writes r15, since
    // the handler leaves moving r15 to its caller.
    bool HandledByTheInterpreter() {
        if (!IsHandledByTheInterpreter(instruction_.operation) ||
            arm::WritesProgramCounter(instruction_)) {
            return false;
        }
        Emit(OpKind::Execute, {});
        return true;
    }

    // --- Emitting ----------------------------------------------------------------

    struct Operands {
        std::uint32_t index = 0;
        Operand a{};
        Operand b{};
        Operand c{};
        Operand d{};
    };

    void Emit(OpKind kind, const Operands& operands) {
        out_.ops.push_back(Op{.kind = kind,
                              .index = operands.index,
                              .destination = ir::NoTemporary,
                              .destination2 = ir::NoTemporary,
                              .destination3 = ir::NoTemporary,
                              .a = operands.a,
                              .b = operands.b,
                              .c = operands.c,
                              .d = operands.d});
    }

    Operand NewTemporary(Type type) {
        return Operand::Temporary(temporaries_++, type);
    }

    // An operation with one result.
    Operand Produce(OpKind kind, Type type, const Operands& operands) {
        const Operand result = NewTemporary(type);
        Emit(kind, operands);
        out_.ops.back().destination = result.value;
        return result;
    }

    Operand Binary(OpKind kind, Operand a, Operand b) {
        return Produce(kind, Type::Word, {.a = a, .b = b});
    }

    Operand Compare(OpKind kind, Operand a, Operand b) {
        return Produce(kind, Type::Bit, {.a = a, .b = b});
    }

    Operand ReadFlag(Flag flag) {
        return Produce(OpKind::ReadFlag, Type::Bit, {.index = static_cast<std::uint32_t>(flag)});
    }

    void WriteFlag(Flag flag, Operand value) {
        Emit(OpKind::WriteFlag, {.index = static_cast<std::uint32_t>(flag), .a = value});
    }

    // The value an instruction reads from a register. r15 reads as the
    // instruction's address plus the pipeline offset, which the lifter knows.
    Operand Read(arm::RegisterIndex index) {
        if (index == arm::ProgramCounter) {
            return Operand::Word(primitives::ReadProgramCounter(address_, thumb_));
        }
        return Produce(OpKind::ReadRegister, Type::Word, {.index = index});
    }

    // The same, rounded down to a word in Thumb, as a literal address is.
    Operand ReadAligned(arm::RegisterIndex index) {
        if (index == arm::ProgramCounter) {
            return Operand::Word(primitives::ReadAlignedProgramCounter(address_, thumb_));
        }
        return Read(index);
    }

    void Write(arm::RegisterIndex index, Operand value) {
        Emit(OpKind::WriteRegister, {.index = index, .a = value});
    }

    void WriteLink() {
        const std::uint32_t link = (address_ + instruction_.width) | (thumb_ ? 1U : 0U);
        Write(arm::LinkRegister, Operand::Word(link));
    }

    // --- Branches ---------------------------------------------------------------

    // BranchWritePC: the target with the bits below the instruction size
    // dropped, in the instruction set the processor is in.
    void BranchWrite(Operand target) {
        const std::uint32_t mask = thumb_ ? HalfwordMask : WordMask;
        Write(arm::ProgramCounter, Binary(OpKind::And, target, Operand::Word(mask)));
    }

    // BXWritePC: the low bit of the target chooses the instruction set, and an
    // ARM target that is only halfword aligned is unpredictable.
    void ExchangeTo(Operand target) {
        const Operand low = Binary(OpKind::And, target, Operand::Word(LowTwoBits));
        Emit(OpKind::BailIf, {.a = Compare(OpKind::Equal, low, Operand::Word(HalfwordBit))});

        const Operand to_thumb = Compare(
            OpKind::NotEqual, Binary(OpKind::And, target, Operand::Word(LowBit)), Operand::Word(0));
        Emit(OpKind::WriteThumb, {.a = to_thumb});
        const Operand mask = Produce(
            OpKind::Select,
            Type::Word,
            {.a = to_thumb, .b = Operand::Word(HalfwordMask), .c = Operand::Word(WordMask)});
        Write(arm::ProgramCounter, Binary(OpKind::And, target, mask));
    }

    // --- Data processing --------------------------------------------------------

    struct Shifted {
        Operand value;
        Operand carry;
    };

    Shifted ShifterOperand() {
        switch (instruction_.operand) {
        case arm::OperandForm::Immediate: {
            // An ARM immediate rotated by a nonzero amount sets the carry from its
            // top bit. Thumb immediates are never rotated.
            const bool rotated = !thumb_ && ((instruction_.encoding >> 8U) & 0xFU) != 0;
            const Operand carry = rotated ? Operand::Bit((instruction_.immediate >> 31U) != 0)
                                          : ReadFlag(Flag::Carry);
            return {.value = Operand::Word(instruction_.immediate), .carry = carry};
        }
        case arm::OperandForm::Register:
            return {.value = Read(instruction_.second), .carry = ReadFlag(Flag::Carry)};
        case arm::OperandForm::ShiftedRegister: {
            const Operand amount = instruction_.shift_register == arm::NoRegister
                                       ? Operand::Word(instruction_.shift_amount)
                                       : Binary(OpKind::And,
                                                Read(instruction_.shift_register),
                                                Operand::Word(ByteMask));
            const Operand value = Read(instruction_.second);
            const Operand carry_in = ReadFlag(Flag::Carry);
            const Operand shifted = NewTemporary(Type::Word);
            const Operand carry = NewTemporary(Type::Bit);
            Emit(OpKind::Shift,
                 {.index = static_cast<std::uint32_t>(instruction_.shift),
                  .a = value,
                  .b = amount,
                  .c = carry_in});
            out_.ops.back().destination = shifted.value;
            out_.ops.back().destination2 = carry.value;
            return {.value = shifted, .carry = carry};
        }
        case arm::OperandForm::None:
            break;
        }
        return {.value = Operand::Word(0), .carry = ReadFlag(Flag::Carry)};
    }

    void DataProcessing() {
        // Thumb's `add rd, pc, #imm` takes an address, and reads r15 rounded down
        // to a word the way a literal load does.
        const bool takes_address = thumb_ && instruction_.operand == arm::OperandForm::Immediate;
        Operand first = Operand::Word(0);
        if (instruction_.first != arm::NoRegister) {
            first = takes_address ? ReadAligned(instruction_.first) : Read(instruction_.first);
        }
        const Shifted second = ShifterOperand();
        const Operand carry_in = ReadFlag(Flag::Carry);

        const Operand result = NewTemporary(Type::Word);
        const Operand carry = NewTemporary(Type::Bit);
        const Operand overflow = NewTemporary(Type::Bit);
        Emit(OpKind::Alu,
             {.index = static_cast<std::uint32_t>(instruction_.operation),
              .a = first,
              .b = second.value,
              .c = second.carry,
              .d = carry_in});
        out_.ops.back().destination = result.value;
        out_.ops.back().destination2 = carry.value;
        out_.ops.back().destination3 = overflow.value;

        if (instruction_.destination != arm::NoRegister) {
            if (instruction_.destination == arm::ProgramCounter) {
                ComputedBranch(result);
            } else {
                Write(instruction_.destination, result);
            }
        }

        if (instruction_.updates_flags) {
            WriteFlag(Flag::Negative, Produce(OpKind::Negative, Type::Bit, {.a = result}));
            WriteFlag(Flag::Zero, Compare(OpKind::Equal, result, Operand::Word(0)));
            WriteFlag(Flag::Carry, carry);
            if (IsArithmetic(instruction_.operation)) {
                WriteFlag(Flag::Overflow, overflow);
            }
        }
    }

    // What a data-processing instruction does when it writes r15: a plain branch
    // in both instruction sets, and an ARM target that is not word aligned is
    // unpredictable.
    void ComputedBranch(Operand target) {
        if (!thumb_) {
            const Operand low = Binary(OpKind::And, target, Operand::Word(LowTwoBits));
            Emit(OpKind::BailIf, {.a = Compare(OpKind::NotEqual, low, Operand::Word(0))});
        }
        BranchWrite(target);
    }

    void Multiply() {
        const Operand x = Read(instruction_.second);
        const Operand y = Read(instruction_.third);
        Operand product = Binary(OpKind::Multiply, x, y);
        if (instruction_.operation == arm::Operation::Mla) {
            product = Binary(OpKind::Add, product, Read(instruction_.first));
        }
        Write(instruction_.destination, product);
        if (instruction_.updates_flags) {
            WriteFlag(Flag::Negative, Produce(OpKind::Negative, Type::Bit, {.a = product}));
            WriteFlag(Flag::Zero, Compare(OpKind::Equal, product, Operand::Word(0)));
        }
    }

    // --- Single transfers -------------------------------------------------------

    struct Address {
        Operand address;
        Operand written_back;
    };

    Operand TransferOffset() {
        switch (instruction_.operand) {
        case arm::OperandForm::Immediate:
            return Operand::Word(instruction_.immediate);
        case arm::OperandForm::Register:
            return Read(instruction_.second);
        case arm::OperandForm::ShiftedRegister: {
            const Operand value = Read(instruction_.second);
            const Operand carry_in = ReadFlag(Flag::Carry);
            const Operand shifted = NewTemporary(Type::Word);
            const Operand carry = NewTemporary(Type::Bit);
            Emit(OpKind::Shift,
                 {.index = static_cast<std::uint32_t>(instruction_.shift),
                  .a = value,
                  .b = Operand::Word(instruction_.shift_amount),
                  .c = carry_in});
            out_.ops.back().destination = shifted.value;
            out_.ops.back().destination2 = carry.value;
            return shifted;
        }
        case arm::OperandForm::None:
            break;
        }
        return Operand::Word(0);
    }

    Address SingleAddress() {
        const Operand base = ReadAligned(instruction_.first);
        const Operand offset = TransferOffset();
        const Operand offset_address =
            Binary(instruction_.add_offset ? OpKind::Add : OpKind::Subtract, base, offset);
        return {.address = instruction_.preindexed ? offset_address : base,
                .written_back = offset_address};
    }

    void WriteBack(Operand value) {
        if (instruction_.writes_back) {
            Write(instruction_.first, value);
        }
    }

    void Load() {
        const Address where = SingleAddress();
        const auto width = static_cast<std::uint32_t>(primitives::WidthOf(instruction_.operation));
        const bool into_pc = instruction_.destination == arm::ProgramCounter;

        if (into_pc) {
            // A load into r15 from an address that is not word aligned is
            // unpredictable.
            const Operand low = Binary(OpKind::And, where.address, Operand::Word(LowTwoBits));
            Emit(OpKind::BailIf, {.a = Compare(OpKind::NotEqual, low, Operand::Word(0))});
        }

        const Operand value =
            Produce(OpKind::Load, Type::Word, {.index = width, .a = where.address});
        if (into_pc) {
            ExchangeTo(value);
            WriteBack(where.written_back);
            return;
        }
        WriteBack(where.written_back);
        Write(instruction_.destination, value);
    }

    void Store() {
        const Address where = SingleAddress();
        const auto width = static_cast<std::uint32_t>(primitives::WidthOf(instruction_.operation));
        const Operand value = Read(instruction_.destination);
        Emit(OpKind::Store, {.index = width, .a = where.address, .b = value});
        WriteBack(where.written_back);
    }

    // --- Block transfers --------------------------------------------------------

    // The address of the nth word of a block that starts at `start`.
    Operand WordAt(Operand start, unsigned n) {
        return n == 0 ? start : Binary(OpKind::Add, start, Operand::Word(n * primitives::WordSize));
    }

    // Where a block transfer starts, after bailing if that is not word aligned.
    // Returns the base as well, which the write-back needs.
    struct Block {
        Operand base;
        Operand start;
        std::uint32_t bytes = 0;
    };

    Block StartBlock() {
        Block block;
        block.base = Read(instruction_.first);
        block.bytes = static_cast<std::uint32_t>(std::popcount(instruction_.registers)) *
                      primitives::WordSize;
        // BlockStart, with the addressing mode known: the four of them are an
        // offset from the base.
        std::uint32_t before_offset = 0;
        if (instruction_.add_offset) {
            before_offset = instruction_.preindexed ? primitives::WordSize : 0;
        } else {
            before_offset = block.bytes - (instruction_.preindexed ? 0 : primitives::WordSize);
        }
        block.start = block.base;
        if (before_offset != 0) {
            block.start = Binary(instruction_.add_offset ? OpKind::Add : OpKind::Subtract,
                                 block.base,
                                 Operand::Word(before_offset));
        }
        const Operand low = Binary(OpKind::And, block.start, Operand::Word(LowTwoBits));
        Emit(OpKind::BailIf, {.a = Compare(OpKind::NotEqual, low, Operand::Word(0))});
        return block;
    }

    Operand BlockWrittenBack(const Block& block) {
        return Binary(instruction_.add_offset ? OpKind::Add : OpKind::Subtract,
                      block.base,
                      Operand::Word(block.bytes));
    }

    // LDM, and POP in Thumb. Every word is read before any register is written,
    // so that a fault leaves the registers alone.
    void LoadMultiple() {
        const Block block = StartBlock();
        std::array<Operand, arm::RegisterCount> loaded{};
        unsigned n = 0;
        for (arm::RegisterIndex index = 0; index < arm::RegisterCount; ++index) {
            if (arm::Holds(instruction_.registers, index)) {
                loaded.at(index) =
                    Produce(OpKind::Load,
                            Type::Word,
                            {.index = static_cast<std::uint32_t>(primitives::Width::Word),
                             .a = WordAt(block.start, n++)});
            }
        }

        if (arm::Holds(instruction_.registers, arm::ProgramCounter)) {
            ExchangeTo(loaded.at(arm::ProgramCounter));
        }
        const bool base_listed = arm::Holds(instruction_.registers, instruction_.first);
        if (instruction_.writes_back && !base_listed) {
            Write(instruction_.first, BlockWrittenBack(block));
        }
        for (arm::RegisterIndex index = 0; index < arm::ProgramCounter; ++index) {
            if (arm::Holds(instruction_.registers, index)) {
                Write(index, loaded.at(index));
            }
        }
    }

    // STM, and PUSH in Thumb. A store that faults part of the way has written the
    // words before it, as the interpreter's has; the interpreter re-executes the
    // instruction after the bail and writes them again, the same values.
    void StoreMultiple() {
        const Block block = StartBlock();
        unsigned n = 0;
        for (arm::RegisterIndex index = 0; index < arm::RegisterCount; ++index) {
            if (arm::Holds(instruction_.registers, index)) {
                Emit(OpKind::Store,
                     {.index = static_cast<std::uint32_t>(primitives::Width::Word),
                      .a = WordAt(block.start, n++),
                      .b = Read(index)});
            }
        }
        if (instruction_.writes_back) {
            Write(instruction_.first, BlockWrittenBack(block));
        }
    }

    // --- Extensions -------------------------------------------------------------

    // SXTB, UXTH and the rest, with and without an addend. The rotation is the
    // shift amount.
    void Extension() {
        const Operand source = Read(instruction_.second);
        const Operand rotated =
            Produce(OpKind::Shift,
                    Type::Word,
                    {.index = static_cast<std::uint32_t>(arm::ShiftKind::RotateRight),
                     .a = source,
                     .b = Operand::Word(instruction_.shift_amount),
                     .c = Operand::Bit(false)});
        const Operand addend =
            instruction_.first == arm::NoRegister ? Operand::Word(0) : Read(instruction_.first);
        const Operand result = Produce(OpKind::Extend,
                                       Type::Word,
                                       {.index = static_cast<std::uint32_t>(instruction_.operation),
                                        .a = rotated,
                                        .b = addend});
        Write(instruction_.destination, result);
    }

    // --- Coprocessor 15 ---------------------------------------------------------

    // The two thread ID registers, and the barriers the interpreter lets
    // complete without doing anything. Anything else in coprocessor space stays
    // with the interpreter.
    bool ThreadIdAccess() {
        constexpr std::uint8_t Coprocessor = 15;
        constexpr arm::RegisterIndex CacheOperations = 7;
        constexpr arm::RegisterIndex ThreadIds = 13;
        constexpr std::uint8_t ReadWriteOpcode = 2;
        constexpr std::uint8_t ReadOnlyOpcode = 3;

        if (instruction_.coprocessor != Coprocessor) {
            return false;
        }
        const bool reading = instruction_.operation == arm::Operation::MoveFromCoprocessor;

        if (!reading && instruction_.first == CacheOperations) {
            return IsBarrier();
        }
        if (instruction_.destination == arm::ProgramCounter) {
            return false;
        }
        const bool thread_id = instruction_.first == ThreadIds && instruction_.second == 0 &&
                               instruction_.coprocessor_opcode1 == 0;
        if (!thread_id) {
            return false;
        }
        const std::uint8_t opcode = instruction_.coprocessor_opcode2;
        if (opcode == ReadWriteOpcode) {
            if (reading) {
                const Operand value =
                    Produce(OpKind::ReadThreadId,
                            Type::Word,
                            {.index = static_cast<std::uint32_t>(ir::ThreadId::ReadWrite)});
                Write(instruction_.destination, value);
            } else {
                Emit(OpKind::WriteThreadId,
                     {.index = static_cast<std::uint32_t>(ir::ThreadId::ReadWrite),
                      .a = Read(instruction_.destination)});
            }
            return true;
        }
        if (opcode == ReadOnlyOpcode && reading) {
            const Operand value =
                Produce(OpKind::ReadThreadId,
                        Type::Word,
                        {.index = static_cast<std::uint32_t>(ir::ThreadId::ReadOnly)});
            Write(instruction_.destination, value);
            return true;
        }
        return false;
    }

    // The three c7 operations user mode may issue: draining the write buffer, a
    // data memory barrier and flushing the prefetch buffer.
    [[nodiscard]] bool IsBarrier() const {
        constexpr arm::RegisterIndex WriteBuffer = 10;
        constexpr arm::RegisterIndex PrefetchBuffer = 5;
        constexpr std::uint8_t Drain = 4;
        constexpr std::uint8_t MemoryBarrier = 5;
        constexpr std::uint8_t Flush = 4;
        if (instruction_.coprocessor_opcode1 != 0) {
            return false;
        }
        const arm::RegisterIndex crm = instruction_.second;
        const std::uint8_t opcode = instruction_.coprocessor_opcode2;
        return (crm == WriteBuffer && (opcode == Drain || opcode == MemoryBarrier)) ||
               (crm == PrefetchBuffer && opcode == Flush);
    }

    arm::Instruction instruction_;
    std::uint32_t address_;
    bool thumb_;
    ir::Instruction out_;
    std::uint32_t temporaries_ = 0;
};

}  // namespace

ir::Instruction Lower(const PlacedInstruction& placed) {
    return Lowerer{placed}.Run();
}

}  // namespace hexlith::domain::lift
