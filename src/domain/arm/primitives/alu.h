#ifndef HEXLITH_DOMAIN_ARM_PRIMITIVES_ALU_H
#define HEXLITH_DOMAIN_ARM_PRIMITIVES_ALU_H

#include "domain/arm/operation.h"
#include "domain/arm/primitives/integer.h"

#include <cstdint>

// What a data-processing operation computes, and which flags it leaves behind.
//
// The interpreter and the lifter both ask this and nothing else: given the
// operation, the first operand, the shifter's output and the carry going in,
// the result and the flags it would set. Whether the flags are written at all
// is the S bit, and is the caller's business.
namespace hexlith::domain::arm::primitives {

struct AluResult {
    std::uint32_t value = 0;
    bool carry = false;
    bool overflow = false;

    // Logical operations leave V alone and set C from the shifter.
    bool arithmetic = false;
};

[[nodiscard]] constexpr AluResult Arithmetic(AddResult sum) {
    return {.value = sum.value, .carry = sum.carry, .overflow = sum.overflow, .arithmetic = true};
}

[[nodiscard]] constexpr AluResult Logical(std::uint32_t value, bool shifter_carry) {
    return {.value = value, .carry = shifter_carry, .overflow = false, .arithmetic = false};
}

[[nodiscard]] constexpr AluResult
ComputeAlu(Operation operation, std::uint32_t first, ShiftResult second, bool carry) {
    const std::uint32_t operand = second.value;

    switch (operation) {
    case Operation::And:
    case Operation::Tst:
        return Logical(first & operand, second.carry);
    case Operation::Eor:
    case Operation::Teq:
        return Logical(first ^ operand, second.carry);
    case Operation::Orr:
        return Logical(first | operand, second.carry);
    case Operation::Bic:
        return Logical(first & ~operand, second.carry);
    case Operation::Mov:
        return Logical(operand, second.carry);
    case Operation::Mvn:
        return Logical(~operand, second.carry);
    case Operation::Sub:
    case Operation::Cmp:
        return Arithmetic(AddWithCarry(first, ~operand, true));
    case Operation::Rsb:
        return Arithmetic(AddWithCarry(operand, ~first, true));
    case Operation::Add:
    case Operation::Cmn:
        return Arithmetic(AddWithCarry(first, operand, false));
    case Operation::Adc:
        return Arithmetic(AddWithCarry(first, operand, carry));
    case Operation::Sbc:
        return Arithmetic(AddWithCarry(first, ~operand, carry));
    case Operation::Rsc:
        return Arithmetic(AddWithCarry(operand, ~first, carry));
    default:
        break;
    }
    return {};
}

}  // namespace hexlith::domain::arm::primitives

#endif  // HEXLITH_DOMAIN_ARM_PRIMITIVES_ALU_H
