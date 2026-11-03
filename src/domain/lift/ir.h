#ifndef HEXLITH_DOMAIN_LIFT_IR_H
#define HEXLITH_DOMAIN_LIFT_IR_H

#include "domain/arm/condition.h"
#include "domain/arm/instruction_set.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hexlith::domain::lift::ir {

// The lifter's own intermediate representation: what one guest instruction does,
// as a short list of operations on the processor's state and on temporary values.
//
// It exists so that three things can be decided about an instruction without
// looking at its encoding again: what C++ to write for it, which condition flags
// nothing reads afterwards, and whether the translation is the same as the
// interpreter's. The last is why every operation here has an exact meaning in
// terms of the primitive library that the interpreter also calls, and why the
// evaluator in evaluate.h can run it: the translation from an instruction to
// these operations is the one new piece of logic in the lifter, and it is
// checked against the interpreter one instruction at a time.
//
// The form is a list per instruction and not a graph. A temporary is born and
// dies inside the instruction that made it, so nothing about an instruction's
// internals outlives it. What crosses between instructions is the processor's
// state, named by the operations that read and write it, and that is what the
// liveness analysis looks at.

// A bit is a flag or a comparison; a word is a register or an address.
enum class Type : std::uint8_t { Bit, Word };

// Where an operation's input comes from: nothing, a constant the lifter knew,
// an address of the code being lifted, or a temporary an earlier operation of
// the same instruction produced.
//
// An address is a constant of the code's own address space: the instruction's
// address plus an offset, or the address after it. For the main executable that
// is where it is mapped. For a module it is an offset from where the module is,
// and the base is added where the code runs.
struct Operand {
    enum class Kind : std::uint8_t { None, Immediate, Address, Temporary };

    Kind kind = Kind::None;
    Type type = Type::Word;
    std::uint32_t value = 0;

    [[nodiscard]] static Operand Word(std::uint32_t immediate) {
        return {.kind = Kind::Immediate, .type = Type::Word, .value = immediate};
    }

    [[nodiscard]] static Operand Address(std::uint32_t address) {
        return {.kind = Kind::Address, .type = Type::Word, .value = address};
    }

    [[nodiscard]] static Operand Bit(bool immediate) {
        return {.kind = Kind::Immediate, .type = Type::Bit, .value = immediate ? 1U : 0U};
    }

    [[nodiscard]] static Operand Temporary(std::uint32_t id, Type type) {
        return {.kind = Kind::Temporary, .type = type, .value = id};
    }

    [[nodiscard]] bool IsPresent() const {
        return kind != Kind::None;
    }
};

// The condition flags that are separate pieces of processor state. GE is four
// bits and is not a flag of this kind; it is not lowered yet.
enum class Flag : std::uint8_t { Negative, Zero, Carry, Overflow, Saturated };

constexpr std::size_t FlagCount = 5;

// The thread ID registers of coprocessor 15 that user mode reaches.
enum class ThreadId : std::uint8_t { ReadWrite, ReadOnly };

// What an operation does. A destination is a temporary; "a", "b", "c" and "d"
// are the operands in order. The primitive named is in domain/arm/primitives.
enum class OpKind : std::uint8_t {
    // State in and out. `index` names the register, flag or thread ID.
    ReadRegister,   // destination <- r[index]
    WriteRegister,  // r[index] <- a
    ReadFlag,       // destination <- flag[index]
    WriteFlag,      // flag[index] <- a
    WriteThumb,     // the instruction set bit <- a
    ReadThreadId,   // destination <- thread ID register[index]
    WriteThreadId,  // thread ID register[index] <- a

    // Arithmetic on words.
    Add,       // destination <- a + b
    Subtract,  // destination <- a - b
    And,       // destination <- a & b
    Or,        // destination <- a | b
    Xor,       // destination <- a ^ b
    Multiply,  // destination <- a * b, the low 32 bits

    // Comparisons, giving a bit.
    Equal,     // destination <- a == b
    NotEqual,  // destination <- a != b
    Negative,  // destination <- bit 31 of a

    // destination <- a ? b : c, with a a bit.
    Select,

    // primitives::Shift. `index` is the ShiftKind; a is the value, b the amount
    // and c the carry going in. destination is the value, destination2 the
    // carry out.
    Shift,

    // primitives::ComputeAlu. `index` is the Operation; a is the first operand,
    // b the second operand's value and c its carry out of the shifter, d the
    // carry going in. destination is the result, destination2 the carry and
    // destination3 the overflow, which only an arithmetic operation sets.
    Alu,

    // primitives::Extend. `index` is the Operation, a the source already
    // rotated and b the addend, which is zero for the forms without one.
    Extend,

    // primitives::ConditionPassed. `index` is the Condition and a, b, c, d are
    // N, Z, C and V.
    ConditionPassed,

    // Memory. `index` is the primitives::Width. A load or store the memory
    // refuses leaves the instruction, as a bail does.
    Load,   // destination <- memory[a]
    Store,  // memory[a] <- b

    // Leaves the lifted code for the interpreter, at this instruction and with
    // the state as it was before it, if a is true. The interpreter then does
    // what it does, which for the cases the lifter bails on is stopping and
    // saying why.
    BailIf,

    // The interpreter executes this instruction, whole. For what the lifter does
    // not translate yet, and for the supervisor call, whose effect is the
    // kernel's. Always the only operation of its instruction.
    Interpret,

    // The interpreter's handler for this instruction does its work, called
    // directly: the condition is the instruction's, checked as for any other,
    // the operands were checked when it was lowered, and it does not write r15.
    // Always the only operation of its instruction.
    Execute,
};

constexpr std::uint32_t NoTemporary = 0xFFFFFFFF;

struct Op {
    OpKind kind = OpKind::Interpret;
    std::uint32_t index = 0;
    std::uint32_t destination = NoTemporary;
    std::uint32_t destination2 = NoTemporary;
    std::uint32_t destination3 = NoTemporary;
    Operand a{};
    Operand b{};
    Operand c{};
    Operand d{};
};

// One guest instruction, translated.
struct Instruction {
    std::uint32_t address = 0;
    std::uint8_t width = 4;
    arm::InstructionSet instruction_set = arm::InstructionSet::Arm;

    // The operations run only if this passes; the instruction costs a tick
    // either way.
    arm::Condition condition = arm::Condition::Always;

    std::vector<Op> ops;
    std::uint32_t temporaries = 0;

    // Where a branch with a displacement goes, which the operations do not
    // write: the block's exit knows it. Empty for everything else.
    std::optional<std::uint32_t> branch_target;

    [[nodiscard]] bool IsInterpreted() const {
        return ops.size() == 1 && ops.front().kind == OpKind::Interpret;
    }

    [[nodiscard]] bool IsExecutedByAHandler() const {
        return ops.size() == 1 && ops.front().kind == OpKind::Execute;
    }
};

// The operation as a line of text, for tests and for a person reading what an
// instruction became.
std::string Describe(const Op& op);

// All the operations of an instruction, one to a line, headed by its address and
// condition.
std::string Describe(const Instruction& instruction);

}  // namespace hexlith::domain::lift::ir

#endif  // HEXLITH_DOMAIN_LIFT_IR_H
