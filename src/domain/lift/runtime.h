#ifndef HEXLITH_DOMAIN_LIFT_RUNTIME_H
#define HEXLITH_DOMAIN_LIFT_RUNTIME_H

#include "domain/arm/cpu_state.h"
#include "domain/arm/decode_arm.h"
#include "domain/arm/decode_thumb.h"
#include "domain/arm/instruction.h"
#include "domain/arm/interpreter/interpreter.h"
#include "domain/arm/interpreter/step.h"
#include "domain/arm/primitives/alu.h"
#include "domain/arm/primitives/integer.h"
#include "domain/arm/primitives/media.h"
#include "domain/arm/primitives/transfer.h"
#include "domain/memory/process_memory.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

// What generated code includes. It is small on purpose: the generated code calls
// the shared primitives for what an instruction does, and this for the few
// things that are about running as lifted code rather than about the processor.
//
// The protocol, which ADR 0025 records, is this. A lifted function runs the
// guest from a given address until the guest leaves the function, and answers
// the address the guest continues at. A caller that made a call checks the
// answer against the address after the call: if it matches, the callee has
// returned and the caller goes on; if not, the answer is passed up, because
// whoever is above it will know what to do with it, and at the top it is the
// engine. Leaving is therefore always safe. The whole of the guest's state is in
// the CpuState and the guest's memory at every point where code can leave, so
// the engine can pick up from the address it is given, natively or not.
//
// Two answers are not addresses. An address the guest can continue at has its
// low bit clear, because both instruction sets align to at least two bytes, so
// odd numbers are free to mean something else.
namespace hexlith::domain::lift::runtime {

using Pc = std::uint32_t;

// The code has stopped being able to run natively at cpu.pc. The interpreter
// executes the instruction there and the engine tries again afterwards.
constexpr Pc Interpret = 1;

// The interpreter stopped, and Context::stop says why. The state is the one the
// interpreter left.
constexpr Pc Stopped = 3;

struct Context;

using Function = Pc (*)(Context&, Pc);

// Where lifted code can be entered: every address that starts a block, for each
// instruction set, with the function that holds it.
class EntryTable {
public:
    void Add(Pc address, bool thumb, Function function) {
        (thumb ? thumb_ : arm_).emplace(address, function);
        lowest_ = std::min(lowest_, address);
        highest_ = std::max(highest_, address);
    }

    // Whether an address lies between the lowest and the highest entry, either
    // instruction set. Outside, Find answers nothing, and code the table does
    // not cover, such as a module's, is spared a lookup for each instruction
    // the interpreter runs.
    [[nodiscard]] bool MayCover(Pc address) const {
        return address >= lowest_ && address <= highest_;
    }

    [[nodiscard]] Function Find(Pc address, bool thumb) const {
        const auto& entries = thumb ? thumb_ : arm_;
        const auto found = entries.find(address);
        return found == entries.end() ? nullptr : found->second;
    }

    [[nodiscard]] std::size_t Size() const {
        return arm_.size() + thumb_.size();
    }

private:
    std::unordered_map<Pc, Function> arm_;
    std::unordered_map<Pc, Function> thumb_;
    Pc lowest_ = std::numeric_limits<Pc>::max();
    Pc highest_ = 0;
};

struct Context {
    arm::CpuState* cpu = nullptr;
    memory::ProcessMemory* memory = nullptr;
    const EntryTable* entries = nullptr;

    // Ticks the run may still spend. A block is charged when it is entered, so
    // this is what the clock reads at the start of the block that is running.
    std::int64_t budget = 0;

    // Why the interpreter stopped, when a function answered Stopped.
    arm::Stop stop;
};

[[nodiscard]] inline bool Passes(const arm::CpuState& cpu, arm::Condition condition) {
    return arm::primitives::ConditionPassed(
        condition,
        {.negative = cpu.negative, .zero = cpu.zero, .carry = cpu.carry, .overflow = cpu.overflow});
}

// Enters the lifted code at an address, or answers the address when there is
// none, so that the caller passes it up.
[[nodiscard]] inline Pc Dispatch(Context& context, Pc target) {
    const Function function = context.entries->Find(target, context.cpu->thumb);
    return function == nullptr ? target : function(context, target);
}

// The block at `address` costs more ticks than the run has left. The
// interpreter takes the rest of the slice an instruction at a time, which is how
// the clock stays exact.
[[nodiscard]] inline Pc OutOfBudget(Context& context, Pc address) {
    context.cpu->registers[arm::ProgramCounter] = address;
    return Interpret;
}

// The lifted code cannot do what the instruction at `address` needs, such as
// reach memory that is not mapped. Nothing of the instruction has been done, and
// `unexecuted` instructions of its block, itself included, were charged to the
// clock and have not run, so they are given back.
[[nodiscard]] inline Pc Bail(Context& context, Pc address, std::int64_t unexecuted) {
    context.budget += unexecuted;
    context.cpu->registers[arm::ProgramCounter] = address;
    return Interpret;
}

// The interpreter executes the instruction at `address`, for an instruction the
// lifter does not translate.
[[nodiscard]] inline arm::Stop ExecuteInterpreted(Context& context, Pc address) {
    context.cpu->registers[arm::ProgramCounter] = address;
    return arm::Step(*context.cpu, *context.memory);
}

// The same for an instruction decoded once, where the lifted code is, and not
// every time it runs.
[[nodiscard]] inline arm::Stop
ExecuteDecoded(Context& context, Pc address, const arm::Instruction& instruction) {
    context.cpu->registers[arm::ProgramCounter] = address;
    return arm::StepDecoded(*context.cpu, *context.memory, instruction);
}

// The interpreter's handler does the instruction's work. Its condition has
// passed and its operands were checked when it was lifted, and it does not
// write r15; the instruction is a constant, so the compiler picks the handler
// and sees the registers it names. r15 is set as the interpreter would have it,
// for a handler that reads it.
[[nodiscard]] [[gnu::always_inline]] inline arm::Stop
ExecuteHandled(Context& context, Pc address, const arm::Instruction& instruction) {
    context.cpu->registers[arm::ProgramCounter] = address;
    return arm::ExecuteHandler(*context.cpu, *context.memory, instruction, address);
}

// The interpreter stopped. The instruction that stopped it was not completed,
// except a supervisor call, which is, so `remaining` counts the ones after it
// and the stopping instruction is given back with them unless it was one.
[[nodiscard]] inline Pc StoppedBy(Context& context, arm::Stop stop, std::int64_t remaining) {
    const bool completed = stop.reason == arm::StopReason::SupervisorCall;
    context.budget += remaining + (completed ? 0 : 1);
    context.stop = stop;
    return Stopped;
}

}  // namespace hexlith::domain::lift::runtime

#endif  // HEXLITH_DOMAIN_LIFT_RUNTIME_H
