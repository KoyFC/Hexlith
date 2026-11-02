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

// A processor state and a small address space: a read-only page of code, a
// page of data and a page of stack.
struct Machine {
    Machine();

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
std::string Disagreement(const arm::Instruction& instruction, const Setup& setup);

// The same, from every combination of the four condition flags.
std::string DisagreementForAllFlags(const arm::Instruction& instruction, const Setup& setup);

}  // namespace hexlith::domain::lift::test

#endif  // HEXLITH_TESTS_DOMAIN_LIFT_DIFFERENTIAL_H
