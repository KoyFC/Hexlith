#ifndef HEXLITH_DOMAIN_ARM_PRIMITIVES_MULTIPLY_H
#define HEXLITH_DOMAIN_ARM_PRIMITIVES_MULTIPLY_H

#include "domain/arm/primitives/saturate.h"

#include <cstdint>

// The multiplies whose arithmetic is more than a host multiplication: the
// halfword multiplies ARMv5TE added for signal processing, the dual multiplies
// and most significant word multiplies ARMv6 added, and the 64-bit forms.
//
// A plain MUL or UMULL is a host multiplication and needs nothing here. What
// these add is which half of an operand is read, where the result is rounded,
// and when the Q flag is set, and those are the details a lifter would get
// subtly wrong if it spelled them out at each use.
//
// Sources: the instruction pages of the ARMv7-A and ARMv7-R reference manual
// (DDI 0406C), whose pseudocode computes each result as an unbounded integer
// and then takes bits of it. The functions below do the same in 64 bits, or in
// unsigned 64-bit arithmetic where the true value would not fit in a signed one
// and only the high word is wanted.
namespace hexlith::domain::arm::primitives {

// One halfword of a register, sign extended: the high half when asked for, and
// the low half otherwise.
[[nodiscard]] constexpr std::int32_t Half(std::uint32_t value, bool high) {
    constexpr unsigned HalfWidth = 16;
    return static_cast<std::int16_t>(high ? value >> HalfWidth : value);
}

// A 32-bit result that may have overflowed, which is what sets Q for the
// accumulating multiplies. The value is the low 32 bits either way: these
// instructions wrap and report, they do not saturate.
struct AccumulateResult {
    std::uint32_t value = 0;
    bool overflowed = false;
};

[[nodiscard]] constexpr AccumulateResult Wrapped(std::int64_t exact) {
    const auto low = static_cast<std::int32_t>(exact);
    return {.value = static_cast<std::uint32_t>(low), .overflowed = low != exact};
}

// SMULxy: a halfword times a halfword. The product always fits in 32 bits.
[[nodiscard]] constexpr std::uint32_t
MultiplyHalves(std::uint32_t x, bool x_high, std::uint32_t y, bool y_high) {
    return static_cast<std::uint32_t>(Half(x, x_high) * Half(y, y_high));
}

// SMLAxy: the same, plus an accumulator, setting Q on overflow.
[[nodiscard]] constexpr AccumulateResult MultiplyHalvesAccumulate(
    std::uint32_t x, bool x_high, std::uint32_t y, bool y_high, std::uint32_t accumulator) {
    return Wrapped((static_cast<std::int64_t>(Half(x, x_high)) * Half(y, y_high)) +
                   Signed(accumulator));
}

// SMULWy: a word times a halfword, keeping the top 32 bits of the 48-bit
// product.
[[nodiscard]] constexpr std::uint32_t
MultiplyWordByHalf(std::uint32_t x, std::uint32_t y, bool y_high) {
    constexpr unsigned HalfWidth = 16;
    return static_cast<std::uint32_t>((Signed(x) * Half(y, y_high)) >> HalfWidth);
}

// SMLAWy: the same, plus an accumulator, setting Q on overflow.
[[nodiscard]] constexpr AccumulateResult MultiplyWordByHalfAccumulate(std::uint32_t x,
                                                                      std::uint32_t y,
                                                                      bool y_high,
                                                                      std::uint32_t accumulator) {
    constexpr unsigned HalfWidth = 16;
    return Wrapped(((Signed(x) * Half(y, y_high)) >> HalfWidth) + Signed(accumulator));
}

// SMLALxy: a halfword times a halfword added to a 64-bit accumulator, with no
// flag at all.
[[nodiscard]] constexpr std::uint64_t MultiplyHalvesAccumulateLong(
    std::uint32_t x, bool x_high, std::uint32_t y, bool y_high, std::uint64_t accumulator) {
    return accumulator +
           static_cast<std::uint64_t>(static_cast<std::int64_t>(Half(x, x_high)) * Half(y, y_high));
}

// The two products of a dual multiply. With exchange set, the halves of the
// second operand swap first, which is the X suffix.
struct DualProducts {
    std::int64_t low = 0;
    std::int64_t high = 0;
};

[[nodiscard]] constexpr DualProducts Products(std::uint32_t x, std::uint32_t y, bool exchange) {
    return {.low = static_cast<std::int64_t>(Half(x, false)) * Half(y, exchange),
            .high = static_cast<std::int64_t>(Half(x, true)) * Half(y, !exchange)};
}

// SMUAD and SMLAD: the sum of the two products, plus an accumulator for SMLAD,
// setting Q on overflow. SMUAD is SMLAD with an accumulator of zero, which is
// how the reference manual writes it too.
[[nodiscard]] constexpr AccumulateResult
DualMultiplyAdd(std::uint32_t x, std::uint32_t y, bool exchange, std::uint32_t accumulator) {
    const DualProducts products = Products(x, y, exchange);
    return Wrapped(products.low + products.high + Signed(accumulator));
}

// SMUSD and SMLSD: the low product minus the high one. SMUSD cannot overflow and
// never sets Q, which this still reports correctly as no overflow.
[[nodiscard]] constexpr AccumulateResult
DualMultiplySubtract(std::uint32_t x, std::uint32_t y, bool exchange, std::uint32_t accumulator) {
    const DualProducts products = Products(x, y, exchange);
    return Wrapped(products.low - products.high + Signed(accumulator));
}

// SMLALD and SMLSLD: the same into a 64-bit accumulator, with no flag.
[[nodiscard]] constexpr std::uint64_t
DualMultiplyAddLong(std::uint32_t x, std::uint32_t y, bool exchange, std::uint64_t accumulator) {
    const DualProducts products = Products(x, y, exchange);
    return accumulator + static_cast<std::uint64_t>(products.low + products.high);
}

[[nodiscard]] constexpr std::uint64_t DualMultiplySubtractLong(std::uint32_t x,
                                                               std::uint32_t y,
                                                               bool exchange,
                                                               std::uint64_t accumulator) {
    const DualProducts products = Products(x, y, exchange);
    return accumulator + static_cast<std::uint64_t>(products.low - products.high);
}

// SMMUL, SMMLA and SMMLS: the top word of a 64-bit product, optionally with an
// accumulator in the top word, optionally rounded. The R suffix rounds by adding
// half of the bottom word before it is discarded.
//
// The exact value can exceed a signed 64-bit range once the accumulator is
// shifted up, so this works in unsigned arithmetic, which wraps, and takes the
// top word. The top word of the true value and of the wrapped one are the same.
[[nodiscard]] constexpr std::uint32_t MostSignificantWord(
    std::uint32_t x, std::uint32_t y, std::uint32_t accumulator, bool subtract, bool round) {
    constexpr unsigned Word = 32;
    constexpr std::uint64_t RoundingHalf = 0x80000000;
    const auto product = static_cast<std::uint64_t>(Signed(x) * Signed(y));
    const std::uint64_t top = static_cast<std::uint64_t>(accumulator) << Word;
    std::uint64_t sum = subtract ? top - product : top + product;
    if (round) {
        sum += RoundingHalf;
    }
    return static_cast<std::uint32_t>(sum >> Word);
}

// UMAAL: an unsigned product plus two unsigned accumulators. The largest
// possible result fits in 64 bits exactly, which is the point of the
// instruction.
[[nodiscard]] constexpr std::uint64_t MultiplyAccumulateAccumulate(std::uint32_t x,
                                                                   std::uint32_t y,
                                                                   std::uint32_t low,
                                                                   std::uint32_t high) {
    return (static_cast<std::uint64_t>(x) * y) + low + high;
}

}  // namespace hexlith::domain::arm::primitives

#endif  // HEXLITH_DOMAIN_ARM_PRIMITIVES_MULTIPLY_H
