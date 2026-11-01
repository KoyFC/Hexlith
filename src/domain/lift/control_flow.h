#ifndef HEXLITH_DOMAIN_LIFT_CONTROL_FLOW_H
#define HEXLITH_DOMAIN_LIFT_CONTROL_FLOW_H

#include "core/byte_reader.h"
#include "domain/analysis/tables.h"
#include "domain/arm/condition.h"
#include "domain/arm/instruction.h"
#include "domain/arm/instruction_set.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

namespace hexlith::domain::lift {

// The control flow of one function, as the lifter sees it: basic blocks, and how
// each one ends.
//
// A block is a run of instructions entered only at its first one and left only
// at its last, and "left" includes everything that hands control to something
// the function does not own: a call, a supervisor call, a return. Those end a
// block for a reason beyond tidiness. The lifted code charges a block's
// instructions to the clock when it enters the block, so that the clock agrees
// with the interpreter's to the instruction. If a callee gave the clock away in
// the middle of a block the caller had already paid for, the instructions after
// the call would be counted twice when the interpreter finished them. A call
// ends its block, and the code after it is a block of its own, paid for when
// the call returns.
//
// Every instruction of a block is executed, whether or not its condition
// passes, and costs one tick either way, so a block's cost is the number of
// instructions in it.

struct CodeImage {
    core::ByteSpan bytes;
    std::uint32_t base = 0;

    [[nodiscard]] bool Contains(std::uint32_t address) const {
        return address >= base && address - base < bytes.size();
    }
};

// How a block's last instruction hands control on.
//
// Every kind but Fallthrough is the last instruction of its block, and carries
// that instruction's condition: when it fails, control continues at the
// instruction after it. A call and a supervisor call continue there when they
// return, whatever their condition is.
enum class ExitKind : std::uint8_t {
    // The block ends because the next instruction is entered from elsewhere too.
    Fallthrough,

    // A branch to an address in this function.
    Jump,

    // A branch to another function's entry point, which is a call that does not
    // come back here.
    TailCall,

    // BL or BLX to an address.
    Call,

    // BLX through a register.
    IndirectCall,

    // BX or a load or move into the program counter, to an address held in a
    // register or in memory that the image does not settle. The interpreter, or the dispatcher,
    // finds out
    // where.
    IndirectJump,

    // A return: BX lr, a move of lr into the program counter, or a pop of it.
    Return,

    // A load through a table of code addresses the analysis found.
    Switch,

    // SVC. The kernel is entered, and may hand the processor to another thread.
    SystemCall,

    // Control cannot be followed: an instruction that is not one, one that
    // stops the processor, a branch to bytes outside the image, or the end of
    // the image. The interpreter takes over at this instruction and says why.
    Trap,
};

struct Exit {
    ExitKind kind = ExitKind::Fallthrough;
    arm::Condition condition = arm::Condition::Always;

    // The address a Jump, TailCall or Call goes to, and the instruction set it
    // is in. Also where a Trap that was a branch outside the image was heading.
    std::uint32_t target = 0;
    arm::InstructionSet target_set = arm::InstructionSet::Arm;

    // A Switch's table and the address each of its entries holds, in order.
    std::uint32_t table = 0;
    std::vector<std::uint32_t> cases;

    // The address control continues at when the exit does not take it, or when
    // a call returns. Zero when there is none.
    std::uint32_t next = 0;

    [[nodiscard]] bool HasNext() const {
        return next != 0;
    }
};

struct PlacedInstruction {
    std::uint32_t address = 0;
    arm::Instruction instruction;
};

struct BasicBlock {
    std::uint32_t start = 0;

    // The address after the last instruction.
    std::uint32_t end = 0;

    std::vector<PlacedInstruction> instructions;
    Exit exit;

    // The ticks entering the block costs the clock.
    [[nodiscard]] std::size_t Cost() const {
        return instructions.size();
    }
};

struct Function {
    std::uint32_t entry = 0;
    arm::InstructionSet instruction_set = arm::InstructionSet::Arm;

    // In address order. The first is the entry only if nothing before it is
    // reached, which a loop back to before the entry can contradict.
    std::vector<BasicBlock> blocks;

    [[nodiscard]] const BasicBlock* BlockAt(std::uint32_t address) const;
    [[nodiscard]] std::size_t InstructionCount() const;
};

// What the analysis knows that the instructions do not say.
struct FlowContext {
    // Every function entry point. A direct branch to one that is not this
    // function's own is a tail call.
    const std::unordered_set<std::uint32_t>* entries = nullptr;

    // The regions of the analysis, sorted by address, of which only the jump
    // tables are used: the table behind a dispatch is whatever region starts
    // eight bytes after it.
    std::span<const analysis::Region> regions;

    // Whether a literal in the image can be read as the constant it is. It can
    // in the main executable, whose text is mapped read-only and is never
    // relocated. It cannot in a module, where the loader writes the final
    // address into the literal after the file is read.
    bool literals_are_constant = true;
};

// The largest function the builder will follow. A function this long is a sign
// that something other than a function was followed, and the builder stops
// with a Trap rather than lifting it.
constexpr std::size_t MaximumInstructions = 1U << 16;

// Follows every path from the entry and cuts the instructions it reaches into
// blocks. The instruction set is the one the entry is in; a call or a branch
// that exchanges is a call, and what it reaches is another function's.
[[nodiscard]] Function BuildFunction(const CodeImage& image,
                                     std::uint32_t entry,
                                     arm::InstructionSet instruction_set,
                                     const FlowContext& context);

}  // namespace hexlith::domain::lift

#endif  // HEXLITH_DOMAIN_LIFT_CONTROL_FLOW_H
