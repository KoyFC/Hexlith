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
#include <string>
#include <string_view>
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

// The lifted code of the modules, and which of them is loaded where.
//
// A library holds one table of entries for each module, keyed by offset from the
// module's start. It offers them here. The loader then says when a module is
// loaded or unloaded and at what address, and the code of the modules that are
// loaded is what can be found. A module loaded twice at once is two ranges over
// one table.
class LoadedModules {
public:
    enum class Outcome : std::uint8_t {
        // The module's code will run lifted.
        Lifted,

        // Nothing was lifted for a module of this name, so it runs interpreted.
        NotLifted,

        // The module's code is not the code that was lifted. Lifted code against
        // other bytes is not a crash that announces itself, so the module runs
        // interpreted.
        DigestDiffers,
    };

    struct Found {
        Function function = nullptr;

        // Where the module is: what the function is run against.
        std::uint32_t base = 0;
    };

    // The table must outlive this. The digest is the SHA-256 of the module's code
    // segment as the file has it, in hexadecimal.
    void Offer(std::string_view name, std::string_view digest, const EntryTable& table) {
        offered_.push_back(
            Offered{.name = std::string{name}, .digest = std::string{digest}, .table = &table});
    }

    Outcome Loaded(std::string_view name,
                   std::uint32_t address,
                   std::uint32_t code_offset,
                   std::uint32_t code_size,
                   std::string_view digest) {
        const auto offer =
            std::ranges::find_if(offered_, [&](const Offered& each) { return each.name == name; });
        if (offer == offered_.end()) {
            return Outcome::NotLifted;
        }
        if (offer->digest != digest) {
            return Outcome::DigestDiffers;
        }
        residents_.push_back(Resident{.name = std::string{name},
                                      .address = address,
                                      .first = address + code_offset,
                                      .end = address + code_offset + code_size,
                                      .table = offer->table});
        return Outcome::Lifted;
    }

    // A module that was never loaded as lifted code is not an error here: it ran
    // interpreted, and there is nothing to forget.
    void Unloaded(std::string_view name, std::uint32_t address) {
        std::erase_if(residents_, [&](const Resident& each) {
            return each.address == address && each.name == name;
        });
    }

    [[nodiscard]] std::optional<Found> Find(Pc address, bool thumb) const {
        for (const Resident& each : residents_) {
            if (address >= each.first && address < each.end) {
                if (const Function function = each.table->Find(address - each.address, thumb)) {
                    return Found{.function = function, .base = each.address};
                }
            }
        }
        return std::nullopt;
    }

private:
    struct Offered {
        std::string name;
        std::string digest;
        const EntryTable* table = nullptr;
    };

    struct Resident {
        std::string name;
        std::uint32_t address = 0;
        std::uint32_t first = 0;
        std::uint32_t end = 0;
        const EntryTable* table = nullptr;
    };

    std::vector<Offered> offered_;
    std::vector<Resident> residents_;
};

struct Context {
    arm::CpuState* cpu = nullptr;
    memory::ProcessMemory* memory = nullptr;
    const EntryTable* entries = nullptr;

    // The modules that are loaded, or none when the run has no lifted modules.
    const LoadedModules* modules = nullptr;

    // Where the module whose code is running is mapped. Module code is lifted
    // at offsets from its start and adds this to every address it computes or
    // hands back; the main executable's code is lifted at the addresses it runs
    // at and leaves this alone. A branch or a call inside a module cannot leave
    // it, so only the engine changes it.
    std::uint32_t base = 0;

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
    if (const Function function = context.entries->Find(target, context.cpu->thumb)) {
        return function(context, target);
    }
    if (context.modules != nullptr) {
        if (const std::optional<LoadedModules::Found> found =
                context.modules->Find(target, context.cpu->thumb)) {
            // The caller may be in another module, and is again once this returns.
            const std::uint32_t caller = context.base;
            context.base = found->base;
            const Pc next = found->function(context, target);
            context.base = caller;
            return next;
        }
    }
    return target;
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
// lifter does not translate. It was decoded once, where the lifted code is, and
// not every time it runs.
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
