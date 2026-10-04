#ifndef HEXLITH_DOMAIN_ARM_CPU_STATE_H
#define HEXLITH_DOMAIN_ARM_CPU_STATE_H

#include "domain/arm/registers.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace hexlith::domain::arm {

// The thirty-two single precision registers of VFPv2. The sixteen doubles are
// the same storage seen two words at a time.
constexpr std::size_t SingleRegisterCount = 32;
constexpr std::size_t DoubleRegisterCount = 16;

// What one thread of the game can see of the processor, and nothing more.
//
// The game runs in user mode and the kernel is reimplemented rather than run,
// so there are no banked registers, no saved status registers and no mode
// field. An instruction that would need them is privileged, and whatever
// executes it stops there rather than inventing a mode.
//
// The condition flags are separate fields rather than bits of a CPSR word.
// Generated code reads and writes them constantly, and a host compiler keeps a
// bool in a register where it would have to mask and shift a packed word; the
// word is assembled only when an instruction asks for it, which is rare.
struct CpuState {
    // r0 to r15. r15 holds the address of the instruction about to run, not the
    // value an instruction reads from it, which is eight bytes further on in ARM
    // and four in Thumb.
    std::array<std::uint32_t, RegisterCount> registers{};

    // N, Z, C and V.
    bool negative = false;
    bool zero = false;
    bool carry = false;
    bool overflow = false;

    // Q, the sticky flag saturating arithmetic sets and nothing clears except an
    // explicit write.
    bool saturated = false;

    // GE[3:0], one bit per byte lane, set by parallel addition and subtraction
    // and read by SEL.
    std::uint8_t greater_or_equal = 0;

    // T: whether the instruction at r15 is Thumb.
    bool thumb = false;

    // s0 to s31. d0 is s0 and s1, with s0 the low word.
    std::array<std::uint32_t, SingleRegisterCount> float_registers{};
    std::uint32_t fpscr = 0;

    // The local exclusive monitor: whether a load exclusive has opened it and
    // which address it tagged. A store exclusive succeeds only while it is open.
    bool exclusive_open = false;
    std::uint32_t exclusive_address = 0;
};

// CPSR bit positions. Source: ARM Architecture Reference Manual, ARMv6 edition
// (DDI 0100I), section A2.5, "Program status registers".
namespace cpsr {
constexpr unsigned NegativeBit = 31;
constexpr unsigned ZeroBit = 30;
constexpr unsigned CarryBit = 29;
constexpr unsigned OverflowBit = 28;
constexpr unsigned SaturatedBit = 27;
constexpr unsigned GreaterOrEqualShift = 16;
constexpr std::uint32_t GreaterOrEqualMask = 0xF;
constexpr unsigned ThumbBit = 5;

// The mode field reads 0b10000 in user mode, the only mode the game runs in.
constexpr std::uint32_t UserMode = 0x10;

// The bits user mode may write with MSR: the flags, Q and GE. Writes to the
// rest of the word are ignored in user mode rather than refused.
constexpr std::uint32_t UserWritableMask = 0xF80F0000;
}  // namespace cpsr

// The CPSR as MRS reads it.
[[nodiscard]] constexpr std::uint32_t PackCpsr(const CpuState& state) {
    auto bit = [](bool value, unsigned position) {
        return static_cast<std::uint32_t>(value ? 1U : 0U) << position;
    };
    return bit(state.negative, cpsr::NegativeBit) | bit(state.zero, cpsr::ZeroBit) |
           bit(state.carry, cpsr::CarryBit) | bit(state.overflow, cpsr::OverflowBit) |
           bit(state.saturated, cpsr::SaturatedBit) |
           (static_cast<std::uint32_t>(state.greater_or_equal) << cpsr::GreaterOrEqualShift) |
           bit(state.thumb, cpsr::ThumbBit) | cpsr::UserMode;
}

// Writes the bits of a CPSR word that user mode may change, under a byte mask
// of the kind MSR carries: bit 3 for the flags byte, bit 2 for the byte that
// holds GE. The other two bytes hold nothing user mode can write.
constexpr void
WriteCpsrFromUserMode(CpuState& state, std::uint32_t value, std::uint32_t byte_mask) {
    constexpr std::uint32_t FlagsByte = 0b1000;
    constexpr std::uint32_t StatusByte = 0b0100;

    if ((byte_mask & FlagsByte) != 0) {
        state.negative = ((value >> cpsr::NegativeBit) & 1U) != 0;
        state.zero = ((value >> cpsr::ZeroBit) & 1U) != 0;
        state.carry = ((value >> cpsr::CarryBit) & 1U) != 0;
        state.overflow = ((value >> cpsr::OverflowBit) & 1U) != 0;
        state.saturated = ((value >> cpsr::SaturatedBit) & 1U) != 0;
    }
    if ((byte_mask & StatusByte) != 0) {
        state.greater_or_equal = static_cast<std::uint8_t>((value >> cpsr::GreaterOrEqualShift) &
                                                           cpsr::GreaterOrEqualMask);
    }
}

[[nodiscard]] constexpr std::uint64_t ReadDouble(const CpuState& state, std::size_t index) {
    const std::uint64_t low = state.float_registers.at(index * 2);
    const std::uint64_t high = state.float_registers.at((index * 2) + 1);
    return low | (high << 32U);
}

constexpr void WriteDouble(CpuState& state, std::size_t index, std::uint64_t value) {
    state.float_registers.at(index * 2) = static_cast<std::uint32_t>(value);
    state.float_registers.at((index * 2) + 1) = static_cast<std::uint32_t>(value >> 32U);
}

}  // namespace hexlith::domain::arm

#endif  // HEXLITH_DOMAIN_ARM_CPU_STATE_H
