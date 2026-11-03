#ifndef HEXLITH_TESTS_DOMAIN_LIFT_DIFFERENTIAL_H
#define HEXLITH_TESTS_DOMAIN_LIFT_DIFFERENTIAL_H

#include "domain/arm/cpu_state.h"
#include "domain/arm/instruction.h"
#include "domain/memory/paged_memory.h"

#include <cstdint>
#include <functional>
#include <string>

namespace hexlith::domain::lift::test {

// Where the machines of these tests keep their code, data and stack.
constexpr std::uint32_t Code = 0x00100000;
constexpr std::uint32_t Data = 0x00200000;
constexpr std::uint32_t Stack = 0x00300000;

// Where an instruction is, for a test of module code: lifted at `offset` from
// the start of a module, which is mapped at `base`. The main executable's code
// has a base of zero and is at the address it is lifted at.
struct Placement {
    std::uint32_t offset = Code;
    std::uint32_t base = 0;

    [[nodiscard]] std::uint32_t Address() const {
        return base + offset;
    }
};

// A processor state and a small address space: a read-only page of code, a
// page of data and a page of stack.
struct Machine {
    explicit Machine(std::uint32_t code = Code);

    arm::CpuState state;
    memory::PagedMemory memory;
};

using Setup = std::function<void(Machine&)>;

// Registers pointing into the data page and a few that hold awkward values,
// and a data page of bytes that differ from one another.
void DefaultSetup(Machine& machine);

// Runs one instruction through the interpreter and through its lowering from the
// same starting state, and says in what way the two differ, or nothing if they
// agree.
//
// "Agree" has a precise meaning for an instruction that does not complete. The
// interpreter stops; the lowered code either bails, after which the interpreter
// executes the instruction from the state the bail left and must stop with the
// same reason and leave the same state, or hands the instruction to the
// interpreter and stops with the same reason.
std::string Disagreement(const arm::Instruction& instruction,
                         const Setup& setup,
                         const Placement& placement = {});

// The same, from every combination of the four condition flags.
std::string DisagreementForAllFlags(const arm::Instruction& instruction,
                                    const Setup& setup,
                                    const Placement& placement = {});

}  // namespace hexlith::domain::lift::test

#endif  // HEXLITH_TESTS_DOMAIN_LIFT_DIFFERENTIAL_H
