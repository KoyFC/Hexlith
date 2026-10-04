#ifndef HEXLITH_DOMAIN_ARM_REGISTERS_H
#define HEXLITH_DOMAIN_ARM_REGISTERS_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hexlith::domain::arm {

// A general-purpose register, numbered the way the encoding numbers it.
//
// A plain index rather than an enumeration, because that is what the encoding
// holds and what every table is indexed by. The three that have a job of their
// own are named, and r15 having a job of its own is the reason analysis is
// hard: an instruction that writes it is a branch, whatever its mnemonic says.
using RegisterIndex = std::uint8_t;

constexpr std::size_t RegisterCount = 16;

// A field of a decoded instruction that names no register. Zero would be r0,
// which is a register, so a value outside the range is the only way to tell a
// field that was filled in from one that was not.
constexpr RegisterIndex NoRegister = 0xFF;

constexpr RegisterIndex StackPointer = 13;
constexpr RegisterIndex LinkRegister = 14;
constexpr RegisterIndex ProgramCounter = 15;

// The conventional name: r0 to r12, then sp, lr and pc.
std::string_view RegisterName(RegisterIndex index);

// A bitmask of registers, as a load or store multiple carries it: bit n is
// register n.
using RegisterSet = std::uint16_t;

[[nodiscard]] constexpr bool Holds(RegisterSet set, RegisterIndex index) {
    return (set & (1U << index)) != 0;
}

}  // namespace hexlith::domain::arm

#endif  // HEXLITH_DOMAIN_ARM_REGISTERS_H
