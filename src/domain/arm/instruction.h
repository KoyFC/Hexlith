#ifndef HEXLITH_DOMAIN_ARM_INSTRUCTION_H
#define HEXLITH_DOMAIN_ARM_INSTRUCTION_H

#include "domain/arm/condition.h"
#include "domain/arm/instruction_set.h"
#include "domain/arm/operation.h"
#include "domain/arm/registers.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hexlith::domain::arm {

enum class ShiftKind : std::uint8_t {
    LogicalLeft,
    LogicalRight,
    ArithmeticRight,
    RotateRight,

    // Rotate right by one through the carry flag, which the encoding spells as
    // a rotate right by zero.
    RotateRightExtended,
};

std::string_view ShiftName(ShiftKind kind);

// How the operand that is not a plain register is formed: the second operand of
// a data-processing instruction, or the offset of a transfer.
enum class OperandForm : std::uint8_t { None, Immediate, Register, ShiftedRegister };

// What a parallel instruction does to each lane, and how it treats the lanes'
// signs. The forty-eight encodings are six of these prefixes over the six lane
// shapes below, and an instruction is one of each.
//
// The prefix decides the arithmetic: modular, saturating or halving, signed or
// unsigned. Modular arithmetic is the only kind that sets the GE flags, which is
// what SEL reads afterwards.
enum class ParallelPrefix : std::uint8_t {
    Signed,
    SignedSaturating,
    SignedHalving,
    Unsigned,
    UnsignedSaturating,
    UnsignedHalving,
};

// The lanes and what happens in each. The two exchanging shapes pair the high
// half of one source with the low half of the other. ARMv6 spells them ADDSUBX
// and SUBADDX; ARMv7 renamed them ASX and SAX, which is what an assembler and a
// disassembler print today and what this project prints too.
enum class ParallelShape : std::uint8_t {
    Add16,
    AddSubtractExchange,
    SubtractAddExchange,
    Subtract16,
    Add8,
    Subtract8,
};

// The assembler's name for a parallel instruction, prefix and shape together,
// such as uqadd8 or shasx.
std::string ParallelMnemonic(ParallelPrefix prefix, ParallelShape shape);

// One decoded instruction.
//
// A flat structure with a field per operand rather than a variant per
// instruction class. Two reasons, and the first is the one that decides it:
// ARCHITECTURE.md requires the hot paths to avoid per-instruction virtual
// dispatch and work over flat data, and this is the hottest path there is. The
// second is that the fields an instruction does not use cost nothing to carry
// and a great deal to reach through a variant.
//
// A field only means something for the operations that use it. The register
// fields hold NoRegister when they are not used, so a reader can tell a field
// that was filled in from one that was left alone.
struct Instruction {
    // The word, or halfword, exactly as it was read. Kept so that an operation
    // this project does not model yet can still be identified later, and so
    // that a disagreement with another disassembler can be reported with the
    // encoding that caused it.
    std::uint32_t encoding = 0;

    Operation operation = Operation::Undefined;
    Condition condition = Condition::Always;
    InstructionSet instruction_set = InstructionSet::Arm;

    // In bytes: four in ARM, two or four in Thumb.
    std::uint8_t width = 4;

    // Rd, and the second destination of a long multiply.
    RegisterIndex destination = NoRegister;
    RegisterIndex destination_high = NoRegister;

    // Rn: the first operand of an ALU operation, the base of a transfer.
    RegisterIndex first = NoRegister;

    // Rm: the register operand.
    RegisterIndex second = NoRegister;

    // Rs: the third operand of a multiply.
    RegisterIndex third = NoRegister;

    OperandForm operand = OperandForm::None;

    // The immediate, already rotated for a data-processing operand and already
    // scaled where the encoding scales it. Also the comment field of a software
    // interrupt or a breakpoint, and the bound of a saturating operation.
    std::uint32_t immediate = 0;

    ShiftKind shift = ShiftKind::LogicalLeft;
    std::uint8_t shift_amount = 0;
    RegisterIndex shift_register = NoRegister;

    // The registers a block transfer moves, bit n for register n.
    RegisterSet registers = 0;

    // The branch displacement, signed and already scaled, counted from the
    // address the program counter reads rather than from the instruction.
    std::int32_t branch_offset = 0;

    // The S bit: the operation updates the condition flags.
    bool updates_flags = false;

    // The P bit: the offset is applied before the transfer rather than after.
    bool preindexed = false;

    // The U bit: the offset is added rather than subtracted.
    bool add_offset = true;

    // The W bit: the base register keeps the address the offset produced.
    bool writes_back = false;

    // The R bit of a status register access, and the S bit of a block transfer:
    // the saved status register rather than the current one, and the user mode
    // bank rather than the current one.
    bool saved_status = false;

    // Which half of each source a halfword multiply reads.
    bool left_half_high = false;
    bool right_half_high = false;

    // The coprocessor an instruction in the coprocessor space reaches. On the
    // ARM11 that is mostly CP15, the system control coprocessor.
    std::uint8_t coprocessor = 0;
    std::uint8_t coprocessor_opcode1 = 0;
    std::uint8_t coprocessor_opcode2 = 0;

    // The arithmetic and the lanes of a parallel instruction.
    ParallelPrefix parallel_prefix = ParallelPrefix::Signed;
    ParallelShape parallel_shape = ParallelShape::Add16;
};

// Whether the instruction can be reached as a value: whether it decoded at all.
[[nodiscard]] constexpr bool IsDefined(const Instruction& instruction) {
    return instruction.operation != Operation::Undefined;
}

// Whether the operation writes its destination register, which is what makes a
// destination of r15 a branch.
[[nodiscard]] bool WritesDestination(Operation operation);

// Whether the instruction can change the program counter, by any route: a
// branch, a destination of r15, a block transfer holding r15, or a return from
// exception.
//
// This is the question the whole of control flow analysis is built on, and
// getting it wrong is expensive in a particular way. A missed program counter
// write makes the analysis walk off the end of a function into whatever follows
// it and call that code too.
[[nodiscard]] bool WritesProgramCounter(const Instruction& instruction);

// Whether the branch target is known from the instruction alone, which is true
// for a displacement and false for anything through a register.
[[nodiscard]] bool HasKnownTarget(const Instruction& instruction);

// The address a branch with a displacement reaches, given where the branch is.
//
// The displacement counts from the value the program counter reads, which is
// two instructions ahead in ARM and one in Thumb, because that is what the
// pipeline made visible when the encoding was designed. Every target
// computation in the project goes through here rather than adding eight
// somewhere of its own.
[[nodiscard]] std::uint32_t BranchTarget(const Instruction& instruction, std::uint32_t address);

// Whether the instruction calls: it transfers control and leaves a return
// address in the link register.
[[nodiscard]] bool IsCall(const Instruction& instruction);

// Whether the instruction returns from a function, as far as the instruction
// alone can say: an exchange to the link register, a move of the link register
// into the program counter, or a stack pop that loads it.
[[nodiscard]] bool IsReturn(const Instruction& instruction);

// Whether the instruction that follows this one in memory can be reached from
// it. False only for an unconditional transfer of control, since a conditional
// one leaves both paths open.
[[nodiscard]] bool ContinuesToTheNextInstruction(const Instruction& instruction);

// The address a transfer relative to the program counter reads from, when there
// is one.
//
// This is how a 32-bit constant gets into a register on a machine whose
// instructions are 32 bits wide: the constant sits in a pool near the code and
// is loaded from it. For the analysis that makes it two things at once, a word
// of data inside a function and, when the value is an address, the only
// evidence that the address is used.
[[nodiscard]] std::optional<std::uint32_t>
ProgramCounterRelativeAddress(const Instruction& instruction, std::uint32_t address);

}  // namespace hexlith::domain::arm

#endif  // HEXLITH_DOMAIN_ARM_INSTRUCTION_H
