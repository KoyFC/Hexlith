#include "domain/arm/decode_thumb.h"

#include <cstdint>
#include <gtest/gtest.h>

namespace hexlith::domain::arm {
namespace {

// Every encoding here was checked against an independent disassembler before it
// was written down. The encodings come from the reference image where they could,
// which is why some of them look like the middle of a real function.
constexpr std::uint32_t Here = 0x00100000;

// A Thumb instruction can sit at any even address, which is what makes the
// alignment of a literal pool read worth a test of its own.
constexpr std::uint32_t HereOdd = 0x00100002;

constexpr std::uint16_t NoSecondHalfword = 0;

Instruction Decode(std::uint16_t first) {
    return DecodeThumb(first, NoSecondHalfword);
}

TEST(DecodeThumb, EveryInstructionIsTwoBytesExceptABranchWithLink) {
    EXPECT_EQ(Decode(0xB510).width, 2);
    EXPECT_EQ(Decode(0x2100).width, 2);
    EXPECT_EQ(DecodeThumb(0xF000, 0xF8CB).width, 4);
}

TEST(DecodeThumb, ReadsAShiftByAnImmediate) {
    // lsls r1, r3, #1
    const Instruction decoded = Decode(0x0059);
    EXPECT_EQ(decoded.operation, Operation::Mov);
    EXPECT_EQ(decoded.destination, 1);
    EXPECT_EQ(decoded.second, 3);
    EXPECT_EQ(decoded.shift, ShiftKind::LogicalLeft);
    EXPECT_EQ(decoded.shift_amount, 1);
    EXPECT_TRUE(decoded.updates_flags) << "Thumb has no way to suppress the flags here";
    EXPECT_EQ(decoded.instruction_set, InstructionSet::Thumb);
}

TEST(DecodeThumb, AShiftOfZeroIsAMoveOrAShiftByThirtyTwo) {
    // movs r1, r3, which is a logical left shift by nothing
    const Instruction move = Decode(0x0019);
    EXPECT_EQ(move.operation, Operation::Mov);
    EXPECT_EQ(move.operand, OperandForm::Register);

    // lsrs r1, r3, #32
    const Instruction shift = Decode(0x0819);
    EXPECT_EQ(shift.shift, ShiftKind::LogicalRight);
    EXPECT_EQ(shift.shift_amount, 32);
}

TEST(DecodeThumb, ReadsTheImmediateOperations) {
    // movs r1, #0
    const Instruction move = Decode(0x2100);
    EXPECT_EQ(move.operation, Operation::Mov);
    EXPECT_EQ(move.destination, 1);
    EXPECT_EQ(move.immediate, 0U);
    EXPECT_TRUE(move.updates_flags);

    // cmp r3, #0
    const Instruction compare = Decode(0x2B00);
    EXPECT_EQ(compare.operation, Operation::Cmp);
    EXPECT_EQ(compare.first, 3);
    EXPECT_EQ(compare.destination, NoRegister);

    // adds r2, #1, which reads and writes the same register
    const Instruction add = Decode(0x3201);
    EXPECT_EQ(add.operation, Operation::Add);
    EXPECT_EQ(add.destination, 2);
    EXPECT_EQ(add.first, 2);
    EXPECT_EQ(add.immediate, 1U);
}

TEST(DecodeThumb, ReadsTheThreeOperandAddAndSubtract) {
    // adds r2, r2, #1
    const Instruction immediate = Decode(0x1C52);
    EXPECT_EQ(immediate.operation, Operation::Add);
    EXPECT_EQ(immediate.destination, 2);
    EXPECT_EQ(immediate.first, 2);
    EXPECT_EQ(immediate.immediate, 1U);

    // subs r2, r0, #1
    const Instruction subtract = Decode(0x1E42);
    EXPECT_EQ(subtract.operation, Operation::Sub);
    EXPECT_EQ(subtract.destination, 2);
    EXPECT_EQ(subtract.first, 0);

    // adds r0, r1, r2
    const Instruction registers = Decode(0x1888);
    EXPECT_EQ(registers.operation, Operation::Add);
    EXPECT_EQ(registers.operand, OperandForm::Register);
    EXPECT_EQ(registers.second, 2);
}

TEST(DecodeThumb, ReadsTheRegisterOperations) {
    // ands r0, r1
    const Instruction conjunction = Decode(0x4008);
    EXPECT_EQ(conjunction.operation, Operation::And);
    EXPECT_EQ(conjunction.destination, 0);
    EXPECT_EQ(conjunction.first, 0);
    EXPECT_EQ(conjunction.second, 1);

    // A shift by a register arrives as a move whose operand is shifted.
    // lsls r0, r1
    const Instruction shift = Decode(0x4088);
    EXPECT_EQ(shift.operation, Operation::Mov);
    EXPECT_EQ(shift.shift, ShiftKind::LogicalLeft);
    EXPECT_EQ(shift.shift_register, 1);
    EXPECT_EQ(shift.second, 0);

    // tst r0, r1 compares and has no destination.
    const Instruction test = Decode(0x4208);
    EXPECT_EQ(test.operation, Operation::Tst);
    EXPECT_EQ(test.destination, NoRegister);
    EXPECT_EQ(test.first, 0);

    // A negation is a reverse subtract from zero, which is the only operation
    // Thumb spells with an implied immediate.
    // negs r0, r1
    const Instruction negate = Decode(0x4248);
    EXPECT_EQ(negate.operation, Operation::Rsb);
    EXPECT_EQ(negate.destination, 0);
    EXPECT_EQ(negate.first, 1);
    EXPECT_EQ(negate.operand, OperandForm::Immediate);
    EXPECT_EQ(negate.immediate, 0U);

    // muls r0, r1, which multiplies r0 by r1. The factors sit where an ARM MUL
    // keeps them, and there is no accumulator.
    const Instruction multiply = Decode(0x4348);
    EXPECT_EQ(multiply.operation, Operation::Mul);
    EXPECT_EQ(multiply.destination, 0);
    EXPECT_EQ(multiply.second, 1);
    EXPECT_EQ(multiply.third, 0);
    EXPECT_EQ(multiply.first, NoRegister);
    // mvns r0, r1
    EXPECT_EQ(Decode(0x43C8).operation, Operation::Mvn);
}

TEST(DecodeThumb, ReachesTheHighRegistersWithoutSettingTheFlags) {
    // mov r8, r1, which is how Thumb code moves into a register the three-bit
    // fields cannot name.
    const Instruction move = Decode(0x4688);
    EXPECT_EQ(move.operation, Operation::Mov);
    EXPECT_EQ(move.destination, 8);
    EXPECT_EQ(move.second, 1);
    EXPECT_FALSE(move.updates_flags)
        << "the bit that would say so is the fourth register bit instead";

    // add r1, r8
    const Instruction add = Decode(0x4441);
    EXPECT_EQ(add.operation, Operation::Add);
    EXPECT_EQ(add.destination, 1);
    EXPECT_EQ(add.second, 8);
    EXPECT_FALSE(add.updates_flags);

    // cmp r1, r8, which does set them: it is the only one of the three that can.
    const Instruction compare = Decode(0x4541);
    EXPECT_EQ(compare.operation, Operation::Cmp);
    EXPECT_TRUE(compare.updates_flags);
}

TEST(DecodeThumb, ReadsTheExchangingBranches) {
    // bx lr
    const Instruction ret = Decode(0x4770);
    EXPECT_EQ(ret.operation, Operation::BranchExchange);
    EXPECT_EQ(ret.second, LinkRegister);
    EXPECT_TRUE(IsReturn(ret));
    EXPECT_FALSE(ContinuesToTheNextInstruction(ret));

    // blx r2
    const Instruction call = Decode(0x4790);
    EXPECT_EQ(call.operation, Operation::BranchLinkExchangeRegister);
    EXPECT_EQ(call.second, 2);
    EXPECT_TRUE(IsCall(call));
    EXPECT_TRUE(ContinuesToTheNextInstruction(call));

    // The three low bits are fixed, and a word that uses them is not a branch.
    EXPECT_EQ(Decode(0x472F).operation, Operation::Undefined);

    // A linking exchange cannot branch through the program counter.
    EXPECT_EQ(Decode(0x47F8).operation, Operation::Undefined);
}

TEST(DecodeThumb, FindsALiteralPoolReadAtTheRightWord) {
    // ldr r1, [pc, #8]
    const Instruction decoded = Decode(0x4902);
    EXPECT_EQ(decoded.operation, Operation::LoadWord);
    EXPECT_EQ(decoded.destination, 1);
    EXPECT_EQ(decoded.first, ProgramCounter);
    EXPECT_EQ(decoded.immediate, 8U);
    EXPECT_FALSE(decoded.updates_flags);

    EXPECT_EQ(ProgramCounterRelativeAddress(decoded, Here), Here + 4 + 8);

    // The program counter is rounded down to a word before the offset is added,
    // which matters because a Thumb instruction can sit at any even address. At
    // an address that is not a multiple of four the read lands two bytes lower
    // than the arithmetic alone would say.
    EXPECT_EQ(ProgramCounterRelativeAddress(decoded, HereOdd), Here + 4 + 8);
}

TEST(DecodeThumb, ReadsTheTransfers) {
    // str r1, [r0, #0] and str r1, [r0, #4]
    const Instruction store = Decode(0x6001);
    EXPECT_EQ(store.operation, Operation::StoreWord);
    EXPECT_EQ(store.destination, 1);
    EXPECT_EQ(store.first, 0);
    EXPECT_EQ(store.immediate, 0U);
    EXPECT_EQ(Decode(0x6041).immediate, 4U) << "a word offset is scaled by four";

    // ldrb r3, [r2, #0]
    const Instruction byte = Decode(0x7813);
    EXPECT_EQ(byte.operation, Operation::LoadByte);
    EXPECT_EQ(byte.immediate, 0U);

    // ldrh r3, [r2, #2], scaled by two
    EXPECT_EQ(Decode(0x8853).operation, Operation::LoadHalf);
    EXPECT_EQ(Decode(0x8853).immediate, 2U);

    // ldr r1, [r2, r3], a register offset
    const Instruction indexed = Decode(0x58D1);
    EXPECT_EQ(indexed.operation, Operation::LoadWord);
    EXPECT_EQ(indexed.operand, OperandForm::Register);
    EXPECT_EQ(indexed.second, 3);

    // ldrsb r1, [r2, r3], which only the register-offset form can reach
    EXPECT_EQ(Decode(0x56D1).operation, Operation::LoadSignedByte);

    // ldr r1, [sp, #4]
    const Instruction stack = Decode(0x9901);
    EXPECT_EQ(stack.operation, Operation::LoadWord);
    EXPECT_EQ(stack.first, StackPointer);
    EXPECT_EQ(stack.immediate, 4U);
}

TEST(DecodeThumb, ReadsTheAddressOfSomething) {
    // add r1, pc, #8, which takes the address of a pool entry rather than
    // reading it.
    const Instruction relative = Decode(0xA102);
    EXPECT_EQ(relative.operation, Operation::Add);
    EXPECT_EQ(relative.destination, 1);
    EXPECT_EQ(relative.first, ProgramCounter);
    EXPECT_EQ(relative.immediate, 8U);
    EXPECT_FALSE(relative.updates_flags);

    // add r1, sp, #8
    EXPECT_EQ(Decode(0xA902).first, StackPointer);
}

TEST(DecodeThumb, ReadsAPushAndAPopIncludingTheRegisterTheBitNames) {
    // push {r4, lr}
    const Instruction push = Decode(0xB510);
    EXPECT_EQ(push.operation, Operation::StoreMultiple);
    EXPECT_EQ(push.first, StackPointer);
    EXPECT_TRUE(Holds(push.registers, 4));
    EXPECT_TRUE(Holds(push.registers, LinkRegister))
        << "one bit names the link register going down";
    EXPECT_FALSE(WritesProgramCounter(push));

    // pop {r4, pc}
    const Instruction pop = Decode(0xBD10);
    EXPECT_EQ(pop.operation, Operation::LoadMultiple);
    EXPECT_TRUE(Holds(pop.registers, ProgramCounter))
        << "and the same bit names the program counter coming back";
    EXPECT_TRUE(IsReturn(pop));
    EXPECT_FALSE(ContinuesToTheNextInstruction(pop));

    // push {r4} without the extra register
    const Instruction plain = Decode(0xB410);
    EXPECT_FALSE(Holds(plain.registers, LinkRegister));

    // An empty list is not an instruction.
    EXPECT_EQ(Decode(0xB400).operation, Operation::Undefined);
}

TEST(DecodeThumb, ReadsTheBlockTransfers) {
    // stmia r0!, {r1, r2}
    const Instruction store = Decode(0xC006);
    EXPECT_EQ(store.operation, Operation::StoreMultiple);
    EXPECT_EQ(store.first, 0);
    EXPECT_TRUE(store.writes_back) << "the Thumb form always writes back";
    EXPECT_TRUE(Holds(store.registers, 1));
    EXPECT_TRUE(Holds(store.registers, 2));

    // ldmia r0!, {r1, r2}
    EXPECT_EQ(Decode(0xC806).operation, Operation::LoadMultiple);

    EXPECT_EQ(Decode(0xC000).operation, Operation::Undefined);
}

TEST(DecodeThumb, AdjustsTheStackWithoutSettingTheFlags) {
    // add sp, #16
    const Instruction grow = Decode(0xB004);
    EXPECT_EQ(grow.operation, Operation::Add);
    EXPECT_EQ(grow.destination, StackPointer);
    EXPECT_EQ(grow.immediate, 16U);
    EXPECT_FALSE(grow.updates_flags);

    // sub sp, #16
    EXPECT_EQ(Decode(0xB084).operation, Operation::Sub);
}

TEST(DecodeThumb, ReadsTheInstructionsArmv6Added) {
    // sxth r1, r2, and the other three extensions beside it
    EXPECT_EQ(Decode(0xB211).operation, Operation::Sxth);
    EXPECT_EQ(Decode(0xB251).operation, Operation::Sxtb);
    EXPECT_EQ(Decode(0xB291).operation, Operation::Uxth);
    EXPECT_EQ(Decode(0xB2D1).operation, Operation::Uxtb);

    // rev r1, r2, rev16 and revsh
    EXPECT_EQ(Decode(0xBA11).operation, Operation::Rev);
    EXPECT_EQ(Decode(0xBA51).operation, Operation::Rev16);
    EXPECT_EQ(Decode(0xBAD1).operation, Operation::Revsh);
    EXPECT_EQ(Decode(0xBA91).operation, Operation::Undefined)
        << "the fourth entry of that row is not an instruction in ARMv6K";

    // nop and yield in their 16-bit Thumb encodings, which ARMv6T2 added. The
    // ARM11 has the hints in ARM state only.
    EXPECT_EQ(Decode(0xBF00).operation, Operation::Undefined);
    EXPECT_EQ(Decode(0xBF10).operation, Operation::Undefined);

    // An if-then block, which belongs to ARMv6T2 and not to this core.
    EXPECT_EQ(Decode(0xBF08).operation, Operation::Undefined);

    // setend be, whose fixed bits leave no room for anything else
    EXPECT_EQ(Decode(0xB658).operation, Operation::SetEndianness);
    EXPECT_EQ(Decode(0xB648).operation, Operation::Undefined);
}

TEST(DecodeThumb, ReadsTheBranches) {
    // bne to four bytes back, the shape of the bottom of a loop
    const Instruction conditional = Decode(0xD1FB);
    EXPECT_EQ(conditional.operation, Operation::Branch);
    EXPECT_EQ(conditional.condition, Condition::NotEqual);
    EXPECT_EQ(BranchTarget(conditional, Here), Here + 4 - 10);
    EXPECT_TRUE(ContinuesToTheNextInstruction(conditional))
        << "a condition that can fail leaves the next instruction reachable";

    // b forwards, which has eleven bits of displacement rather than eight
    const Instruction unconditional = Decode(0xE002);
    EXPECT_EQ(unconditional.operation, Operation::Branch);
    EXPECT_EQ(unconditional.condition, Condition::Always);
    EXPECT_EQ(BranchTarget(unconditional, Here), Here + 4 + 4);
    EXPECT_FALSE(ContinuesToTheNextInstruction(unconditional));

    // svc #0
    const Instruction call = Decode(0xDF00);
    EXPECT_EQ(call.operation, Operation::SoftwareInterrupt);
    EXPECT_EQ(call.immediate, 0U);

    // The condition a branch cannot carry, which is permanently undefined.
    EXPECT_EQ(Decode(0xDE00).operation, Operation::Undefined);
}

TEST(DecodeThumb, ReadsTheBranchWithLinkAsOneInstruction) {
    // bl, taken from the start of the reference image's first Thumb function
    const Instruction call = DecodeThumb(0xF000, 0xF8CB);
    EXPECT_EQ(call.operation, Operation::BranchLink);
    EXPECT_EQ(call.width, 4);
    EXPECT_TRUE(IsCall(call));
    EXPECT_TRUE(HasKnownTarget(call));
    EXPECT_EQ(BranchTarget(call, 0x00100786), 0x00100920U);
    EXPECT_EQ(call.encoding, 0xF8CBF000U) << "both halfwords are kept";

    // The suffix is what says whether the branch stays in Thumb or exchanges.
    const Instruction exchange = DecodeThumb(0xF053, 0xE99C);
    EXPECT_EQ(exchange.operation, Operation::BranchLinkExchangeImmediate);
    EXPECT_EQ(exchange.width, 4);

    // The target of an exchange is a word boundary, which the architecture
    // reaches by rounding down rather than by refusing.
    EXPECT_EQ(BranchTarget(exchange, 0x00100136), 0x00153470U);
}

TEST(DecodeThumb, RefusesHalfOfABranchWithLink) {
    // A prefix with nothing after it, which is what the end of a buffer looks
    // like. Decoding it as a branch would invent a target.
    EXPECT_EQ(DecodeThumb(0xF000, NoSecondHalfword).operation, Operation::Undefined);

    // A suffix on its own.
    EXPECT_EQ(Decode(0xF8CB).operation, Operation::Undefined);
    EXPECT_EQ(Decode(0xE99C).operation, Operation::Undefined);
}

TEST(DecodeThumb, DecodesEveryHalfwordWithoutFailing) {
    std::uint32_t defined = 0;
    for (std::uint32_t value = 0; value <= 0xFFFF; ++value) {
        if (IsDefined(DecodeThumb(static_cast<std::uint16_t>(value), 0xF800))) {
            ++defined;
        }
    }

    // 58,847 of the 65,536 halfwords are instructions, which is 89.8 percent.
    // Thumb leaves far less of its space undefined than ARM does, because it has
    // sixteen bits to spend rather than thirty-two.
    //
    // The number is exact on purpose. A change in it is a change in what the
    // decoder accepts, which should be deliberate and should be read alongside a
    // run of scripts/compare_arm_decoder.py rather than discovered later.
    EXPECT_EQ(defined, 58847U);
}

}  // namespace
}  // namespace hexlith::domain::arm
