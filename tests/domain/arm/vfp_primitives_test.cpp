#include "domain/arm/primitives/vfp.h"

#include <bit>
#include <cstdint>
#include <gtest/gtest.h>

namespace hexlith::domain::arm::primitives {
namespace {

// The edges of floating point where the host and the ARM11 answer differently
// unless the library intervenes. The ordinary arithmetic is the host's and is
// covered by the Unicorn comparison rather than by examples here.

constexpr std::uint32_t RoundToNearest = 0;
constexpr std::uint32_t FlushToZero = 1U << 24;
constexpr std::uint32_t DefaultNanMode = 1U << 25;

constexpr std::uint32_t One = 0x3F800000;
constexpr std::uint32_t Two = 0x40000000;
constexpr std::uint32_t Infinity = 0x7F800000;
constexpr std::uint32_t QuietNan = 0x7FC00001;
constexpr std::uint32_t SignallingNan = 0x7F800001;
constexpr std::uint32_t SmallestDenormal = 0x00000001;
constexpr std::uint32_t NegativeDenormal = 0x80000001;

TEST(VfpPrimitives, AddsLikeTheHost) {
    EXPECT_EQ(FloatAdd<float>(One, Two, RoundToNearest), 0x40400000U);
    EXPECT_EQ(FloatAdd<double>(std::bit_cast<std::uint64_t>(1.5),
                               std::bit_cast<std::uint64_t>(2.25),
                               RoundToNearest),
              std::bit_cast<std::uint64_t>(3.75));
}

TEST(VfpPrimitives, AnswersTheDefaultNanForAnInvalidOperation) {
    // Infinity minus infinity. x86 answers 0xFFC00000; ARM answers 0x7FC00000.
    EXPECT_EQ(FloatSubtract<float>(Infinity, Infinity, RoundToNearest), 0x7FC00000U);
    EXPECT_EQ(FloatSquareRoot<float>(0xBF800000, RoundToNearest), 0x7FC00000U);
}

TEST(VfpPrimitives, PrefersASignallingNanAndQuietsIt) {
    // The quiet NaN comes first, the signalling one second, and the signalling
    // one still wins.
    EXPECT_EQ(FloatAdd<float>(QuietNan, SignallingNan | 0x2, RoundToNearest), 0x7FC00003U);
    EXPECT_EQ(FloatAdd<float>(SignallingNan | 0x4, QuietNan, RoundToNearest), 0x7FC00005U);

    // With two quiet NaNs, the first operand wins.
    EXPECT_EQ(FloatMultiply<float>(0xFFC00005, QuietNan, RoundToNearest), 0xFFC00005U);
}

TEST(VfpPrimitives, AnswersTheDefaultNanForEveryNanInDefaultNanMode) {
    EXPECT_EQ(FloatAdd<float>(QuietNan, One, DefaultNanMode), 0x7FC00000U);
    EXPECT_EQ(FloatWiden(0xFFC00123, DefaultNanMode), 0x7FF8000000000000ULL);
}

TEST(VfpPrimitives, FlushesDenormalsInFlushToZeroMode) {
    // A denormal operand reads as zero.
    EXPECT_EQ(FloatAdd<float>(SmallestDenormal, 0, FlushToZero), 0U);
    EXPECT_EQ(FloatAdd<float>(SmallestDenormal, 0, RoundToNearest), SmallestDenormal);

    // A denormal result is written as zero, keeping its sign.
    EXPECT_EQ(FloatMultiply<float>(0x80800000, 0x3F000000, FlushToZero), 0x80000000U);

    // A comparison sees the flushed value too, so a denormal equals zero.
    EXPECT_EQ(FloatCompare<float>(NegativeDenormal, 0, FlushToZero), fpscr::Equal);
}

TEST(VfpPrimitives, LeavesNegationAndAbsoluteValueBitwise) {
    EXPECT_EQ(FloatNegate<float>(SignallingNan), 0xFF800001U) << "no quieting";
    EXPECT_EQ(FloatAbsolute<float>(NegativeDenormal), SmallestDenormal) << "no flushing";
}

TEST(VfpPrimitives, RoundsTheProductBeforeTheSum) {
    // (1 + 2^-23) squared needs more precision than a float has. Fused, the sum
    // with -1 would keep the low bit of the exact square, 2^-46; rounded first,
    // it does not.
    constexpr std::uint32_t OnePlusUlp = 0x3F800001;
    constexpr std::uint32_t MinusOne = 0xBF800000;
    const std::uint32_t result = FloatMultiplyAccumulate<float>(
        MinusOne, OnePlusUlp, OnePlusUlp, false, false, RoundToNearest);
    EXPECT_EQ(result, 0x34800000U) << "2^-22, the rounded product minus one";
}

TEST(VfpPrimitives, ComparesIntoFlags) {
    EXPECT_EQ(FloatCompare<float>(One, Two, RoundToNearest), fpscr::Less);
    EXPECT_EQ(FloatCompare<float>(Two, One, RoundToNearest), fpscr::Greater);
    EXPECT_EQ(FloatCompare<float>(0x80000000, 0, RoundToNearest), fpscr::Equal) << "-0 is 0";
    EXPECT_EQ(FloatCompare<float>(QuietNan, One, RoundToNearest), fpscr::Unordered);
}

TEST(VfpPrimitives, KeepsTheNanPayloadAcrossPrecisions) {
    EXPECT_EQ(FloatWiden(0xFF800001, RoundToNearest), 0xFFF8000020000000ULL);
    EXPECT_EQ(FloatNarrow(0x7FF0000020000000ULL, RoundToNearest), 0x7FC00001U);
}

TEST(VfpPrimitives, ConvertsToIntegersBothWays) {
    constexpr std::uint32_t TwoAndAHalf = 0x40200000;
    constexpr std::uint32_t ThreeAndAHalf = 0x40600000;

    EXPECT_EQ(FloatToInteger<float>(TwoAndAHalf, true, true, RoundToNearest), 2U);
    EXPECT_EQ(FloatToInteger<float>(TwoAndAHalf, true, false, RoundToNearest), 2U)
        << "ties to even";
    EXPECT_EQ(FloatToInteger<float>(ThreeAndAHalf, true, false, RoundToNearest), 4U);
    EXPECT_EQ(FloatToInteger<float>(0xC0200000, true, false, RoundToNearest),
              static_cast<std::uint32_t>(-2));

    // Saturation at both ends, and a NaN converts to zero.
    EXPECT_EQ(FloatToInteger<float>(0x4F800000, true, true, RoundToNearest), 0x7FFFFFFFU);
    EXPECT_EQ(FloatToInteger<float>(0xBF800000, false, true, RoundToNearest), 0U);
    EXPECT_EQ(FloatToInteger<float>(QuietNan, true, true, RoundToNearest), 0U);

    EXPECT_EQ(IntegerToFloat<float>(0xFFFFFFFF, true), 0xBF800000U);
    EXPECT_EQ(IntegerToFloat<float>(0xFFFFFFFF, false), 0x4F800000U);
}

TEST(VfpPrimitives, RefusesTheModesItDoesNotImplement) {
    EXPECT_TRUE(IsSupportedFloatMode(FlushToZero | DefaultNanMode));
    EXPECT_FALSE(IsSupportedFloatMode(1U << 22)) << "round towards plus infinity";
    EXPECT_FALSE(IsSupportedFloatMode(1U << 16)) << "a short vector length";
    EXPECT_FALSE(IsSupportedFloatMode(1U << 8)) << "the invalid operation trap";
}

}  // namespace
}  // namespace hexlith::domain::arm::primitives
