#include "domain/arm/decode_arm.h"
#include "domain/arm/instruction.h"

#include <cstdint>
#include <gtest/gtest.h>

namespace hexlith::domain::arm {
namespace {

// Floating point is reached through DecodeArm, which is how everything else
// reaches it, so these go through the same door. Every encoding was checked
// against Capstone before it was written down; scripts/compare_arm_decoder.py
// --float covers the space these only sample.
constexpr std::uint32_t Here = 0x00100000;

TEST(DecodeVfp, AddsTwoSingles) {
    // vadd.f32 s0, s1, s2
    const Instruction decoded = DecodeArm(0xEE300A81);
    EXPECT_EQ(decoded.operation, Operation::VfpAdd);
    EXPECT_EQ(decoded.condition, Condition::Always);
    EXPECT_EQ(decoded.vfp_destination, 0);
    EXPECT_EQ(decoded.vfp_first, 1);
    EXPECT_EQ(decoded.vfp_second, 2);
    EXPECT_FALSE(decoded.vfp_destination_double);
    EXPECT_FALSE(decoded.vfp_sources_double);
    EXPECT_TRUE(IsFloatingPoint(decoded.operation));
}

TEST(DecodeVfp, NumbersDoubleRegistersFromTheOtherEnd) {
    // vmla.f64 d0, d1, d2. The extra register bit is the high bit of a double
    // and the low bit of a single, so the same fields name different registers.
    const Instruction decoded = DecodeArm(0xEE010B02);
    EXPECT_EQ(decoded.operation, Operation::VfpMultiplyAccumulate);
    EXPECT_TRUE(decoded.vfp_destination_double);
    EXPECT_EQ(decoded.vfp_destination, 0);
    EXPECT_EQ(decoded.vfp_first, 1);
    EXPECT_EQ(decoded.vfp_second, 2);
}

TEST(DecodeVfp, ReadsTheArithmeticTable) {
    // vnmla.f32 s0, s1, s2, the negated row with bit 6 set
    EXPECT_EQ(DecodeArm(0xEE100AC1).operation, Operation::VfpNegatedMultiplyAccumulate);

    // vdiv.f64 d3, d4, d5
    const Instruction divide = DecodeArm(0xEE843B05);
    EXPECT_EQ(divide.operation, Operation::VfpDivide);
    EXPECT_EQ(divide.vfp_destination, 3);
    EXPECT_EQ(divide.vfp_first, 4);
    EXPECT_EQ(divide.vfp_second, 5);
}

TEST(DecodeVfp, ReadsTheOneSourceOperations) {
    // vabs.f32 s4, s5
    const Instruction absolute = DecodeArm(0xEEB02AE2);
    EXPECT_EQ(absolute.operation, Operation::VfpAbsolute);
    EXPECT_EQ(absolute.vfp_destination, 4);
    EXPECT_EQ(absolute.vfp_first, NoRegister) << "the first field is an opcode here";
    EXPECT_EQ(absolute.vfp_second, 5);

    // vsqrt.f64 d1, d2
    EXPECT_EQ(DecodeArm(0xEEB11BC2).operation, Operation::VfpSquareRoot);
}

TEST(DecodeVfp, ReadsTheComparisons) {
    // vcmp.f64 d0, d1
    const Instruction registers = DecodeArm(0xEEB40B41);
    EXPECT_EQ(registers.operation, Operation::VfpCompare);
    EXPECT_EQ(registers.vfp_first, 0);
    EXPECT_EQ(registers.vfp_second, 1);
    EXPECT_EQ(registers.vfp_destination, NoRegister) << "a comparison writes only flags";

    // vcmpe.f32 s0, #0
    const Instruction zero = DecodeArm(0xEEB50AC0);
    EXPECT_EQ(zero.operation, Operation::VfpCompareSignalling);
    EXPECT_EQ(zero.vfp_first, 0);
    EXPECT_EQ(zero.vfp_second, NoRegister) << "no second register means zero";
}

TEST(DecodeVfp, ConvertsBetweenPrecisions) {
    // vcvt.f64.f32 d0, s1
    const Instruction widening = DecodeArm(0xEEB70AE0);
    EXPECT_EQ(widening.operation, Operation::VfpConvertPrecision);
    EXPECT_TRUE(widening.vfp_destination_double);
    EXPECT_FALSE(widening.vfp_sources_double);
    EXPECT_EQ(widening.vfp_destination, 0);
    EXPECT_EQ(widening.vfp_second, 1);

    // vcvt.f32.f64 s0, d1
    const Instruction narrowing = DecodeArm(0xEEB70BC1);
    EXPECT_FALSE(narrowing.vfp_destination_double);
    EXPECT_TRUE(narrowing.vfp_sources_double);
    EXPECT_EQ(narrowing.vfp_second, 1);
}

TEST(DecodeVfp, ConvertsToAndFromIntegers) {
    // vcvt.f32.s32 s0, s1
    const Instruction from = DecodeArm(0xEEB80AE0);
    EXPECT_EQ(from.operation, Operation::VfpConvertFromInteger);
    EXPECT_TRUE(from.integer_signed);

    // vcvt.s32.f64 s0, d1, which truncates as a C cast does
    const Instruction truncating = DecodeArm(0xEEBD0BC1);
    EXPECT_EQ(truncating.operation, Operation::VfpConvertToInteger);
    EXPECT_TRUE(truncating.integer_signed);
    EXPECT_TRUE(truncating.rounds_towards_zero);
    EXPECT_TRUE(truncating.vfp_sources_double);
    EXPECT_FALSE(truncating.vfp_destination_double);

    // vcvtr.u32.f32 s0, s1, which rounds in the FPSCR mode
    const Instruction rounding = DecodeArm(0xEEBC0A60);
    EXPECT_FALSE(rounding.integer_signed);
    EXPECT_FALSE(rounding.rounds_towards_zero);
}

TEST(DecodeVfp, MovesBetweenGeneralAndFloatingRegisters) {
    // vmov s1, r2
    const Instruction to = DecodeArm(0xEE002A90);
    EXPECT_EQ(to.operation, Operation::VfpMoveToSingle);
    EXPECT_EQ(to.destination, 2);
    EXPECT_EQ(to.vfp_destination, 1);
    EXPECT_FALSE(WritesProgramCounter(to));

    // vmov r3, s2
    const Instruction from = DecodeArm(0xEE113A10);
    EXPECT_EQ(from.operation, Operation::VfpMoveFromSingle);
    EXPECT_EQ(from.destination, 3);
    EXPECT_EQ(from.vfp_destination, 2);

    // vmov.32 d0[1], r1
    const Instruction half = DecodeArm(0xEE201B10);
    EXPECT_EQ(half.operation, Operation::VfpMoveToHalf);
    EXPECT_EQ(half.immediate, 1U) << "the high half";

    // vmov r0, r1, d2
    const Instruction pair = DecodeArm(0xEC510B12);
    EXPECT_EQ(pair.operation, Operation::VfpMoveFromPair);
    EXPECT_EQ(pair.destination, 0);
    EXPECT_EQ(pair.destination_high, 1);
    EXPECT_EQ(pair.vfp_destination, 2);
    EXPECT_TRUE(pair.vfp_destination_double);

    // vmov s0, s1, r0, r1
    const Instruction singles = DecodeArm(0xEC410A10);
    EXPECT_EQ(singles.operation, Operation::VfpMoveToPair);
    EXPECT_EQ(singles.vfp_destination, 0);
    EXPECT_FALSE(singles.vfp_destination_double);
}

TEST(DecodeVfp, ReadsTheSystemRegisterMoves) {
    // vmsr fpscr, r0
    const Instruction write = DecodeArm(0xEEE10A10);
    EXPECT_EQ(write.operation, Operation::VfpMoveToSystem);
    EXPECT_EQ(write.destination, 0);
    EXPECT_EQ(write.immediate, 1U);

    // vmrs apsr_nzcv, fpscr, which is how a floating point comparison reaches a
    // conditional branch. It names r15 and writes the flags, not the register.
    const Instruction flags = DecodeArm(0xEEF1FA10);
    EXPECT_EQ(flags.operation, Operation::VfpMoveFromSystem);
    EXPECT_TRUE(flags.updates_flags);
    EXPECT_EQ(flags.destination, NoRegister);
    EXPECT_FALSE(WritesProgramCounter(flags));
}

TEST(DecodeVfp, LoadsAConstantFromAPool) {
    // vldr d0, [pc, #8]
    const Instruction decoded = DecodeArm(0xED9F0B02);
    EXPECT_EQ(decoded.operation, Operation::VfpLoad);
    EXPECT_EQ(decoded.first, ProgramCounter);
    EXPECT_EQ(decoded.immediate, 8U);
    EXPECT_TRUE(decoded.vfp_destination_double);
    EXPECT_EQ(ProgramCounterRelativeAddress(decoded, Here), Here + 8 + 8);

    // vstr s1, [r0, #-4]
    const Instruction store = DecodeArm(0xED400A01);
    EXPECT_EQ(store.operation, Operation::VfpStore);
    EXPECT_FALSE(store.add_offset);
    EXPECT_EQ(store.vfp_destination, 1);
}

TEST(DecodeVfp, ReadsTheBlockTransfers) {
    // vpush {d8-d15}
    const Instruction push = DecodeArm(0xED2D8B10);
    EXPECT_EQ(push.operation, Operation::VfpStoreMultiple);
    EXPECT_EQ(push.first, StackPointer);
    EXPECT_EQ(push.vfp_destination, 8);
    EXPECT_EQ(push.vfp_count, 8);
    EXPECT_TRUE(push.preindexed);
    EXPECT_FALSE(push.add_offset);
    EXPECT_TRUE(push.writes_back);

    // vldmia r0!, {s0-s3}
    const Instruction load = DecodeArm(0xECB00A04);
    EXPECT_EQ(load.operation, Operation::VfpLoadMultiple);
    EXPECT_EQ(load.vfp_count, 4);
    EXPECT_EQ(load.immediate, 16U);

    // fldmiax r0!, {d0-d1}: an odd word count in double precision moves the
    // doubles and one word more.
    const Instruction unknown_format = DecodeArm(0xECB00B05);
    EXPECT_EQ(unknown_format.vfp_count, 2);
    EXPECT_EQ(unknown_format.immediate, 20U);
}

TEST(DecodeVfp, RefusesWhatCameAfterVfpTwo) {
    // vmov.f32 s0, #1.0, an immediate move, which VFPv3 added
    EXPECT_EQ(DecodeArm(0xEEB70A00).operation, Operation::Undefined);

    // vadd.f64 d16, d0, d0, a register VFPv2 does not have
    EXPECT_EQ(DecodeArm(0xEE700B00).operation, Operation::Undefined);

    // vfma.f32 s0, s0, s2, a fused multiply, which VFPv4 added
    EXPECT_EQ(DecodeArm(0xEEA00A01).operation, Operation::Undefined);

    // vselgt.f32 s0, s1, s2, in the unconditional space ARMv8 used
    EXPECT_EQ(DecodeArm(0xFE300A81).operation, Operation::Undefined);
}

TEST(DecodeVfp, RefusesUnpredictableTransfers) {
    // vldmia r5, {s8-...} with 181 words, which runs past s31
    EXPECT_EQ(DecodeArm(0xAC954AB5).operation, Operation::Undefined);

    // vmov lr, lr, s29, s30, which writes one register twice
    EXPECT_EQ(DecodeArm(0x1C5EEA3E).operation, Operation::Undefined);
}

}  // namespace
}  // namespace hexlith::domain::arm
