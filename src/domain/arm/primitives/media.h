#ifndef HEXLITH_DOMAIN_ARM_PRIMITIVES_MEDIA_H
#define HEXLITH_DOMAIN_ARM_PRIMITIVES_MEDIA_H

#include "domain/arm/instruction.h"
#include "domain/arm/operation.h"
#include "domain/arm/primitives/integer.h"
#include "domain/arm/primitives/packing.h"
#include "domain/arm/primitives/saturate.h"

#include <cstdint>

// The parts of the media instructions that sit between the decoded fields and
// the arithmetic: which extension an operation is, and the shift amounts that
// the encodings spell as zero but mean as thirty-two.
namespace hexlith::domain::arm::primitives {

// An arithmetic shift right by zero means by thirty-two, in the saturating
// shifts and in PKHTB alike.
constexpr std::uint32_t FullShift = 32;

[[nodiscard]] constexpr std::uint32_t ArithmeticShiftAmount(std::uint32_t encoded) {
    return encoded == 0 ? FullShift : encoded;
}

// The value SSAT and USAT saturate: the operand after its optional shift,
// signed.
[[nodiscard]] constexpr std::int64_t
SaturationOperand(ShiftKind kind, std::uint32_t value, std::uint32_t shift_amount) {
    const bool right = kind == ShiftKind::ArithmeticRight;
    const std::uint32_t amount = right ? ArithmeticShiftAmount(shift_amount) : shift_amount;
    return Signed(Shift(kind, value, amount, false).value);
}

// SXTB, SXTAB, UXTH and the rest of the extensions, on a source already
// rotated, with the addend that the accumulating forms add and the others
// take as zero. Any other operation yields zero.
[[nodiscard]] constexpr std::uint32_t
Extend(Operation operation, std::uint32_t rotated, std::uint32_t addend) {
    switch (operation) {
    case Operation::Sxtb:
    case Operation::Sxtab:
        return addend + SignExtendByte(rotated);
    case Operation::Sxth:
    case Operation::Sxtah:
        return addend + SignExtendHalf(rotated);
    case Operation::Uxtb:
    case Operation::Uxtab:
        return addend + ZeroExtendByte(rotated);
    case Operation::Uxth:
    case Operation::Uxtah:
        return addend + ZeroExtendHalf(rotated);
    case Operation::Sxtb16:
    case Operation::Sxtab16:
        return AddHalves(addend, SignExtendBytePair(rotated));
    case Operation::Uxtb16:
    case Operation::Uxtab16:
        return AddHalves(addend, ZeroExtendBytePair(rotated));
    default:
        break;
    }
    return 0;
}

}  // namespace hexlith::domain::arm::primitives

#endif  // HEXLITH_DOMAIN_ARM_PRIMITIVES_MEDIA_H
