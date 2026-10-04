#ifndef HEXLITH_DOMAIN_ARM_PRIMITIVES_INTEGER_H
#define HEXLITH_DOMAIN_ARM_PRIMITIVES_INTEGER_H

#include "domain/arm/condition.h"
#include "domain/arm/instruction.h"

#include <bit>
#include <cstdint>

// The integer core of the shared primitive library: the shifter, addition with
// carry, and condition evaluation.
//
// ADR 0003 is why this exists. The interpreter calls these per instruction and
// generated code calls them inline, so the two cannot disagree about what an
// instruction does without one of them calling the wrong function. Everything
// here is constexpr and header-only for that reason: generated code has to be
// able to inline it, and a call across a translation unit boundary for every
// addition would make the lifted code slower than the interpreter.
//
// Each function follows the pseudocode of the ARM Architecture Reference Manual
// for ARMv7-A and ARMv7-R (DDI 0406C), which describes the ARMv6 behaviour as
// well, under the name given in the comment. They take values and return values
// rather than reading a CpuState, so that generated code can keep the operands
// in host registers.
namespace hexlith::domain::arm::primitives {

struct ShiftResult {
    std::uint32_t value = 0;
    bool carry = false;
};

// Shift_C, with the amount as a register-specified shift gives it: the bottom
// byte of a register, so anything from 0 to 255.
//
// An immediate shift is the same function once the decoder has turned the
// encoding's special cases into what they mean, LSR #0 into LSR #32 and ROR #0
// into RRX, which it does. An amount of zero is then no shift at all, and the
// carry passes through unchanged.
[[nodiscard]] constexpr ShiftResult
Shift(ShiftKind kind, std::uint32_t value, std::uint32_t amount, bool carry_in) {
    constexpr std::uint32_t Width = 32;

    if (kind == ShiftKind::RotateRightExtended) {
        return {.value = (static_cast<std::uint32_t>(carry_in) << (Width - 1)) | (value >> 1U),
                .carry = (value & 1U) != 0};
    }
    if (amount == 0) {
        return {.value = value, .carry = carry_in};
    }

    const bool sign = (value >> (Width - 1)) != 0;

    switch (kind) {
    case ShiftKind::LogicalLeft:
        if (amount < Width) {
            return {.value = value << amount, .carry = ((value >> (Width - amount)) & 1U) != 0};
        }
        return {.value = 0, .carry = amount == Width && (value & 1U) != 0};
    case ShiftKind::LogicalRight:
        if (amount < Width) {
            return {.value = value >> amount, .carry = ((value >> (amount - 1)) & 1U) != 0};
        }
        return {.value = 0, .carry = amount == Width && sign};
    case ShiftKind::ArithmeticRight:
        if (amount < Width) {
            // C++20 defines a right shift of a negative value as arithmetic.
            const auto shifted = static_cast<std::int32_t>(value) >> amount;
            return {.value = static_cast<std::uint32_t>(shifted),
                    .carry = ((value >> (amount - 1)) & 1U) != 0};
        }
        return {.value = sign ? ~0U : 0U, .carry = sign};
    case ShiftKind::RotateRight: {
        // A rotation by a multiple of 32 leaves the value alone and still sets
        // the carry from bit 31, which is the one way it differs from no shift.
        const std::uint32_t result = std::rotr(value, static_cast<int>(amount % Width));
        return {.value = result, .carry = (result >> (Width - 1)) != 0};
    }
    case ShiftKind::RotateRightExtended:
        break;
    }
    return {.value = value, .carry = carry_in};
}

// The carry an immediate operand of a data-processing instruction produces. A
// rotated immediate sets it from bit 31 of the result; an immediate that was not
// rotated leaves it alone. Source: ARMExpandImm_C.
[[nodiscard]] constexpr bool ImmediateCarry(std::uint32_t immediate, bool rotated, bool carry_in) {
    return rotated ? (immediate >> 31U) != 0 : carry_in;
}

struct AddResult {
    std::uint32_t value = 0;
    bool carry = false;
    bool overflow = false;
};

// AddWithCarry, which every addition and subtraction in the instruction set is
// written in terms of. A subtraction x - y is AddWithCarry(x, ~y, true), and its
// carry is the inverse of a borrow, which is the convention ARM chose and the
// opposite of x86's.
[[nodiscard]] constexpr AddResult AddWithCarry(std::uint32_t x, std::uint32_t y, bool carry_in) {
    const std::uint64_t unsigned_sum =
        static_cast<std::uint64_t>(x) + y + static_cast<std::uint64_t>(carry_in);
    const auto result = static_cast<std::uint32_t>(unsigned_sum);

    // Overflow is a sign the operands agreed on and the result did not.
    const bool overflow = ((~(x ^ y) & (x ^ result)) >> 31U) != 0;
    return {.value = result, .carry = (unsigned_sum >> 32U) != 0, .overflow = overflow};
}

struct ConditionFlags {
    bool negative = false;
    bool zero = false;
    bool carry = false;
    bool overflow = false;
};

// ConditionPassed. Unconditional passes, because an instruction in that group
// has no condition to fail.
[[nodiscard]] constexpr bool ConditionPassed(Condition condition, ConditionFlags flags) {
    switch (condition) {
    case Condition::Equal:
        return flags.zero;
    case Condition::NotEqual:
        return !flags.zero;
    case Condition::CarrySet:
        return flags.carry;
    case Condition::CarryClear:
        return !flags.carry;
    case Condition::Negative:
        return flags.negative;
    case Condition::NotNegative:
        return !flags.negative;
    case Condition::Overflow:
        return flags.overflow;
    case Condition::NoOverflow:
        return !flags.overflow;
    case Condition::Higher:
        return flags.carry && !flags.zero;
    case Condition::LowerOrSame:
        return !flags.carry || flags.zero;
    case Condition::GreaterOrEqual:
        return flags.negative == flags.overflow;
    case Condition::Less:
        return flags.negative != flags.overflow;
    case Condition::Greater:
        return !flags.zero && flags.negative == flags.overflow;
    case Condition::LessOrEqual:
        return flags.zero || flags.negative != flags.overflow;
    case Condition::Always:
    case Condition::Unconditional:
        return true;
    }
    return true;
}

}  // namespace hexlith::domain::arm::primitives

#endif  // HEXLITH_DOMAIN_ARM_PRIMITIVES_INTEGER_H
