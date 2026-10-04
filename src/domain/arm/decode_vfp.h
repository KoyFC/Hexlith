#ifndef HEXLITH_DOMAIN_ARM_DECODE_VFP_H
#define HEXLITH_DOMAIN_ARM_DECODE_VFP_H

#include "domain/arm/instruction.h"

#include <cstdint>

namespace hexlith::domain::arm {

// Whether an ARM word in the coprocessor space addresses coprocessor 10 or 11,
// which on the ARM11 is the VFPv2 floating point unit rather than a coprocessor
// in its own right. Bits 11 to 9 are 101 for both; bit 8 picks the precision.
[[nodiscard]] constexpr bool AddressesFloatingPoint(std::uint32_t word) {
    constexpr std::uint32_t FloatingPointCoprocessors = 0b101;
    return ((word >> 9U) & 0b111U) == FloatingPointCoprocessors;
}

// Decodes a conditional ARM word in the coprocessor space that addresses the
// floating point unit. DecodeArm calls it; nothing else should need to.
//
// Total in the same way DecodeArm is: a word in this space that VFPv2 does not
// define comes back Undefined. That includes everything VFPv3 and Advanced SIMD
// added later in the same space, such as the registers from d16 up, immediate
// moves and fixed-point conversions, because the ARM11 has none of them.
//
// Sources: the ARM Architecture Reference Manual for ARMv7-A and ARMv7-R,
// sections A7.5 to A7.9, which lay out VFPv2 and its later extensions in one
// table and mark which version added each row; checked against Capstone by
// scripts/compare_arm_decoder.py.
[[nodiscard]] Instruction DecodeVfp(std::uint32_t word);

}  // namespace hexlith::domain::arm

#endif  // HEXLITH_DOMAIN_ARM_DECODE_VFP_H
