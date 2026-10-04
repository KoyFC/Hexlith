#ifndef HEXLITH_DOMAIN_ARM_PRIMITIVES_PARALLEL_H
#define HEXLITH_DOMAIN_ARM_PRIMITIVES_PARALLEL_H

#include "domain/arm/instruction.h"

#include <array>
#include <cstdint>

// The parallel arithmetic instructions: two halfwords or four bytes added or
// subtracted side by side in one register, the SIMD of a core that predates
// NEON.
//
// Thirty-six instructions, written once. The shape says which lane of each
// operand meets which, and whether they are added or subtracted; the prefix
// says what happens to each lane's exact result. Modular arithmetic wraps and
// sets one GE bit per byte of the result, saturating arithmetic clamps, and
// halving arithmetic keeps the top bits of the exact result.
//
// Source: the SADD16 to UHSUB8 pages of the ARMv7-A and ARMv7-R reference
// manual (DDI 0406C), which give each of the thirty-six its own pseudocode. The
// table form below is checked against all of them by the Unicorn comparison.
namespace hexlith::domain::arm::primitives {

struct ParallelResult {
    std::uint32_t value = 0;

    // GE[3:0], one bit per byte. A halfword lane sets the two bits of its two
    // bytes together. Meaningful only when the prefix is modular; the other
    // prefixes leave the flags alone, and the caller must not write this back.
    std::uint8_t greater_or_equal = 0;
};

namespace parallel_detail {

constexpr bool IsSigned(ParallelPrefix prefix) {
    return prefix == ParallelPrefix::Signed || prefix == ParallelPrefix::SignedSaturating ||
           prefix == ParallelPrefix::SignedHalving;
}

constexpr bool IsSaturating(ParallelPrefix prefix) {
    return prefix == ParallelPrefix::SignedSaturating ||
           prefix == ParallelPrefix::UnsignedSaturating;
}

constexpr bool IsHalving(ParallelPrefix prefix) {
    return prefix == ParallelPrefix::SignedHalving || prefix == ParallelPrefix::UnsignedHalving;
}

// Lane n of a value, extended to a full integer the way the prefix reads it.
constexpr std::int32_t Lane(std::uint32_t value, unsigned lane, unsigned width, bool is_signed) {
    const std::uint32_t mask = (1U << width) - 1U;
    const std::uint32_t raw = (value >> (lane * width)) & mask;
    if (!is_signed) {
        return static_cast<std::int32_t>(raw);
    }
    const std::uint32_t sign = 1U << (width - 1);
    return static_cast<std::int32_t>(raw ^ sign) - static_cast<std::int32_t>(sign);
}

// What each result lane is made of: which lane of the second operand it uses,
// and whether it is a sum or a difference. The first operand's lane is always
// the result lane's own.
struct LanePlan {
    unsigned other_lane = 0;
    bool subtract = false;
};

constexpr LanePlan Sum(unsigned other_lane) {
    return {.other_lane = other_lane, .subtract = false};
}

constexpr LanePlan Difference(unsigned other_lane) {
    return {.other_lane = other_lane, .subtract = true};
}

constexpr std::array<LanePlan, 4> Plan(ParallelShape shape) {
    switch (shape) {
    case ParallelShape::Add16:
        return {Sum(0), Sum(1)};
    case ParallelShape::AddSubtractExchange:
        // ASX: the low lane subtracts the other's high half and the high lane
        // adds its low half.
        return {Difference(1), Sum(0)};
    case ParallelShape::SubtractAddExchange:
        return {Sum(1), Difference(0)};
    case ParallelShape::Subtract16:
        return {Difference(0), Difference(1)};
    case ParallelShape::Add8:
        return {Sum(0), Sum(1), Sum(2), Sum(3)};
    case ParallelShape::Subtract8:
        return {Difference(0), Difference(1), Difference(2), Difference(3)};
    }
    return {};
}

constexpr bool IsByteShape(ParallelShape shape) {
    return shape == ParallelShape::Add8 || shape == ParallelShape::Subtract8;
}

// The GE condition for one lane in modular arithmetic. Signed: the exact result
// is not negative. Unsigned: a sum carried out of the lane, or a difference did
// not borrow.
constexpr bool
LaneGreaterOrEqual(std::int32_t exact, unsigned width, bool is_signed, bool subtract) {
    if (is_signed || subtract) {
        return exact >= 0;
    }
    return exact >= (std::int32_t{1} << width);
}

constexpr std::int32_t Clamp(std::int32_t exact, unsigned width, bool is_signed) {
    const std::int32_t maximum = is_signed ? (1 << (width - 1)) - 1 : (1 << width) - 1;
    const std::int32_t minimum = is_signed ? -(1 << (width - 1)) : 0;
    if (exact > maximum) {
        return maximum;
    }
    if (exact < minimum) {
        return minimum;
    }
    return exact;
}

}  // namespace parallel_detail

[[nodiscard]] constexpr ParallelResult ParallelArithmetic(ParallelPrefix prefix,
                                                          ParallelShape shape,
                                                          std::uint32_t first,
                                                          std::uint32_t second) {
    using namespace parallel_detail;
    constexpr unsigned ByteWidth = 8;
    constexpr unsigned HalfWidth = 16;

    const bool is_signed = IsSigned(prefix);
    const unsigned width = IsByteShape(shape) ? ByteWidth : HalfWidth;
    const unsigned lanes = IsByteShape(shape) ? 4 : 2;
    const std::uint32_t lane_mask = (1U << width) - 1U;
    const std::array<LanePlan, 4> plan = Plan(shape);

    ParallelResult result;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        const LanePlan step = plan.at(lane);
        const std::int32_t mine = Lane(first, lane, width, is_signed);
        const std::int32_t other = Lane(second, step.other_lane, width, is_signed);
        const std::int32_t exact = step.subtract ? mine - other : mine + other;

        std::int32_t kept = exact;
        if (IsSaturating(prefix)) {
            kept = Clamp(exact, width, is_signed);
        } else if (IsHalving(prefix)) {
            kept = exact >> 1;
        } else if (LaneGreaterOrEqual(exact, width, is_signed, step.subtract)) {
            // One GE bit per byte the lane covers.
            const unsigned bytes = width / ByteWidth;
            const unsigned bits = (1U << bytes) - 1U;
            result.greater_or_equal =
                static_cast<std::uint8_t>(result.greater_or_equal | (bits << (lane * bytes)));
        }

        result.value |= (static_cast<std::uint32_t>(kept) & lane_mask) << (lane * width);
    }
    return result;
}

// Whether the prefix sets the GE flags, which only modular arithmetic does.
[[nodiscard]] constexpr bool SetsGreaterOrEqual(ParallelPrefix prefix) {
    return prefix == ParallelPrefix::Signed || prefix == ParallelPrefix::Unsigned;
}

// SEL: each byte from the first operand where its GE bit is set, and from the
// second where it is clear.
[[nodiscard]] constexpr std::uint32_t
Select(std::uint32_t first, std::uint32_t second, std::uint8_t greater_or_equal) {
    constexpr unsigned ByteWidth = 8;
    constexpr std::uint32_t ByteMask = 0xFF;
    std::uint32_t result = 0;
    for (unsigned lane = 0; lane < 4; ++lane) {
        const bool from_first = ((greater_or_equal >> lane) & 1U) != 0;
        const std::uint32_t mask = ByteMask << (lane * ByteWidth);
        result |= (from_first ? first : second) & mask;
    }
    return result;
}

}  // namespace hexlith::domain::arm::primitives

#endif  // HEXLITH_DOMAIN_ARM_PRIMITIVES_PARALLEL_H
