#include "domain/arm/instruction.h"
#include "domain/arm/primitives/alu.h"
#include "domain/arm/primitives/branch.h"
#include "domain/arm/primitives/media.h"
#include "domain/arm/primitives/multiply.h"
#include "domain/arm/primitives/transfer.h"
#include "domain/arm/primitives/unpredictable.h"
#include "domain/memory/paged_memory.h"

#include <cstdint>
#include <gtest/gtest.h>

namespace hexlith::domain::arm::primitives {
namespace {

ShiftResult Operand(std::uint32_t value, bool carry = false) {
    return {.value = value, .carry = carry};
}

TEST(ComputeAlu, LogicalOperationsTakeTheShifterCarryAndLeaveOverflowAlone) {
    const AluResult result = ComputeAlu(Operation::And, 0xF0F0, Operand(0x00FF, true), false);
    EXPECT_EQ(result.value, 0x00F0U);
    EXPECT_TRUE(result.carry);
    EXPECT_FALSE(result.arithmetic);
}

TEST(ComputeAlu, SubtractionCarriesWhenThereIsNoBorrow) {
    const AluResult result = ComputeAlu(Operation::Sub, 5, Operand(3), false);
    EXPECT_EQ(result.value, 2U);
    EXPECT_TRUE(result.carry);
    EXPECT_TRUE(result.arithmetic);
}

TEST(ComputeAlu, ReverseSubtractSwapsTheOperands) {
    EXPECT_EQ(ComputeAlu(Operation::Rsb, 3, Operand(5), false).value, 2U);
    EXPECT_EQ(ComputeAlu(Operation::Rsc, 3, Operand(5), false).value, 1U)
        << "no carry in means one less";
}

TEST(ComputeAlu, CompareAndTestComputeTheSameValueAsTheirWritingTwins) {
    EXPECT_EQ(ComputeAlu(Operation::Cmp, 9, Operand(4), false).value,
              ComputeAlu(Operation::Sub, 9, Operand(4), false).value);
    EXPECT_EQ(ComputeAlu(Operation::Tst, 6, Operand(3), false).value,
              ComputeAlu(Operation::And, 6, Operand(3), false).value);
}

TEST(ReadProgramCounter, ReadsTheInstructionSetsPipelineOffset) {
    EXPECT_EQ(ReadProgramCounter(0x1000, false), 0x1008U);
    EXPECT_EQ(ReadProgramCounter(0x1002, true), 0x1006U);
}

TEST(ReadProgramCounter, RoundsDownToAWordOnlyInThumb) {
    EXPECT_EQ(ReadAlignedProgramCounter(0x1002, true), 0x1004U);
    EXPECT_EQ(ReadAlignedProgramCounter(0x1000, false), 0x1008U);
}

TEST(BranchWriteTarget, IgnoresTheBitsBelowTheInstructionSize) {
    EXPECT_EQ(BranchWriteTarget(0x2003, false), 0x2000U);
    EXPECT_EQ(BranchWriteTarget(0x2003, true), 0x2002U);
}

TEST(Exchange, ChoosesTheInstructionSetFromTheLowBit) {
    EXPECT_TRUE(ExchangeToThumb(0x2001));
    EXPECT_FALSE(ExchangeToThumb(0x2000));
}

TEST(Exchange, RefusesAnArmTargetThatIsOnlyHalfwordAligned) {
    EXPECT_TRUE(ExchangeTargetUnpredictable(0x2002));
    EXPECT_FALSE(ExchangeTargetUnpredictable(0x2003)) << "odd means Thumb, which is fine";
    EXPECT_FALSE(ExchangeTargetUnpredictable(0x2004));
}

TEST(ComputedBranch, IsUnpredictableToAMisalignedArmTargetOnly) {
    EXPECT_TRUE(ComputedBranchUnpredictable(0x2001, false));
    EXPECT_FALSE(ComputedBranchUnpredictable(0x2001, true));
}

TEST(SingleAddress, PreIndexedTransfersAtTheOffsetAddress) {
    const Addressing up = SingleAddress(0x100, 0x10, true, true);
    EXPECT_EQ(up.address, 0x110U);
    EXPECT_EQ(up.written_back, 0x110U);
    EXPECT_EQ(SingleAddress(0x100, 0x10, false, true).address, 0xF0U);
}

TEST(SingleAddress, PostIndexedTransfersAtTheBaseAndWritesTheOffsetBack) {
    const Addressing down = SingleAddress(0x100, 0x10, false, false);
    EXPECT_EQ(down.address, 0x100U);
    EXPECT_EQ(down.written_back, 0xF0U);
}

TEST(BlockStart, CoversTheFourAddressingModes) {
    constexpr std::uint32_t Base = 0x1000;
    constexpr std::uint32_t Bytes = 12;
    EXPECT_EQ(BlockStart(true, false, Base, Bytes), 0x1000U) << "increment after";
    EXPECT_EQ(BlockStart(true, true, Base, Bytes), 0x1004U) << "increment before";
    EXPECT_EQ(BlockStart(false, false, Base, Bytes), 0x0FF8U) << "decrement after";
    EXPECT_EQ(BlockStart(false, true, Base, Bytes), 0x0FF4U) << "decrement before";
    EXPECT_EQ(BlockWrittenBack(true, Base, Bytes), 0x100CU);
    EXPECT_EQ(BlockWrittenBack(false, Base, Bytes), 0x0FF4U);
}

TEST(TransferWidth, FollowsTheOperation) {
    EXPECT_EQ(WidthOf(Operation::LoadByte), Width::Byte);
    EXPECT_EQ(WidthOf(Operation::StoreByteUnprivileged), Width::Byte);
    EXPECT_EQ(WidthOf(Operation::LoadSignedHalf), Width::SignedHalf);
    EXPECT_EQ(WidthOf(Operation::StoreWord), Width::Word);
    EXPECT_EQ(WidthOfSize(2), Width::Half);
    EXPECT_EQ(ExclusiveSize(Operation::StoreExclusiveDouble), 8U);
    EXPECT_EQ(ExclusiveSize(Operation::LoadExclusiveByte), 1U);
}

TEST(TransferValue, SignExtendsOnTheWayInAndTruncatesOnTheWayOut) {
    memory::PagedMemory memory;
    ASSERT_TRUE(memory.Map(0x1000, 0x1000, true).Ok());
    ASSERT_TRUE(StoreValue(memory, 0x1000, 0x12345680, Width::Word));
    EXPECT_EQ(LoadValue(memory, 0x1000, Width::Byte), 0x80U);
    EXPECT_EQ(LoadValue(memory, 0x1000, Width::SignedByte), 0xFFFFFF80U);
    EXPECT_EQ(LoadValue(memory, 0x1000, Width::Half), 0x5680U);
    ASSERT_TRUE(StoreValue(memory, 0x1004, 0xAABBCCDD, Width::Half));
    EXPECT_EQ(LoadValue(memory, 0x1004, Width::Word), 0xCCDDU);
}

TEST(TransferValue, AnUnmappedAddressAnswersNothing) {
    memory::PagedMemory memory;
    EXPECT_FALSE(LoadValue(memory, 0x9000, Width::Word).has_value());
    EXPECT_FALSE(StoreValue(memory, 0x9000, 1, Width::Word));
}

TEST(LongMultiply, SignedAndUnsignedDifferInTheHighWord) {
    constexpr std::uint32_t Minus1 = 0xFFFFFFFF;
    EXPECT_EQ(LongMultiply(Operation::Umull, Minus1, 2, 0, 0, 0), 0x1FFFFFFFEULL);
    EXPECT_EQ(LongMultiply(Operation::Smull, Minus1, 2, 0, 0, 0), 0xFFFFFFFFFFFFFFFEULL);
}

TEST(LongMultiply, AccumulatesAndAddsBothHalvesForUmaal) {
    EXPECT_EQ(LongMultiply(Operation::Umlal, 3, 4, 100, 0, 0), 112U);
    EXPECT_EQ(LongMultiply(Operation::Umaal, 3, 4, 0, 5, 6), 23U);
}

TEST(Extend, AddsTheExtendedByteToTheAddend) {
    EXPECT_EQ(Extend(Operation::Sxtab, 0x80, 1), 0xFFFFFF81U);
    EXPECT_EQ(Extend(Operation::Uxtab, 0x80, 1), 0x81U);
    EXPECT_EQ(Extend(Operation::Uxth, 0x12345678, 0), 0x5678U);
}

TEST(ArithmeticShiftAmount, ReadsZeroAsThirtyTwo) {
    EXPECT_EQ(ArithmeticShiftAmount(0), 32U);
    EXPECT_EQ(ArithmeticShiftAmount(5), 5U);
}

TEST(SaturationOperand, ShiftsBeforeSaturatingAndTreatsAsrZeroAsThirtyTwo) {
    EXPECT_EQ(SaturationOperand(ShiftKind::LogicalLeft, 3, 2), 12);
    EXPECT_EQ(SaturationOperand(ShiftKind::ArithmeticRight, 0x80000000, 0), -1);
}

Instruction With(Operation operation) {
    Instruction instruction;
    instruction.operation = operation;
    return instruction;
}

TEST(HasUnpredictableOperands, ADataProcessingWriteToThePcWithSIsRefused) {
    Instruction instruction = With(Operation::Sub);
    instruction.destination = ProgramCounter;
    instruction.updates_flags = true;
    EXPECT_TRUE(HasUnpredictableOperands(instruction));
    instruction.updates_flags = false;
    EXPECT_FALSE(HasUnpredictableOperands(instruction));
}

TEST(HasUnpredictableOperands, AShiftByRegisterThatTouchesThePcIsRefused) {
    Instruction instruction = With(Operation::Mov);
    instruction.destination = 1;
    instruction.second = ProgramCounter;
    instruction.shift_register = 2;
    EXPECT_TRUE(HasUnpredictableOperands(instruction));
}

TEST(HasUnpredictableOperands, ALongMultiplyNeedsTwoDistinctDestinations) {
    Instruction instruction = With(Operation::Umull);
    instruction.destination = 1;
    instruction.destination_high = 1;
    instruction.second = 2;
    instruction.third = 3;
    EXPECT_TRUE(HasUnpredictableOperands(instruction));
    instruction.destination_high = 4;
    EXPECT_FALSE(HasUnpredictableOperands(instruction));
}

TEST(HasUnpredictableOperands, WritingBackIntoTheLoadedRegisterIsRefused) {
    Instruction instruction = With(Operation::LoadWord);
    instruction.operand = OperandForm::Immediate;
    instruction.first = 3;
    instruction.destination = 3;
    instruction.writes_back = true;
    EXPECT_TRUE(HasUnpredictableOperands(instruction));
}

TEST(HasUnpredictableOperands, LoadMultipleWithWritebackIntoAListedBaseDependsOnTheSet) {
    Instruction instruction = With(Operation::LoadMultiple);
    instruction.first = 0;
    instruction.registers = 0b11;
    instruction.writes_back = true;
    EXPECT_TRUE(HasUnpredictableOperands(instruction));
    instruction.instruction_set = InstructionSet::Thumb;
    EXPECT_FALSE(HasUnpredictableOperands(instruction)) << "Thumb defines the loaded value to win";
}

TEST(HasUnpredictableOperands, StoreMultipleWithWritebackNeedsTheBaseToBeTheLowestListed) {
    Instruction instruction = With(Operation::StoreMultiple);
    instruction.first = 1;
    instruction.registers = 0b0011;
    instruction.writes_back = true;
    EXPECT_TRUE(HasUnpredictableOperands(instruction));
    instruction.registers = 0b0110;
    EXPECT_FALSE(HasUnpredictableOperands(instruction));
}

TEST(HasUnpredictableOperands, ExclusiveStoreStatusMustDifferFromItsOperands) {
    Instruction instruction = With(Operation::StoreExclusive);
    instruction.destination = 1;
    instruction.second = 1;
    instruction.first = 2;
    EXPECT_TRUE(HasUnpredictableOperands(instruction));
    instruction.second = 3;
    EXPECT_FALSE(HasUnpredictableOperands(instruction));
}

TEST(HasUnpredictableOperands, ClassifiesNothingForAnOperationItDoesNotKnow) {
    EXPECT_FALSE(HasUnpredictableOperands(With(Operation::Undefined)));
    EXPECT_FALSE(HasUnpredictableOperands(With(Operation::Branch)));
}

}  // namespace
}  // namespace hexlith::domain::arm::primitives
