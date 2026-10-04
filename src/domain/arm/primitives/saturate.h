#ifndef HEXLITH_DOMAIN_ARM_PRIMITIVES_SATURATE_H
#define HEXLITH_DOMAIN_ARM_PRIMITIVES_SATURATE_H

#include <cstdint>

// Saturation: clamping a result into a range instead of letting it wrap, and
// reporting whether it had to. The report is what sets the sticky Q flag.
//
// Sources: SignedSatQ and UnsignedSatQ in the ARMv7-A and ARMv7-R reference
// manual (DDI 0406C), and the instruction pages that use them.
namespace hexlith::domain::arm::primitives {

struct SaturateResult {
    std::uint32_t value = 0;
    bool saturated = false;
};

// SignedSatQ: the value clamped into a signed range of the given bit count,
// from 1 to 32.
[[nodiscard]] constexpr SaturateResult SignedSaturate(std::int64_t value, unsigned bits) {
    const std::int64_t maximum = (std::int64_t{1} << (bits - 1)) - 1;
    const std::int64_t minimum = -(std::int64_t{1} << (bits - 1));
    if (value > maximum) {
        return {.value = static_cast<std::uint32_t>(maximum), .saturated = true};
    }
    if (value < minimum) {
        return {.value = static_cast<std::uint32_t>(minimum), .saturated = true};
    }
    return {.value = static_cast<std::uint32_t>(value), .saturated = false};
}

// UnsignedSatQ: the value clamped into an unsigned range of the given bit
// count, from 0 to 31.
[[nodiscard]] constexpr SaturateResult UnsignedSaturate(std::int64_t value, unsigned bits) {
    const std::int64_t maximum = (std::int64_t{1} << bits) - 1;
    if (value > maximum) {
        return {.value = static_cast<std::uint32_t>(maximum), .saturated = true};
    }
    if (value < 0) {
        return {.value = 0, .saturated = true};
    }
    return {.value = static_cast<std::uint32_t>(value), .saturated = false};
}

[[nodiscard]] constexpr std::int64_t Signed(std::uint32_t value) {
    return static_cast<std::int32_t>(value);
}

// QADD and QSUB.
[[nodiscard]] constexpr SaturateResult SaturatingAdd(std::uint32_t x, std::uint32_t y) {
    constexpr unsigned Word = 32;
    return SignedSaturate(Signed(x) + Signed(y), Word);
}

[[nodiscard]] constexpr SaturateResult SaturatingSubtract(std::uint32_t x, std::uint32_t y) {
    constexpr unsigned Word = 32;
    return SignedSaturate(Signed(x) - Signed(y), Word);
}

// QDADD and QDSUB: the second operand is doubled with saturation first, and
// either saturation sets Q.
[[nodiscard]] constexpr SaturateResult SaturatingDoubleAdd(std::uint32_t x, std::uint32_t y) {
    constexpr unsigned Word = 32;
    const SaturateResult doubled = SignedSaturate(2 * Signed(y), Word);
    const SaturateResult sum = SignedSaturate(Signed(x) + Signed(doubled.value), Word);
    return {.value = sum.value, .saturated = doubled.saturated || sum.saturated};
}

[[nodiscard]] constexpr SaturateResult SaturatingDoubleSubtract(std::uint32_t x, std::uint32_t y) {
    constexpr unsigned Word = 32;
    const SaturateResult doubled = SignedSaturate(2 * Signed(y), Word);
    const SaturateResult difference = SignedSaturate(Signed(x) - Signed(doubled.value), Word);
    return {.value = difference.value, .saturated = doubled.saturated || difference.saturated};
}

// SSAT16 and USAT16: each halfword saturated on its own, into the given bit
// count.
[[nodiscard]] constexpr SaturateResult SignedSaturateHalves(std::uint32_t value, unsigned bits) {
    constexpr unsigned Half = 16;
    constexpr std::uint32_t HalfMask = 0xFFFF;
    const SaturateResult low = SignedSaturate(static_cast<std::int16_t>(value & HalfMask), bits);
    const SaturateResult high = SignedSaturate(static_cast<std::int16_t>(value >> Half), bits);
    return {.value = (low.value & HalfMask) | (high.value << Half),
            .saturated = low.saturated || high.saturated};
}

[[nodiscard]] constexpr SaturateResult UnsignedSaturateHalves(std::uint32_t value, unsigned bits) {
    constexpr unsigned Half = 16;
    constexpr std::uint32_t HalfMask = 0xFFFF;
    const SaturateResult low = UnsignedSaturate(static_cast<std::int16_t>(value & HalfMask), bits);
    const SaturateResult high = UnsignedSaturate(static_cast<std::int16_t>(value >> Half), bits);
    return {.value = (low.value & HalfMask) | (high.value << Half),
            .saturated = low.saturated || high.saturated};
}

}  // namespace hexlith::domain::arm::primitives

#endif  // HEXLITH_DOMAIN_ARM_PRIMITIVES_SATURATE_H
