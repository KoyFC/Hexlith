#include "domain/arm/cpu_state.h"
#include "domain/arm/primitives/integer.h"
#include "domain/arm/primitives/multiply.h"
#include "domain/arm/primitives/packing.h"
#include "domain/arm/primitives/parallel.h"
#include "domain/arm/primitives/saturate.h"

#include <cstdint>
#include <gtest/gtest.h>

namespace hexlith::domain::arm::primitives {
namespace {

// These pin down the corners the reference manual's pseudocode spells out. The
// Unicorn comparison covers the rest of the space against an independent
// implementation; what is here is what a reader should be able to check by eye.

TEST(Shift, PassesTheCarryThroughAShiftOfZero) {
    const ShiftResult result = Shift(ShiftKind::LogicalLeft, 0x80000001, 0, true);
    EXPECT_EQ(result.value, 0x80000001U);
    EXPECT_TRUE(result.carry);
}

TEST(Shift, TakesTheCarryFromTheLastBitShiftedOut) {
    EXPECT_EQ(Shift(ShiftKind::LogicalLeft, 0x80000001, 1, false).value, 0x00000002U);
    EXPECT_TRUE(Shift(ShiftKind::LogicalLeft, 0x80000001, 1, false).carry);
    EXPECT_TRUE(Shift(ShiftKind::LogicalRight, 0x00000003, 1, false).carry);
    EXPECT_FALSE(Shift(ShiftKind::LogicalRight, 0x00000002, 1, true).carry);
}

TEST(Shift, ShiftsByThirtyTwoAndBeyond) {
    // LSL #32 keeps bit 0 as the carry, and anything longer clears it.
    EXPECT_EQ(Shift(ShiftKind::LogicalLeft, 0x00000001, 32, false).value, 0U);
    EXPECT_TRUE(Shift(ShiftKind::LogicalLeft, 0x00000001, 32, false).carry);
    EXPECT_FALSE(Shift(ShiftKind::LogicalLeft, 0x00000001, 33, true).carry);

    // LSR #32, which is also what LSR #0 means as an immediate.
    EXPECT_EQ(Shift(ShiftKind::LogicalRight, 0x80000000, 32, false).value, 0U);
    EXPECT_TRUE(Shift(ShiftKind::LogicalRight, 0x80000000, 32, false).carry);

    // ASR by 32 or more fills with the sign.
    const ShiftResult negative = Shift(ShiftKind::ArithmeticRight, 0x80000000, 200, false);
    EXPECT_EQ(negative.value, 0xFFFFFFFFU);
    EXPECT_TRUE(negative.carry);
}

TEST(Shift, RotatesByAMultipleOfThirtyTwoAsNoRotationWithACarry) {
    const ShiftResult result = Shift(ShiftKind::RotateRight, 0x80000000, 64, false);
    EXPECT_EQ(result.value, 0x80000000U);
    EXPECT_TRUE(result.carry) << "bit 31 becomes the carry";
}

TEST(Shift, RotatesThroughTheCarry) {
    const ShiftResult result = Shift(ShiftKind::RotateRightExtended, 0x00000003, 1, true);
    EXPECT_EQ(result.value, 0x80000001U);
    EXPECT_TRUE(result.carry);
}

TEST(AddWithCarry, SetsCarryOnUnsignedOverflow) {
    const AddResult result = AddWithCarry(0xFFFFFFFF, 1, false);
    EXPECT_EQ(result.value, 0U);
    EXPECT_TRUE(result.carry);
    EXPECT_FALSE(result.overflow);
}

TEST(AddWithCarry, SetsOverflowOnSignedOverflow) {
    const AddResult result = AddWithCarry(0x7FFFFFFF, 1, false);
    EXPECT_EQ(result.value, 0x80000000U);
    EXPECT_FALSE(result.carry);
    EXPECT_TRUE(result.overflow);
}

TEST(AddWithCarry, SubtractsWithCarryMeaningNoBorrow) {
    // 5 - 3 as AddWithCarry(5, ~3, 1): no borrow, so the carry is set.
    const AddResult no_borrow = AddWithCarry(5, ~3U, true);
    EXPECT_EQ(no_borrow.value, 2U);
    EXPECT_TRUE(no_borrow.carry);

    // 3 - 5 borrows, so the carry is clear.
    const AddResult borrow = AddWithCarry(3, ~5U, true);
    EXPECT_EQ(borrow.value, 0xFFFFFFFEU);
    EXPECT_FALSE(borrow.carry);
}

TEST(ConditionPassed, ReadsTheSignedComparisons) {
    const ConditionFlags less{.negative = true, .zero = false, .carry = false, .overflow = false};
    EXPECT_TRUE(ConditionPassed(Condition::Less, less));
    EXPECT_FALSE(ConditionPassed(Condition::GreaterOrEqual, less));

    // N and V both set is greater or equal, which is what a signed overflow
    // into a negative result means.
    const ConditionFlags overflowed{
        .negative = true, .zero = false, .carry = false, .overflow = true};
    EXPECT_TRUE(ConditionPassed(Condition::GreaterOrEqual, overflowed));
    EXPECT_TRUE(ConditionPassed(Condition::Greater, overflowed));
}

TEST(ConditionPassed, ReadsTheUnsignedComparisons) {
    const ConditionFlags higher{.negative = false, .zero = false, .carry = true, .overflow = false};
    EXPECT_TRUE(ConditionPassed(Condition::Higher, higher));
    EXPECT_FALSE(ConditionPassed(Condition::LowerOrSame, higher));

    const ConditionFlags same{.negative = false, .zero = true, .carry = true, .overflow = false};
    EXPECT_FALSE(ConditionPassed(Condition::Higher, same));
    EXPECT_TRUE(ConditionPassed(Condition::LowerOrSame, same));
}

TEST(Saturate, ClampsAndReports) {
    EXPECT_EQ(SaturatingAdd(0x7FFFFFFF, 1).value, 0x7FFFFFFFU);
    EXPECT_TRUE(SaturatingAdd(0x7FFFFFFF, 1).saturated);
    EXPECT_EQ(SaturatingSubtract(0x80000000, 1).value, 0x80000000U);
    EXPECT_FALSE(SaturatingAdd(1, 2).saturated);

    // QDADD saturates the doubling on its own, and reports it even when the sum
    // that follows does not saturate.
    const SaturateResult doubled = SaturatingDoubleAdd(0x80000000, 0x40000000);
    EXPECT_EQ(doubled.value, 0xFFFFFFFFU);
    EXPECT_TRUE(doubled.saturated);
}

TEST(Saturate, ClampsToABitCount) {
    EXPECT_EQ(SignedSaturate(200, 8).value, 127U);
    EXPECT_EQ(SignedSaturate(-200, 8).value, static_cast<std::uint32_t>(-128));
    EXPECT_EQ(UnsignedSaturate(-1, 8).value, 0U);
    EXPECT_EQ(UnsignedSaturate(300, 8).value, 255U);
    EXPECT_EQ(UnsignedSaturate(5, 0).value, 0U) << "a bound of zero bits allows only zero";
}

TEST(Multiply, ReadsTheHalvesItIsAskedFor) {
    // The top half of the first operand, -2, times the bottom half of the
    // second, 3.
    EXPECT_EQ(MultiplyHalves(0xFFFE0000, true, 0x00000003, false), static_cast<std::uint32_t>(-6));
}

TEST(Multiply, SetsQWhenADualSumOverflows) {
    // Both products are 0x8000 * 0x8000 = 0x40000000, and their sum does not fit.
    const AccumulateResult result = DualMultiplyAdd(0x80008000, 0x80008000, false, 0);
    EXPECT_EQ(result.value, 0x80000000U);
    EXPECT_TRUE(result.overflowed);
}

TEST(Multiply, RoundsTheMostSignificantWord) {
    // 0x40000000 * 3 = 0xC0000000: a top word of 0, and half of the bottom word
    // or more, so rounding carries one into it.
    EXPECT_EQ(MostSignificantWord(0x40000000, 3, 0, false, false), 0U);
    EXPECT_EQ(MostSignificantWord(0x40000000, 3, 0, false, true), 1U);

    // SMMLS with an accumulator at the top of the range, which overflows a
    // signed 64-bit intermediate if computed naively.
    EXPECT_EQ(MostSignificantWord(0x80000000, 0x7FFFFFFF, 0x7FFFFFFF, true, false), 0xBFFFFFFEU);
}

TEST(Multiply, AddsTwoAccumulatorsWithoutOverflow) {
    EXPECT_EQ(MultiplyAccumulateAccumulate(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF),
              0xFFFFFFFFFFFFFFFFULL);
}

TEST(Parallel, SetsGreaterOrEqualPerLane) {
    // uadd8: 0xFF + 0x01 carries out of the low byte, the others do not.
    const ParallelResult added =
        ParallelArithmetic(ParallelPrefix::Unsigned, ParallelShape::Add8, 0x010203FF, 0x01010101);
    EXPECT_EQ(added.value, 0x02030400U);
    EXPECT_EQ(added.greater_or_equal, 0b0001);

    // ssub16: the low lane goes negative, the high lane does not.
    const ParallelResult subtracted = ParallelArithmetic(
        ParallelPrefix::Signed, ParallelShape::Subtract16, 0x00050001, 0x00030002);
    EXPECT_EQ(subtracted.value, 0x0002FFFFU);
    EXPECT_EQ(subtracted.greater_or_equal, 0b1100);
}

TEST(Parallel, ExchangesHalvesInTheCrossedShapes) {
    // uasx: low = 10 - 1 (the other's high half), high = 20 + 2 (its low half).
    const ParallelResult result = ParallelArithmetic(
        ParallelPrefix::Unsigned, ParallelShape::AddSubtractExchange, 0x0014000A, 0x00010002);
    EXPECT_EQ(result.value, 0x00160009U);
}

TEST(Parallel, SaturatesAndHalves) {
    EXPECT_EQ(ParallelArithmetic(
                  ParallelPrefix::SignedSaturating, ParallelShape::Add16, 0x7FFF8000, 0x00018000)
                  .value,
              0x7FFF8000U);
    EXPECT_EQ(ParallelArithmetic(
                  ParallelPrefix::UnsignedHalving, ParallelShape::Add8, 0xFF000001, 0xFF000001)
                  .value,
              0xFF000001U);
    EXPECT_FALSE(SetsGreaterOrEqual(ParallelPrefix::SignedSaturating));
}

TEST(Parallel, SelectsBytesByTheFlags) {
    EXPECT_EQ(Select(0xAAAAAAAA, 0xBBBBBBBB, 0b0101), 0xBBAABBAAU);
}

TEST(Packing, ExtendsAndPacks) {
    EXPECT_EQ(SignExtendByte(Rotate(0x0000FF00, 8)), 0xFFFFFFFFU);
    EXPECT_EQ(SignExtendBytePair(0x00800080), 0xFF80FF80U);
    EXPECT_EQ(ZeroExtendBytePair(0x12345678), 0x00340078U);
    EXPECT_EQ(AddHalves(0xFFFF0001, 0x00010001), 0x00000002U) << "each half wraps on its own";
    EXPECT_EQ(PackBottomTop(0x11112222, 0x33334444), 0x33332222U);
    EXPECT_EQ(PackTopBottom(0x11112222, 0x33334444), 0x11114444U);
}

TEST(Packing, Reverses) {
    EXPECT_EQ(ReverseBytes(0x12345678), 0x78563412U);
    EXPECT_EQ(ReverseBytesInHalves(0x12345678), 0x34127856U);
    EXPECT_EQ(ReverseSignedHalf(0x00000080), 0xFFFF8000U);
}

TEST(Packing, CountsAndSums) {
    EXPECT_EQ(CountLeadingZeros(0), 32U);
    EXPECT_EQ(CountLeadingZeros(1), 31U);
    EXPECT_EQ(SumOfAbsoluteDifferences(0x0A000005, 0x0500000A, 100), 110U);
}

TEST(CpuState, PacksTheStatusRegisterInUserMode) {
    CpuState state;
    state.negative = true;
    state.carry = true;
    state.greater_or_equal = 0b1010;
    state.thumb = true;
    EXPECT_EQ(PackCpsr(state), 0xA00A0030U);
}

TEST(CpuState, WritesOnlyTheBytesTheMaskNames) {
    CpuState state;
    WriteCpsrFromUserMode(state, 0xF80F00FF, 0b1000);
    EXPECT_TRUE(state.negative && state.zero && state.carry && state.overflow && state.saturated);
    EXPECT_EQ(state.greater_or_equal, 0) << "the GE byte was not in the mask";
    EXPECT_FALSE(state.thumb) << "user mode cannot write the execution state";
}

TEST(CpuState, OverlapsDoublesWithSingles) {
    CpuState state;
    WriteDouble(state, 1, 0x1122334455667788ULL);
    EXPECT_EQ(state.float_registers.at(2), 0x55667788U) << "the low word is the lower single";
    EXPECT_EQ(state.float_registers.at(3), 0x11223344U);
    EXPECT_EQ(ReadDouble(state, 1), 0x1122334455667788ULL);
}

}  // namespace
}  // namespace hexlith::domain::arm::primitives
