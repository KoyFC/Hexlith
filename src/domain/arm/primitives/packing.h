#ifndef HEXLITH_DOMAIN_ARM_PRIMITIVES_PACKING_H
#define HEXLITH_DOMAIN_ARM_PRIMITIVES_PACKING_H

#include <bit>
#include <cstdint>

// Moving bytes and halfwords around inside a register: extension, packing,
// reversal, the leading zero count and the sum of absolute differences.
//
// Each of these is short enough to write at the point of use, and each has one
// detail that is easy to get wrong there, such as which half PKHTB keeps or
// that SXTB16 extends bytes 0 and 2. Writing them once is the cure.
//
// Source: the instruction pages of the ARMv7-A and ARMv7-R reference manual
// (DDI 0406C).
namespace hexlith::domain::arm::primitives {

namespace packing_detail {
constexpr unsigned ByteWidth = 8;
constexpr unsigned HalfWidth = 16;
constexpr std::uint32_t ByteMask = 0xFF;
constexpr std::uint32_t HalfMask = 0xFFFF;
}  // namespace packing_detail

// The rotation the extension instructions apply to their source first, by 0, 8,
// 16 or 24 bits.
[[nodiscard]] constexpr std::uint32_t Rotate(std::uint32_t value, unsigned amount) {
    return std::rotr(value, static_cast<int>(amount));
}

// SXTB, SXTH, UXTB and UXTH, after the rotation.
[[nodiscard]] constexpr std::uint32_t SignExtendByte(std::uint32_t value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(value)));
}

[[nodiscard]] constexpr std::uint32_t SignExtendHalf(std::uint32_t value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
}

[[nodiscard]] constexpr std::uint32_t ZeroExtendByte(std::uint32_t value) {
    return value & packing_detail::ByteMask;
}

[[nodiscard]] constexpr std::uint32_t ZeroExtendHalf(std::uint32_t value) {
    return value & packing_detail::HalfMask;
}

// SXTB16 and UXTB16: bytes 0 and 2 of the rotated value, each extended into its
// own halfword of the result.
[[nodiscard]] constexpr std::uint32_t SignExtendBytePair(std::uint32_t value) {
    using namespace packing_detail;
    const std::uint32_t low = SignExtendByte(value) & HalfMask;
    const std::uint32_t high = SignExtendByte(value >> HalfWidth) & HalfMask;
    return low | (high << HalfWidth);
}

[[nodiscard]] constexpr std::uint32_t ZeroExtendBytePair(std::uint32_t value) {
    using namespace packing_detail;
    return value & ((ByteMask << HalfWidth) | ByteMask);
}

// SXTAB16 and UXTAB16 add each extended byte to the matching halfword of the
// addend, with each halfword wrapping on its own.
[[nodiscard]] constexpr std::uint32_t AddHalves(std::uint32_t addend, std::uint32_t extended) {
    using namespace packing_detail;
    const std::uint32_t low = (addend + extended) & HalfMask;
    const std::uint32_t high = ((addend >> HalfWidth) + (extended >> HalfWidth)) & HalfMask;
    return low | (high << HalfWidth);
}

// PKHBT: the bottom half of the first operand and the top half of the shifted
// second one.
[[nodiscard]] constexpr std::uint32_t PackBottomTop(std::uint32_t first, std::uint32_t shifted) {
    using namespace packing_detail;
    return (first & HalfMask) | (shifted & (HalfMask << HalfWidth));
}

// PKHTB: the top half of the first operand and the bottom half of the shifted
// second one.
[[nodiscard]] constexpr std::uint32_t PackTopBottom(std::uint32_t first, std::uint32_t shifted) {
    using namespace packing_detail;
    return (first & (HalfMask << HalfWidth)) | (shifted & HalfMask);
}

// REV: the four bytes in the opposite order. std::byteswap would do, and it is
// C++23.
[[nodiscard]] constexpr std::uint32_t ReverseBytes(std::uint32_t value) {
    constexpr std::uint32_t EvenBytes = 0x00FF00FF;
    using packing_detail::ByteWidth;
    using packing_detail::HalfWidth;
    const std::uint32_t swapped =
        ((value & EvenBytes) << ByteWidth) | ((value >> ByteWidth) & EvenBytes);
    return std::rotr(swapped, static_cast<int>(HalfWidth));
}

// REV16: the two bytes of each halfword swapped, the halfwords left in place.
[[nodiscard]] constexpr std::uint32_t ReverseBytesInHalves(std::uint32_t value) {
    constexpr std::uint32_t EvenBytes = 0x00FF00FF;
    using packing_detail::ByteWidth;
    return ((value & EvenBytes) << ByteWidth) | ((value >> ByteWidth) & EvenBytes);
}

// REVSH: the bottom halfword byte swapped and then sign extended.
[[nodiscard]] constexpr std::uint32_t ReverseSignedHalf(std::uint32_t value) {
    using namespace packing_detail;
    const std::uint32_t swapped =
        ((value & ByteMask) << ByteWidth) | ((value >> ByteWidth) & ByteMask);
    return SignExtendHalf(swapped);
}

// CLZ.
[[nodiscard]] constexpr std::uint32_t CountLeadingZeros(std::uint32_t value) {
    return static_cast<std::uint32_t>(std::countl_zero(value));
}

// USAD8 and USADA8: the sum of the absolute differences of the four unsigned
// byte pairs, added to an accumulator that is zero for USAD8.
[[nodiscard]] constexpr std::uint32_t
SumOfAbsoluteDifferences(std::uint32_t first, std::uint32_t second, std::uint32_t accumulator) {
    using namespace packing_detail;
    std::uint32_t sum = accumulator;
    for (unsigned lane = 0; lane < 4; ++lane) {
        const auto x = static_cast<std::int32_t>((first >> (lane * ByteWidth)) & ByteMask);
        const auto y = static_cast<std::int32_t>((second >> (lane * ByteWidth)) & ByteMask);
        sum += static_cast<std::uint32_t>(x > y ? x - y : y - x);
    }
    return sum;
}

}  // namespace hexlith::domain::arm::primitives

#endif  // HEXLITH_DOMAIN_ARM_PRIMITIVES_PACKING_H
