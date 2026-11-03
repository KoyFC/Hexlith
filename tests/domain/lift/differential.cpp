#include "differential.h"

#include "domain/arm/interpreter/interpreter.h"
#include "domain/arm/primitives/integer.h"
#include "domain/lift/control_flow.h"
#include "domain/lift/evaluate.h"
#include "domain/lift/lower.h"

#include <format>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::lift::test {
namespace {

namespace primitives = arm::primitives;

constexpr std::uint32_t PageSize = memory::PagedMemory::PageSize;

std::string Describe(const arm::Stop& stop) {
    return std::format("{} {:#x}", arm::Describe(stop.reason), stop.detail);
}

// The bytes of the two pages an instruction can write to.
std::vector<std::uint8_t> WritableBytes(const memory::PagedMemory& memory) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(2) * PageSize);
    static_cast<void>(memory.Copy(Data, std::span{bytes}.first(PageSize)));
    static_cast<void>(memory.Copy(Stack, std::span{bytes}.last(PageSize)));
    return bytes;
}

std::string Differences(const Machine& expected, const Machine& actual) {
    const arm::CpuState& e = expected.state;
    const arm::CpuState& a = actual.state;
    for (std::size_t index = 0; index < e.registers.size(); ++index) {
        if (e.registers.at(index) != a.registers.at(index)) {
            return std::format("r{} is {:#x}, the interpreter has {:#x}",
                               index,
                               a.registers.at(index),
                               e.registers.at(index));
        }
    }
    if (e.negative != a.negative || e.zero != a.zero || e.carry != a.carry ||
        e.overflow != a.overflow || e.saturated != a.saturated) {
        return std::format("flags are NZCVQ {}{}{}{}{}, the interpreter has {}{}{}{}{}",
                           a.negative,
                           a.zero,
                           a.carry,
                           a.overflow,
                           a.saturated,
                           e.negative,
                           e.zero,
                           e.carry,
                           e.overflow,
                           e.saturated);
    }
    if (e.greater_or_equal != a.greater_or_equal || e.thumb != a.thumb) {
        return "GE or the instruction set differs";
    }
    if (e.float_registers != a.float_registers || e.fpscr != a.fpscr) {
        return "floating point state differs";
    }
    if (e.exclusive_open != a.exclusive_open || e.exclusive_address != a.exclusive_address) {
        return "the exclusive monitor differs";
    }
    if (e.thread_id_read_write != a.thread_id_read_write ||
        e.thread_id_read_only != a.thread_id_read_only) {
        return "a thread ID register differs";
    }
    if (WritableBytes(expected.memory) != WritableBytes(actual.memory)) {
        return "memory differs";
    }
    return {};
}

// The machine with the instruction in its code page, where the interpreter
// fetches it from, and the processor about to run it.
Machine
Prepare(const arm::Instruction& instruction, const Setup& setup, const Placement& placement) {
    Machine machine{placement.Address()};
    setup(machine);
    machine.state.registers.at(arm::ProgramCounter) = placement.Address();
    machine.state.thumb = instruction.instruction_set == arm::InstructionSet::Thumb;

    std::vector<std::uint8_t> bytes(instruction.width);
    for (unsigned index = 0; index < instruction.width; ++index) {
        bytes.at(index) = static_cast<std::uint8_t>(instruction.encoding >> (8U * index));
    }
    EXPECT_TRUE(machine.memory.Load(placement.Address(), bytes));
    return machine;
}

}  // namespace

Machine::Machine(std::uint32_t code) {
    EXPECT_TRUE(memory.Map(code, PageSize, false));
    EXPECT_TRUE(memory.Map(Data, PageSize, true));
    EXPECT_TRUE(memory.Map(Stack, PageSize, true));
    state.registers.at(arm::StackPointer) = Stack + PageSize;
}

void DefaultSetup(Machine& machine) {
    constexpr std::uint32_t Awkward = 0xFFFFFFF0;
    machine.state.registers.at(0) = 0x11111111;
    machine.state.registers.at(1) = Data + 0x100;
    machine.state.registers.at(2) = 3;
    machine.state.registers.at(3) = Awkward;
    machine.state.registers.at(4) = 0x80000000;
    machine.state.registers.at(5) = 33;
    machine.state.registers.at(arm::LinkRegister) = Code + 0x20;
    for (std::uint32_t offset = 0; offset < PageSize; ++offset) {
        EXPECT_TRUE(
            machine.memory.Write8(Data + offset, static_cast<std::uint8_t>((offset * 7) + 1)));
    }
}

std::string
Disagreement(const arm::Instruction& instruction, const Setup& setup, const Placement& placement) {
    Machine reference = Prepare(instruction, setup, placement);
    Machine lifted = Prepare(instruction, setup, placement);

    const arm::Stop stopped = arm::Step(reference.state, reference.memory);

    const PlacedInstruction placed{.address = placement.offset, .instruction = instruction};
    const ir::Instruction lowered = Lower(placed);
    // r15 is set to something no branch goes to, so that an instruction that
    // writes it can be told from one that leaves it for the block to move on,
    // including a branch to itself.
    constexpr std::uint32_t Untouched = 0xDEADBEEF;
    lifted.state.registers.at(arm::ProgramCounter) = Untouched;
    const Evaluation evaluation = Evaluate(lowered, lifted.state, lifted.memory, placement.base);

    if (stopped.Stopped()) {
        if (evaluation.kind == EvaluationKind::Completed) {
            return std::format("the interpreter stops ({}) and the lowering completes",
                               Describe(stopped));
        }
        if (evaluation.kind == EvaluationKind::Bailed) {
            // The lifted code leaves for the interpreter, which executes the
            // instruction from the state it was left in and has to stop as the
            // reference did, with the same state.
            lifted.state.registers.at(arm::ProgramCounter) = placement.Address();
            const arm::Stop again = arm::Step(lifted.state, lifted.memory);
            if (again.reason != stopped.reason || again.detail != stopped.detail) {
                return std::format("after the bail the interpreter stops with {}, not {}",
                                   Describe(again),
                                   Describe(stopped));
            }
            return Differences(reference, lifted);
        }
        if (evaluation.stop.reason != stopped.reason || evaluation.stop.detail != stopped.detail) {
            return std::format("stops with {}, the interpreter with {}",
                               Describe(evaluation.stop),
                               Describe(stopped));
        }
        return Differences(reference, lifted);
    }

    if (evaluation.kind == EvaluationKind::Bailed) {
        return "bailed where the interpreter completes";
    }
    if (evaluation.kind == EvaluationKind::Stopped) {
        return std::format("stops ({}) where the interpreter completes", Describe(evaluation.stop));
    }

    // The block moves r15 on, or to a branch's target, unless the operations did.
    std::uint32_t& pc = lifted.state.registers.at(arm::ProgramCounter);
    if (pc == Untouched) {
        pc = placement.Address() + instruction.width;
        if (lowered.branch_target.has_value()) {
            const primitives::ConditionFlags flags{.negative = lifted.state.negative,
                                                   .zero = lifted.state.zero,
                                                   .carry = lifted.state.carry,
                                                   .overflow = lifted.state.overflow};
            // The flags the branch saw are the ones before it ran, and a branch
            // writes none.
            if (primitives::ConditionPassed(lowered.condition, flags)) {
                pc = placement.base + *lowered.branch_target;
            }
        }
    }
    return Differences(reference, lifted);
}

std::string DisagreementForAllFlags(const arm::Instruction& instruction,
                                    const Setup& setup,
                                    const Placement& placement) {
    constexpr unsigned Combinations = 16;
    for (unsigned flags = 0; flags < Combinations; ++flags) {
        const Setup with_flags = [&setup, flags](Machine& machine) {
            setup(machine);
            machine.state.negative = (flags & 8U) != 0;
            machine.state.zero = (flags & 4U) != 0;
            machine.state.carry = (flags & 2U) != 0;
            machine.state.overflow = (flags & 1U) != 0;
        };
        const std::string difference = Disagreement(instruction, with_flags, placement);
        if (!difference.empty()) {
            return std::format("with NZCV {:04b}: {}", flags, difference);
        }
    }
    return {};
}

}  // namespace hexlith::domain::lift::test
