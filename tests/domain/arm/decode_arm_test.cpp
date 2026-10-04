#include "domain/arm/decode_arm.h"

#include <cstdint>
#include <gtest/gtest.h>

namespace hexlith::domain::arm {
namespace {

// Every encoding in this file was checked against an independent disassembler
// before it was written down, so a failure here is a change in the decoder
// rather than a doubt about the vector. docs/arm-decoder.md records the sweep
// that covers the space these cases only sample.
constexpr std::uint32_t Here = 0x00100000;

TEST(DecodeArm, MovesAnImmediate) {
    // mov r0, #1
    const Instruction decoded = DecodeArm(0xE3A00001);
    EXPECT_EQ(decoded.operation, Operation::Mov);
    EXPECT_EQ(decoded.condition, Condition::Always);
    EXPECT_EQ(decoded.destination, 0);
    EXPECT_EQ(decoded.first, NoRegister) << "a move has no first operand";
    EXPECT_EQ(decoded.operand, OperandForm::Immediate);
    EXPECT_EQ(decoded.immediate, 1U);
    EXPECT_FALSE(decoded.updates_flags);
    EXPECT_EQ(decoded.width, 4);
    EXPECT_EQ(decoded.instruction_set, InstructionSet::Arm);
}

TEST(DecodeArm, RotatesAnImmediateIntoPlace) {
    // mov r0, #0x4000000, which is 1 rotated right by 4 bits twice over.
    const Instruction decoded = DecodeArm(0xE3A00101);
    EXPECT_EQ(decoded.operation, Operation::Mov);
    EXPECT_EQ(decoded.immediate, 0x40000000U);
}

TEST(DecodeArm, AddsTwoRegisters) {
    // add r0, r1, r2
    const Instruction decoded = DecodeArm(0xE0810002);
    EXPECT_EQ(decoded.operation, Operation::Add);
    EXPECT_EQ(decoded.destination, 0);
    EXPECT_EQ(decoded.first, 1);
    EXPECT_EQ(decoded.second, 2);
    EXPECT_EQ(decoded.operand, OperandForm::Register);
}

TEST(DecodeArm, ReadsAShiftByAnImmediate) {
    // mov r1, r2, lsl #1
    const Instruction decoded = DecodeArm(0xE1A01082);
    EXPECT_EQ(decoded.operation, Operation::Mov);
    EXPECT_EQ(decoded.operand, OperandForm::ShiftedRegister);
    EXPECT_EQ(decoded.second, 2);
    EXPECT_EQ(decoded.shift, ShiftKind::LogicalLeft);
    EXPECT_EQ(decoded.shift_amount, 1);
    EXPECT_EQ(decoded.shift_register, NoRegister);
}

TEST(DecodeArm, ReadsAShiftByARegister) {
    // mov r1, r0, lsr r2
    const Instruction decoded = DecodeArm(0xE1A01230);
    EXPECT_EQ(decoded.operand, OperandForm::ShiftedRegister);
    EXPECT_EQ(decoded.shift, ShiftKind::LogicalRight);
    EXPECT_EQ(decoded.shift_register, 2);
    EXPECT_EQ(decoded.second, 0);
}

TEST(DecodeArm, ReadsTheThreeShiftsSpeltWithAZeroAmount) {
    // A rotate right by zero is a rotate through the carry flag by one.
    const Instruction rotate = DecodeArm(0xE1A01060);
    EXPECT_EQ(rotate.shift, ShiftKind::RotateRightExtended);
    EXPECT_EQ(rotate.shift_amount, 1);

    // A logical right shift by zero is a shift by 32, which is not the same as
    // no shift at all: it clears the register and sets the carry.
    const Instruction logical = DecodeArm(0xE1A01020);
    EXPECT_EQ(logical.shift, ShiftKind::LogicalRight);
    EXPECT_EQ(logical.shift_amount, 32);

    // A logical left shift by zero is the only one that is really no shift, and
    // it is how a plain register operand is spelt.
    const Instruction plain = DecodeArm(0xE1A01000);
    EXPECT_EQ(plain.operand, OperandForm::Register);
    EXPECT_EQ(plain.shift_amount, 0);
}

TEST(DecodeArm, ReadsTheFlagSettingBit) {
    // movs r1, r0
    const Instruction decoded = DecodeArm(0xE1B01000);
    EXPECT_EQ(decoded.operation, Operation::Mov);
    EXPECT_TRUE(decoded.updates_flags);
}

TEST(DecodeArm, ComparisonsHaveNoDestination) {
    // cmp r0, #0
    const Instruction immediate = DecodeArm(0xE3500000);
    EXPECT_EQ(immediate.operation, Operation::Cmp);
    EXPECT_EQ(immediate.first, 0);
    EXPECT_EQ(immediate.destination, NoRegister);
    EXPECT_TRUE(immediate.updates_flags) << "a comparison sets the flags by definition";

    // cmp r1, r2
    const Instruction registers = DecodeArm(0xE1510002);
    EXPECT_EQ(registers.operation, Operation::Cmp);
    EXPECT_EQ(registers.first, 1);
    EXPECT_EQ(registers.second, 2);
}

TEST(DecodeArm, ReadsEveryCondition) {
    // The condition is the top four bits, so adding to them walks through all
    // sixteen. b with a displacement of zero is the body.
    for (std::uint32_t value = 0; value < 15; ++value) {
        const std::uint32_t word = 0x0A000000 | (value << 28U);
        const Instruction decoded = DecodeArm(word);
        EXPECT_EQ(decoded.operation, Operation::Branch) << value;
        EXPECT_EQ(decoded.condition, static_cast<Condition>(value)) << value;
    }
}

TEST(DecodeArm, ResolvesABranchForwardsAndBackwards) {
    // b with a displacement of zero reaches two instructions on, because that is
    // where the program counter already points.
    const Instruction forwards = DecodeArm(0xEA000000);
    EXPECT_EQ(forwards.operation, Operation::Branch);
    EXPECT_TRUE(HasKnownTarget(forwards));
    EXPECT_EQ(BranchTarget(forwards, Here), Here + 8);

    // b to itself, which is how a spin loop is written.
    const Instruction backwards = DecodeArm(0xEAFFFFFE);
    EXPECT_EQ(BranchTarget(backwards, Here), Here);

    // bl two instructions further on.
    const Instruction call = DecodeArm(0xEB000002);
    EXPECT_EQ(call.operation, Operation::BranchLink);
    EXPECT_TRUE(IsCall(call));
    EXPECT_EQ(BranchTarget(call, Here), Here + 0x10);
}

TEST(DecodeArm, ReadsTheExchangingBranches) {
    // bx lr
    const Instruction ret = DecodeArm(0xE12FFF1E);
    EXPECT_EQ(ret.operation, Operation::BranchExchange);
    EXPECT_EQ(ret.second, LinkRegister);
    EXPECT_TRUE(IsReturn(ret));
    EXPECT_FALSE(HasKnownTarget(ret));

    // blx r3
    const Instruction call = DecodeArm(0xE12FFF33);
    EXPECT_EQ(call.operation, Operation::BranchLinkExchangeRegister);
    EXPECT_EQ(call.second, 3);
    EXPECT_TRUE(IsCall(call));

    // blx to a displacement, which is unconditional by encoding and which is how
    // ARM code calls Thumb code at a known address.
    const Instruction exchange = DecodeArm(0xFA000000);
    EXPECT_EQ(exchange.operation, Operation::BranchLinkExchangeImmediate);
    EXPECT_EQ(exchange.condition, Condition::Unconditional);
    EXPECT_EQ(BranchTarget(exchange, Here), Here + 8);
}

TEST(DecodeArm, ReadsAPushAndAPop) {
    // push {r4, lr}
    const Instruction push = DecodeArm(0xE92D4010);
    EXPECT_EQ(push.operation, Operation::StoreMultiple);
    EXPECT_EQ(push.first, StackPointer);
    EXPECT_TRUE(push.writes_back);
    EXPECT_TRUE(push.preindexed);
    EXPECT_FALSE(push.add_offset);
    EXPECT_TRUE(Holds(push.registers, 4));
    EXPECT_TRUE(Holds(push.registers, LinkRegister));
    EXPECT_FALSE(WritesProgramCounter(push));

    // pop {r4, pc}
    const Instruction pop = DecodeArm(0xE8BD8010);
    EXPECT_EQ(pop.operation, Operation::LoadMultiple);
    EXPECT_EQ(pop.first, StackPointer);
    EXPECT_TRUE(Holds(pop.registers, ProgramCounter));
    EXPECT_TRUE(WritesProgramCounter(pop));
    EXPECT_TRUE(IsReturn(pop));
    EXPECT_FALSE(ContinuesToTheNextInstruction(pop));
}

TEST(DecodeArm, FindsALiteralPoolLoad) {
    // ldr r0, [pc, #0x10]
    const Instruction forwards = DecodeArm(0xE59F0010);
    EXPECT_EQ(forwards.operation, Operation::LoadWord);
    EXPECT_EQ(forwards.first, ProgramCounter);
    EXPECT_EQ(forwards.operand, OperandForm::Immediate);
    EXPECT_EQ(forwards.immediate, 0x10U);
    EXPECT_TRUE(forwards.add_offset);
    EXPECT_EQ(ProgramCounterRelativeAddress(forwards, Here), Here + 8 + 0x10);

    // ldr r0, [pc, #-0x10]
    const Instruction backwards = DecodeArm(0xE51F0010);
    EXPECT_FALSE(backwards.add_offset);
    EXPECT_EQ(ProgramCounterRelativeAddress(backwards, Here), Here + 8 - 0x10);
}

TEST(DecodeArm, ReadsTheSingleTransfers) {
    // str r1, [r0, #4]
    const Instruction store = DecodeArm(0xE5801004);
    EXPECT_EQ(store.operation, Operation::StoreWord);
    EXPECT_EQ(store.destination, 1);
    EXPECT_EQ(store.first, 0);
    EXPECT_EQ(store.immediate, 4U);
    EXPECT_TRUE(store.preindexed);
    EXPECT_FALSE(store.writes_back);

    // ldr r1, [r0, r2]
    const Instruction indexed = DecodeArm(0xE7901002);
    EXPECT_EQ(indexed.operation, Operation::LoadWord);
    EXPECT_EQ(indexed.operand, OperandForm::Register);
    EXPECT_EQ(indexed.second, 2);

    // ldrh r1, [r0] and strh r1, [r0]
    EXPECT_EQ(DecodeArm(0xE1D010B0).operation, Operation::LoadHalf);
    EXPECT_EQ(DecodeArm(0xE1C010B0).operation, Operation::StoreHalf);

    // ldrsb r1, [r0, #0x10], whose offset is split across two fields
    const Instruction signed_byte = DecodeArm(0xE1D011D0);
    EXPECT_EQ(signed_byte.operation, Operation::LoadSignedByte);
    EXPECT_EQ(signed_byte.immediate, 0x10U);

    // ldrd r4, r5, [r0, #0x20]
    const Instruction pair = DecodeArm(0xE1C042D0);
    EXPECT_EQ(pair.operation, Operation::LoadDouble);
    EXPECT_EQ(pair.destination, 4);
    EXPECT_EQ(pair.immediate, 0x20U);
}

TEST(DecodeArm, ReadsTheMultiplies) {
    // mul r1, r3, r2
    const Instruction simple = DecodeArm(0xE0010293);
    EXPECT_EQ(simple.operation, Operation::Mul);
    EXPECT_EQ(simple.destination, 1);
    EXPECT_EQ(simple.second, 3);
    EXPECT_EQ(simple.third, 2);
    EXPECT_EQ(simple.first, NoRegister) << "a plain multiply accumulates nothing";

    // mla r3, r2, r4, r1
    const Instruction accumulating = DecodeArm(0xE0231492);
    EXPECT_EQ(accumulating.operation, Operation::Mla);
    EXPECT_EQ(accumulating.destination, 3);
    EXPECT_EQ(accumulating.first, 1);

    // umull r4, r5, r2, r3: the two destinations are the fields a short multiply
    // uses for its destination and its addend, which is easy to read backwards.
    const Instruction unsigned_long = DecodeArm(0xE0854392);
    EXPECT_EQ(unsigned_long.operation, Operation::Umull);
    EXPECT_EQ(unsigned_long.destination, 4);
    EXPECT_EQ(unsigned_long.destination_high, 5);
    EXPECT_EQ(unsigned_long.second, 2);
    EXPECT_EQ(unsigned_long.third, 3);

    EXPECT_EQ(DecodeArm(0xE0C54392).operation, Operation::Smull);
}

TEST(DecodeArm, ReadsTheInstructionsHiddenBesideTheComparisons) {
    // clz r1, r0
    const Instruction count = DecodeArm(0xE16F1F10);
    EXPECT_EQ(count.operation, Operation::CountLeadingZeros);
    EXPECT_EQ(count.destination, 1);
    EXPECT_EQ(count.second, 0);

    // mrs r0, cpsr
    const Instruction from_status = DecodeArm(0xE10F0000);
    EXPECT_EQ(from_status.operation, Operation::MoveFromStatus);
    EXPECT_EQ(from_status.destination, 0);
    EXPECT_FALSE(from_status.saved_status);

    // msr cpsr_c, r0
    const Instruction to_status = DecodeArm(0xE121F000);
    EXPECT_EQ(to_status.operation, Operation::MoveToStatus);
    EXPECT_EQ(to_status.second, 0);

    // qadd r1, r1, r3
    const Instruction saturating = DecodeArm(0xE1031051);
    EXPECT_EQ(saturating.operation, Operation::Qadd);
    EXPECT_EQ(saturating.destination, 1);
    EXPECT_EQ(saturating.first, 3);
    EXPECT_EQ(saturating.second, 1);

    // swp r1, r3, [r2] and swpb
    const Instruction swap = DecodeArm(0xE1021093);
    EXPECT_EQ(swap.operation, Operation::Swap);
    EXPECT_EQ(swap.first, 2);
    EXPECT_EQ(DecodeArm(0xE1421093).operation, Operation::SwapByte);
}

TEST(DecodeArm, ReadsTheHints) {
    const Instruction nothing = DecodeArm(0xE320F000);
    EXPECT_EQ(nothing.operation, Operation::NoOperation);
    EXPECT_TRUE(ContinuesToTheNextInstruction(nothing));

    EXPECT_EQ(DecodeArm(0xE320F001).operation, Operation::Yield);
    EXPECT_EQ(DecodeArm(0xE320F002).operation, Operation::WaitForEvent);
    EXPECT_EQ(DecodeArm(0xE320F003).operation, Operation::WaitForInterrupt);
    EXPECT_EQ(DecodeArm(0xE320F004).operation, Operation::SendEvent);

    // ARMv6K allocates five and leaves the rest of the field undefined.
    EXPECT_EQ(DecodeArm(0xE320F005).operation, Operation::Undefined);
    EXPECT_EQ(DecodeArm(0xE320F0FF).operation, Operation::Undefined);
}

TEST(DecodeArm, ReadsTheExtensionsAndReversals) {
    // sxth r1, r2 and uxth r1, r2, which extend without an addend: the field
    // that would name one holds all ones.
    const Instruction extend = DecodeArm(0xE6BF1072);
    EXPECT_EQ(extend.operation, Operation::Sxth);
    EXPECT_EQ(extend.destination, 1);
    EXPECT_EQ(extend.second, 2);
    EXPECT_EQ(extend.first, NoRegister);

    EXPECT_EQ(DecodeArm(0xE6FF1072).operation, Operation::Uxth);
    EXPECT_EQ(DecodeArm(0xE6AF1071).operation, Operation::Sxtb);

    // rev r1, r1
    EXPECT_EQ(DecodeArm(0xE6BF1F31).operation, Operation::Rev);

    // pkhbt r1, r1, r2, lsl #30 and pkhtb r1, r1, r2, asr #1
    const Instruction pack = DecodeArm(0xE6811F12);
    EXPECT_EQ(pack.operation, Operation::Pkhbt);
    EXPECT_EQ(pack.shift, ShiftKind::LogicalLeft);
    EXPECT_EQ(pack.shift_amount, 30);
    EXPECT_EQ(DecodeArm(0xE68110D2).operation, Operation::Pkhtb);

    // qadd16 r1, r1, r2, one of the thirty-six instructions reported as one
    // operation.
    const Instruction parallel = DecodeArm(0xE6211F12);
    EXPECT_EQ(parallel.operation, Operation::ParallelArithmetic);
    EXPECT_EQ(parallel.parallel_prefix, ParallelPrefix::SignedSaturating);
    EXPECT_EQ(parallel.parallel_shape, ParallelShape::Add16);
}

TEST(DecodeArm, ReadsTheLanesOfAParallelInstruction) {
    // uqadd8 r0, r1, r2
    const Instruction saturating = DecodeArm(0xE6610F92);
    EXPECT_EQ(saturating.operation, Operation::ParallelArithmetic);
    EXPECT_EQ(saturating.parallel_prefix, ParallelPrefix::UnsignedSaturating);
    EXPECT_EQ(saturating.parallel_shape, ParallelShape::Add8);
    EXPECT_EQ(saturating.destination, 0);
    EXPECT_EQ(saturating.first, 1);
    EXPECT_EQ(saturating.second, 2);

    // shasx r3, r4, r5, the exchanging shape ARMv6 calls shaddsubx
    const Instruction halving = DecodeArm(0xE6343F35);
    EXPECT_EQ(halving.parallel_prefix, ParallelPrefix::SignedHalving);
    EXPECT_EQ(halving.parallel_shape, ParallelShape::AddSubtractExchange);

    // usub8 r0, r1, r2, the shape in the last row of the table
    const Instruction subtract = DecodeArm(0xE6510FF2);
    EXPECT_EQ(subtract.parallel_prefix, ParallelPrefix::Unsigned);
    EXPECT_EQ(subtract.parallel_shape, ParallelShape::Subtract8);
}

TEST(DecodeArm, RefusesTheBlankRowsOfTheParallelTable) {
    // Shape 101 under the signed prefix, a row the table leaves empty.
    EXPECT_EQ(DecodeArm(0xE6110FB2).operation, Operation::Undefined);

    // Prefix 100, which is not a prefix.
    EXPECT_EQ(DecodeArm(0xE6410F12).operation, Operation::Undefined);
}

TEST(ParallelMnemonic, SpellsThePrefixAndTheShape) {
    EXPECT_EQ(ParallelMnemonic(ParallelPrefix::UnsignedHalving, ParallelShape::Subtract16),
              "uhsub16");
    EXPECT_EQ(ParallelMnemonic(ParallelPrefix::Signed, ParallelShape::SubtractAddExchange), "ssax");
}

TEST(DecodeArm, ReadsTheSynchronisationInstructions) {
    // ldrex r2, [r0]
    const Instruction load = DecodeArm(0xE1902F9F);
    EXPECT_EQ(load.operation, Operation::LoadExclusive);
    EXPECT_EQ(load.destination, 2);
    EXPECT_EQ(load.first, 0);

    // strex r3, r2, [r0]
    const Instruction store = DecodeArm(0xE1803F92);
    EXPECT_EQ(store.operation, Operation::StoreExclusive);
    EXPECT_EQ(store.destination, 3);
    EXPECT_EQ(store.second, 2);

    // clrex, which has one encoding and no operands
    const Instruction clear = DecodeArm(0xF57FF01F);
    EXPECT_EQ(clear.operation, Operation::ClearExclusive);
    EXPECT_EQ(clear.condition, Condition::Unconditional);
}

TEST(DecodeArm, ReadsTheSystemInstructions) {
    // svc #0, which is how every kernel call in the title reaches the kernel.
    const Instruction call = DecodeArm(0xEF000000);
    EXPECT_EQ(call.operation, Operation::SoftwareInterrupt);
    EXPECT_EQ(call.immediate, 0U);

    const Instruction numbered = DecodeArm(0xEF000032);
    EXPECT_EQ(numbered.operation, Operation::SoftwareInterrupt);
    EXPECT_EQ(numbered.immediate, 0x32U);

    // bkpt #0, which is unconditional: a condition on it is unpredictable.
    EXPECT_EQ(DecodeArm(0xE1200070).operation, Operation::Breakpoint);
    EXPECT_EQ(DecodeArm(0x11200070).operation, Operation::Undefined);
}

TEST(DecodeArm, ReadsCoprocessorAccess) {
    // mrc p15, 0, r0, c1, c0, 0, which reads the system control register.
    const Instruction read = DecodeArm(0xEE110F10);
    EXPECT_EQ(read.operation, Operation::MoveFromCoprocessor);
    EXPECT_EQ(read.coprocessor, 15);
    EXPECT_EQ(read.destination, 0);

    const Instruction write = DecodeArm(0xEE010F10);
    EXPECT_EQ(write.operation, Operation::MoveToCoprocessor);
    EXPECT_EQ(write.coprocessor, 15);

    // A floating point instruction is encoded as a coprocessor instruction on 10
    // or 11 and decoded as the floating point instruction it is. This one reads
    // the floating point system identity register.
    const Instruction floating = DecodeArm(0xEEF00A10);
    EXPECT_EQ(floating.operation, Operation::VfpMoveFromSystem);
    EXPECT_EQ(floating.immediate, 0U);
}

TEST(DecodeArm, RefusesAWordThatIsNotAnInstruction) {
    // A block transfer of no registers.
    EXPECT_EQ(DecodeArm(0xE8BD0000).operation, Operation::Undefined);

    // A doubleword transfer naming an odd register, which is half of a pair that
    // does not exist.
    EXPECT_EQ(DecodeArm(0xE1C052D0).operation, Operation::Undefined);

    // A hint whose rotation field is not zero.
    EXPECT_EQ(DecodeArm(0xE320F100).operation, Operation::Undefined);

    // The bitfield group, which ARMv6T2 added and the ARM11 does not have.
    EXPECT_EQ(DecodeArm(0xE7C01011).operation, Operation::Undefined);
}

TEST(DecodeArm, RefusesAWordTheArchitectureCallsUnpredictable) {
    // cmp r0, #0 with a destination field that is not zero. The architecture
    // fixes that field, so the word is not a comparison; a disassembler for a
    // later architecture decodes it as one anyway.
    //
    // Refusing it is deliberate. A compiler never emits it, so a word that looks
    // like this is far more likely to be data, and the decoder's answer to that
    // question is what the classification pass is built on.
    EXPECT_EQ(DecodeArm(0xE3500000).operation, Operation::Cmp);
    EXPECT_EQ(DecodeArm(0xE3501000).operation, Operation::Undefined);

    // mov with a first operand field that is not zero.
    EXPECT_EQ(DecodeArm(0xE3A10001).operation, Operation::Undefined);

    // bx with any of the twelve fixed bits cleared.
    EXPECT_EQ(DecodeArm(0xE12FFF1E).operation, Operation::BranchExchange);
    EXPECT_EQ(DecodeArm(0xE10FFF1E).operation, Operation::Undefined);
}

TEST(DecodeArm, DecodesEveryWordWithoutFailing) {
    // The decoder is total: it answers for every word, and the answer for the
    // ones ARMv6K does not define is Undefined. A sweep of the whole space is
    // what the comparison against another disassembler does; this only checks
    // that nothing here throws or reads out of a table.
    std::uint32_t defined = 0;
    for (std::uint32_t high = 0; high < 0x100; ++high) {
        for (std::uint32_t low = 0; low < 0x100; ++low) {
            const std::uint32_t word = (high << 24U) | (low << 8U) | 0x00A50000U;
            if (IsDefined(DecodeArm(word))) {
                ++defined;
            }
        }
    }
    EXPECT_GT(defined, 0x8000U) << "most of a sample this shape should decode";
}

}  // namespace
}  // namespace hexlith::domain::arm
