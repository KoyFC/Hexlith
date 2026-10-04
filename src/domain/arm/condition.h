#ifndef HEXLITH_DOMAIN_ARM_CONDITION_H
#define HEXLITH_DOMAIN_ARM_CONDITION_H

#include <cstdint>
#include <string_view>

namespace hexlith::domain::arm {

// The four-bit condition every ARM instruction carries, in encoding order.
//
// Conditional execution is why control flow analysis cannot stop at a branch.
// A conditional branch leaves two successors, and a conditional instruction in
// the middle of a block is not a successor at all, so the two cases have to be
// told apart by the condition rather than by the operation.
enum class Condition : std::uint8_t {
    Equal = 0,
    NotEqual = 1,
    CarrySet = 2,
    CarryClear = 3,
    Negative = 4,
    NotNegative = 5,
    Overflow = 6,
    NoOverflow = 7,
    Higher = 8,
    LowerOrSame = 9,
    GreaterOrEqual = 10,
    Less = 11,
    Greater = 12,
    LessOrEqual = 13,
    Always = 14,

    // Encoding 0b1111. ARMv4 called it never and deprecated it; from ARMv5 it
    // marks a group of instructions that are not conditional at all, BLX to an
    // immediate among them. So it is not a condition that fails, it is a sign
    // that the rest of the word is decoded differently.
    Unconditional = 15,
};

// The suffix the assembler spells, and an empty string for Always, because
// `beq` and `b` are the same mnemonic with and without one.
std::string_view ConditionSuffix(Condition condition);

[[nodiscard]] constexpr bool IsConditional(Condition condition) {
    return condition != Condition::Always && condition != Condition::Unconditional;
}

}  // namespace hexlith::domain::arm

#endif  // HEXLITH_DOMAIN_ARM_CONDITION_H
