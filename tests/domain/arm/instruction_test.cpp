#include "domain/arm/instruction.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <set>
#include <string>

namespace hexlith::domain::arm {
namespace {

constexpr std::uint32_t SomeAddress = 0x00100000;

// A displacement and a literal pool offset, both small enough to read at a
// glance and neither a power of two that could hide a shift mistake.
constexpr std::uint32_t PoolOffset = 0x30;

Instruction ArmInstruction(Operation operation) {
    return Instruction{.operation = operation, .instruction_set = InstructionSet::Arm, .width = 4};
}

TEST(RegisterName, NamesTheThreeWithAJobOfTheirOwn) {
    EXPECT_EQ(RegisterName(0), "r0");
    EXPECT_EQ(RegisterName(12), "r12");
    EXPECT_EQ(RegisterName(StackPointer), "sp");
    EXPECT_EQ(RegisterName(LinkRegister), "lr");
    EXPECT_EQ(RegisterName(ProgramCounter), "pc");
    EXPECT_EQ(RegisterName(NoRegister), "r?");
}

TEST(RegisterSet, HoldsTheRegistersItsBitsName) {
    const RegisterSet pushed = (1U << 4U) | (1U << LinkRegister);
    EXPECT_TRUE(Holds(pushed, 4));
    EXPECT_TRUE(Holds(pushed, LinkRegister));
    EXPECT_FALSE(Holds(pushed, ProgramCounter));
    EXPECT_FALSE(Holds(pushed, 0));
}

TEST(ConditionSuffix, SpellsAlwaysAsNothing) {
    EXPECT_EQ(ConditionSuffix(Condition::Equal), "eq");
    EXPECT_EQ(ConditionSuffix(Condition::LessOrEqual), "le");
    EXPECT_EQ(ConditionSuffix(Condition::Always), "");
    EXPECT_FALSE(IsConditional(Condition::Always));
    EXPECT_FALSE(IsConditional(Condition::Unconditional));
    EXPECT_TRUE(IsConditional(Condition::CarrySet));
}

// The bound is the last enumerator rather than a sentinel inside the
// enumeration, which would defeat the compiler's check that every switch over an
// operation handles every one of them.
constexpr std::uint8_t LastOperation = static_cast<std::uint8_t>(Operation::ReturnFromException);

std::set<std::string> EveryMnemonic() {
    std::set<std::string> spellings;
    for (std::uint8_t value = 1; value <= LastOperation; ++value) {
        spellings.insert(std::string{Mnemonic(static_cast<Operation>(value))});
    }
    return spellings;
}

TEST(Mnemonic, EveryOperationHasOneOfItsOwn) {
    EXPECT_EQ(Mnemonic(Operation::Undefined), "undefined");

    for (std::uint8_t value = 1; value <= LastOperation; ++value) {
        const std::string mnemonic{Mnemonic(static_cast<Operation>(value))};
        EXPECT_NE(mnemonic, "undefined") << "operation " << static_cast<int>(value);
        EXPECT_FALSE(mnemonic.empty()) << "operation " << static_cast<int>(value);
    }
}

TEST(Mnemonic, RepeatsItselfOnlyWhereTheAssemblerDoes) {
    // The two forms of blx share a spelling, and a halfword multiply's mnemonic
    // is completed by which halves it reads, so the spellings are not distinct.
    // Most of them are, and a table that had collapsed would show up here.
    EXPECT_GT(EveryMnemonic().size(), LastOperation - 10U);
}

TEST(ShiftName, NamesEveryShift) {
    EXPECT_EQ(ShiftName(ShiftKind::LogicalLeft), "lsl");
    EXPECT_EQ(ShiftName(ShiftKind::LogicalRight), "lsr");
    EXPECT_EQ(ShiftName(ShiftKind::ArithmeticRight), "asr");
    EXPECT_EQ(ShiftName(ShiftKind::RotateRight), "ror");
    EXPECT_EQ(ShiftName(ShiftKind::RotateRightExtended), "rrx");
}

TEST(WritesDestination, ComparisonsAndStoresDoNot) {
    EXPECT_FALSE(WritesDestination(Operation::Cmp));
    EXPECT_FALSE(WritesDestination(Operation::Teq));
    EXPECT_FALSE(WritesDestination(Operation::StoreWord));
    EXPECT_FALSE(WritesDestination(Operation::StoreMultiple));
    EXPECT_FALSE(WritesDestination(Operation::Undefined));
    EXPECT_TRUE(WritesDestination(Operation::Add));
    EXPECT_TRUE(WritesDestination(Operation::LoadWord));
    EXPECT_TRUE(WritesDestination(Operation::Mov));
    EXPECT_TRUE(WritesDestination(Operation::CountLeadingZeros));
}

TEST(WritesProgramCounter, FindsEveryRouteToIt) {
    EXPECT_TRUE(WritesProgramCounter(ArmInstruction(Operation::Branch)));
    EXPECT_TRUE(WritesProgramCounter(ArmInstruction(Operation::BranchLink)));
    EXPECT_TRUE(WritesProgramCounter(ArmInstruction(Operation::BranchExchange)));

    Instruction move_to_pc = ArmInstruction(Operation::Mov);
    move_to_pc.destination = ProgramCounter;
    EXPECT_TRUE(WritesProgramCounter(move_to_pc));

    Instruction load_to_pc = ArmInstruction(Operation::LoadWord);
    load_to_pc.destination = ProgramCounter;
    EXPECT_TRUE(WritesProgramCounter(load_to_pc));

    Instruction pop = ArmInstruction(Operation::LoadMultiple);
    pop.first = StackPointer;
    pop.registers = (1U << 4U) | (1U << ProgramCounter);
    EXPECT_TRUE(WritesProgramCounter(pop));

    Instruction add_to_pc = ArmInstruction(Operation::Add);
    add_to_pc.destination = ProgramCounter;
    EXPECT_TRUE(WritesProgramCounter(add_to_pc));
}

TEST(WritesProgramCounter, IsNotFooledByAReadOrAStore) {
    // cmp pc, r0 names r15 in the field a data-processing instruction writes
    // through, and writes nothing.
    Instruction compare = ArmInstruction(Operation::Cmp);
    compare.destination = ProgramCounter;
    compare.first = ProgramCounter;
    EXPECT_FALSE(WritesProgramCounter(compare));

    // str pc, [r0] stores the program counter rather than loading it.
    Instruction store = ArmInstruction(Operation::StoreWord);
    store.destination = ProgramCounter;
    store.first = 0;
    EXPECT_FALSE(WritesProgramCounter(store));

    // A push naming the program counter stores it.
    Instruction push = ArmInstruction(Operation::StoreMultiple);
    push.first = StackPointer;
    push.registers = 1U << ProgramCounter;
    EXPECT_FALSE(WritesProgramCounter(push));
}

TEST(WritesProgramCounter, CountsWriteBackIntoTheProgramCounter) {
    Instruction load = ArmInstruction(Operation::LoadWord);
    load.destination = 0;
    load.first = ProgramCounter;
    load.writes_back = true;
    EXPECT_TRUE(WritesProgramCounter(load));

    load.writes_back = false;
    EXPECT_FALSE(WritesProgramCounter(load));
}

TEST(BranchTarget, CountsFromTwoInstructionsAheadInArm) {
    Instruction branch = ArmInstruction(Operation::Branch);
    branch.branch_offset = 0;
    EXPECT_EQ(BranchTarget(branch, SomeAddress), SomeAddress + 8);

    branch.branch_offset = 0x40;
    EXPECT_EQ(BranchTarget(branch, SomeAddress), SomeAddress + 8 + 0x40);

    branch.branch_offset = -0x10;
    EXPECT_EQ(BranchTarget(branch, SomeAddress), SomeAddress + 8 - 0x10);
}

TEST(BranchTarget, CountsFromOneInstructionAheadInThumb) {
    Instruction branch = ArmInstruction(Operation::Branch);
    branch.instruction_set = InstructionSet::Thumb;
    branch.width = 2;
    branch.branch_offset = 0x20;
    EXPECT_EQ(BranchTarget(branch, SomeAddress), SomeAddress + 4 + 0x20);
}

TEST(HasKnownTarget, IsTrueOnlyForADisplacement) {
    EXPECT_TRUE(HasKnownTarget(ArmInstruction(Operation::Branch)));
    EXPECT_TRUE(HasKnownTarget(ArmInstruction(Operation::BranchLink)));
    EXPECT_TRUE(HasKnownTarget(ArmInstruction(Operation::BranchLinkExchangeImmediate)));
    EXPECT_FALSE(HasKnownTarget(ArmInstruction(Operation::BranchExchange)));
    EXPECT_FALSE(HasKnownTarget(ArmInstruction(Operation::LoadMultiple)));
}

TEST(IsCall, IsTheThreeLinkingForms) {
    EXPECT_TRUE(IsCall(ArmInstruction(Operation::BranchLink)));
    EXPECT_TRUE(IsCall(ArmInstruction(Operation::BranchLinkExchangeRegister)));
    EXPECT_TRUE(IsCall(ArmInstruction(Operation::BranchLinkExchangeImmediate)));
    EXPECT_FALSE(IsCall(ArmInstruction(Operation::Branch)));
    EXPECT_FALSE(IsCall(ArmInstruction(Operation::BranchExchange)));
}

TEST(IsReturn, RecognisesTheThreeWaysAFunctionEnds) {
    Instruction exchange = ArmInstruction(Operation::BranchExchange);
    exchange.second = LinkRegister;
    EXPECT_TRUE(IsReturn(exchange));

    exchange.second = 3;
    EXPECT_FALSE(IsReturn(exchange));

    Instruction move = ArmInstruction(Operation::Mov);
    move.destination = ProgramCounter;
    move.operand = OperandForm::Register;
    move.second = LinkRegister;
    EXPECT_TRUE(IsReturn(move));

    Instruction pop = ArmInstruction(Operation::LoadMultiple);
    pop.first = StackPointer;
    pop.registers = 1U << ProgramCounter;
    EXPECT_TRUE(IsReturn(pop));

    // The same load from somewhere other than the stack is a dispatch through a
    // table, not a return.
    pop.first = 4;
    EXPECT_FALSE(IsReturn(pop));
}

TEST(ContinuesToTheNextInstruction, StopsOnlyAtAnUnconditionalTransfer) {
    Instruction branch = ArmInstruction(Operation::Branch);
    EXPECT_FALSE(ContinuesToTheNextInstruction(branch));

    branch.condition = Condition::Equal;
    EXPECT_TRUE(ContinuesToTheNextInstruction(branch));

    EXPECT_TRUE(ContinuesToTheNextInstruction(ArmInstruction(Operation::Add)));
    EXPECT_TRUE(ContinuesToTheNextInstruction(ArmInstruction(Operation::BranchLink)))
        << "a call comes back";
}

TEST(ProgramCounterRelativeAddress, FindsTheLiteralAPoolLoadReads) {
    Instruction load = ArmInstruction(Operation::LoadWord);
    load.destination = 0;
    load.first = ProgramCounter;
    load.operand = OperandForm::Immediate;
    load.preindexed = true;
    load.add_offset = true;
    load.immediate = PoolOffset;

    EXPECT_EQ(ProgramCounterRelativeAddress(load, SomeAddress), SomeAddress + 8 + PoolOffset);

    load.add_offset = false;
    EXPECT_EQ(ProgramCounterRelativeAddress(load, SomeAddress), SomeAddress + 8 - PoolOffset);
}

TEST(ProgramCounterRelativeAddress, IsNothingWhenTheBaseIsNotTheProgramCounter) {
    Instruction load = ArmInstruction(Operation::LoadWord);
    load.first = 4;
    load.operand = OperandForm::Immediate;
    load.preindexed = true;
    EXPECT_FALSE(ProgramCounterRelativeAddress(load, SomeAddress).has_value());

    load.first = ProgramCounter;
    load.operand = OperandForm::ShiftedRegister;
    EXPECT_FALSE(ProgramCounterRelativeAddress(load, SomeAddress).has_value())
        << "an indexed load reads an address the instruction does not hold";

    EXPECT_FALSE(
        ProgramCounterRelativeAddress(ArmInstruction(Operation::Add), SomeAddress).has_value());
}

TEST(IsDefined, SeparatesAnInstructionFromWhateverElseAWordMightBe) {
    EXPECT_FALSE(IsDefined(ArmInstruction(Operation::Undefined)));
    EXPECT_TRUE(IsDefined(ArmInstruction(Operation::Add)));
}

}  // namespace
}  // namespace hexlith::domain::arm
