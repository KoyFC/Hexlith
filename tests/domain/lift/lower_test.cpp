#include "domain/arm/decode_arm.h"
#include "domain/arm/decode_thumb.h"
#include "domain/lift/evaluate.h"
#include "domain/lift/lower.h"

#include "differential.h"

#include <cstdint>
#include <gtest/gtest.h>

namespace hexlith::domain::lift {
namespace {

using test::Data;
using test::DefaultSetup;
using test::Machine;

// Encodings checked against an independent disassembler, each with what it is.
// The second column of a test is what makes it a test: the interpreter and the
// lowering are run from the same states and have to leave the same one.

arm::Instruction Arm(std::uint32_t word) {
    return arm::DecodeArm(word);
}

arm::Instruction Thumb(std::uint16_t halfword) {
    return arm::DecodeThumb(halfword, 0);
}

// Whether the instruction is translated, left whole to the interpreter, or
// done by the interpreter's handler called directly. A lowering that sent
// everything to the interpreter would agree with it perfectly, so what a test
// expects is part of the test.
enum class Expect : std::uint8_t { Translated, Interpreted, Handled };

void ExpectAgreement(const arm::Instruction& instruction,
                     const test::Setup& setup = DefaultSetup,
                     Expect expected = Expect::Translated) {
    ASSERT_TRUE(arm::IsDefined(instruction));
    const ir::Instruction lowered =
        Lower(PlacedInstruction{.address = test::Code, .instruction = instruction});
    EXPECT_EQ(lowered.IsInterpreted(), expected == Expect::Interpreted);
    EXPECT_EQ(lowered.IsExecutedByAHandler(), expected == Expect::Handled);
    EXPECT_EQ(test::DisagreementForAllFlags(instruction, setup), "");
}

void ExpectArm(std::uint32_t word,
               const test::Setup& setup = DefaultSetup,
               Expect expected = Expect::Translated) {
    SCOPED_TRACE(testing::Message() << "encoding " << std::hex << word);
    ExpectAgreement(Arm(word), setup, expected);
}

void ExpectThumb(std::uint16_t halfword, const test::Setup& setup = DefaultSetup) {
    SCOPED_TRACE(testing::Message() << "encoding " << std::hex << halfword);
    ExpectAgreement(Thumb(halfword), setup);
}

ir::Instruction LowerArm(std::uint32_t word) {
    return Lower(PlacedInstruction{.address = test::Code, .instruction = Arm(word)});
}

// What a module's code computes from its own address is an offset from where it
// is, and the lowering is given the offset.
TEST(LowerForAModule, AddressesItComputesFromTheProgramCounterMoveWithTheBase) {
    constexpr std::uint32_t Offset = 0x1230;
    constexpr std::uint32_t PipelineOffset = 8;
    for (const std::uint32_t base : {0x006A5000U, 0x007E5000U}) {
        test::Machine machine;
        const ir::Instruction lowered = Lower(PlacedInstruction{
            .address = Offset, .instruction = Arm(0xE28F0004)});  // add r0, pc, #4
        EXPECT_EQ(Evaluate(lowered, machine.state, machine.memory, base).kind,
                  EvaluationKind::Completed);
        EXPECT_EQ(machine.state.registers.at(0), base + Offset + PipelineOffset + 4);
    }
}

TEST(LowerForAModule, TheLinkRegisterOfACallPointsBackIntoTheModule) {
    constexpr std::uint32_t Offset = 0x1230;
    constexpr std::uint32_t InstructionWidth = 4;
    for (const std::uint32_t base : {0x006A5000U, 0x007E5000U}) {
        test::Machine machine;
        const ir::Instruction lowered =
            Lower(PlacedInstruction{.address = Offset, .instruction = Arm(0xE12FFF31)});  // blx r1
        machine.state.registers.at(1) = base + 0x400;
        EXPECT_EQ(Evaluate(lowered, machine.state, machine.memory, base).kind,
                  EvaluationKind::Completed);
        EXPECT_EQ(machine.state.registers.at(arm::LinkRegister), base + Offset + InstructionWidth);
    }
}

TEST(Lower, DataProcessingWithTwoRegisters) {
    ExpectArm(0xE0810002);  // add r0, r1, r2
    ExpectArm(0xE0910002);  // adds r0, r1, r2
    ExpectArm(0xE2510001);  // subs r0, r1, #1
    ExpectArm(0xE3510005);  // cmp r1, #5
    ExpectArm(0xE2710000);  // rsbs r0, r1, #0
    ExpectArm(0xE0A10002);  // adc r0, r1, r2
    ExpectArm(0xE0D10002);  // sbcs r0, r1, r2
    ExpectArm(0xE1C10002);  // bic r0, r1, r2
    ExpectArm(0xE1810002);  // orr r0, r1, r2
    ExpectArm(0xE0210002);  // eor r0, r1, r2
    ExpectArm(0xE1110002);  // tst r1, r2
    ExpectArm(0xE1310002);  // teq r1, r2
    ExpectArm(0xE1710002);  // cmn r1, r2
    ExpectArm(0xE1E00001);  // mvn r0, r1
}

TEST(Lower, ShiftsByAnImmediateAndByARegister) {
    ExpectArm(0xE1B00181);  // lsls r0, r1, #3
    ExpectArm(0xE1A00211);  // lsl r0, r1, r2
    ExpectArm(0xE1B00061);  // rrxs r0, r1
    ExpectArm(0xE1A00021);  // lsr r0, r1, #32
}

TEST(Lower, ARotatedImmediateSetsTheCarryFromItsTopBit) {
    ExpectArm(0xE3A004FF);  // mov r0, #0xff000000
    ExpectArm(0xE3B004FF);  // movs r0, #0xff000000
}

TEST(Lower, ConditionalInstructionsRunOnlyWhenTheConditionPasses) {
    ExpectArm(0x00810002);  // addeq r0, r1, r2
    ExpectArm(0x10810002);  // addne r0, r1, r2
}

TEST(Lower, ReadingThePcGivesTheAddressPlusEight) {
    ExpectArm(0xE28F0008);  // add r0, pc, #8
}

TEST(Lower, AMoveIntoThePcIsABranchAndAMisalignedTargetBails) {
    ExpectArm(0xE1A0F00E);  // mov pc, lr
    const auto misaligned = [](Machine& machine) {
        DefaultSetup(machine);
        machine.state.registers.at(arm::LinkRegister) = test::Code + 0x22;
    };
    ExpectArm(0xE1A0F00E, misaligned);
}

TEST(Lower, LoadsOfEveryWidth) {
    ExpectArm(0xE5910004);  // ldr r0, [r1, #4]
    ExpectArm(0xE5D10001);  // ldrb r0, [r1, #1]
    ExpectArm(0xE5110004);  // ldr r0, [r1, #-4]
    ExpectArm(0xE1D100B2);  // ldrh r0, [r1, #2]
    ExpectArm(0xE1D100F2);  // ldrsh r0, [r1, #2]
    ExpectArm(0xE1D100D1);  // ldrsb r0, [r1, #1]
    ExpectArm(0xE7910002);  // ldr r0, [r1, r2]
    ExpectArm(0xE7910102);  // ldr r0, [r1, r2, lsl #2]
}

TEST(Lower, AnUnalignedWordLoadRotatesNothingAndReadsWhatIsThere) {
    const auto unaligned = [](Machine& machine) {
        DefaultSetup(machine);
        machine.state.registers.at(1) = Data + 0x101;
    };
    ExpectArm(0xE5910000, unaligned);  // ldr r0, [r1]
}

TEST(Lower, TransfersWriteBackBeforeOrAfter) {
    ExpectArm(0xE5B10004);  // ldr r0, [r1, #4]!
    ExpectArm(0xE4910004);  // ldr r0, [r1], #4
    ExpectArm(0xE5A10004);  // str r0, [r1, #4]!
}

TEST(Lower, StoresOfEveryWidth) {
    ExpectArm(0xE5810004);  // str r0, [r1, #4]
    ExpectArm(0xE5C10001);  // strb r0, [r1, #1]
    ExpectArm(0xE1C100B2);  // strh r0, [r1, #2]
    ExpectArm(0xE581F000);  // str pc, [r1]
}

TEST(Lower, ALoadThatFaultsBailsWithoutChangingAnything) {
    const auto unmapped = [](Machine& machine) {
        DefaultSetup(machine);
        machine.state.registers.at(1) = 0x00500000;
    };
    ExpectArm(0xE5910004, unmapped);  // ldr r0, [r1, #4]
    ExpectArm(0xE5810004, unmapped);  // str r0, [r1, #4]
    ExpectArm(0xE5B10004, unmapped);  // ldr r0, [r1, #4]!
}

TEST(Lower, ALoadIntoThePcIsABranchThatMayChangeTheInstructionSet) {
    const auto to_thumb = [](Machine& machine) {
        DefaultSetup(machine);
        EXPECT_TRUE(machine.memory.Write32(Data + 0x100, test::Code + 0x41));
    };
    ExpectArm(0xE591F000, to_thumb);  // ldr pc, [r1]
    ExpectArm(0xE591F000);            // a target that is only halfword aligned
}

TEST(Lower, MultiplyAndMultiplyAccumulate) {
    ExpectArm(0xE0000291);  // mul r0, r1, r2
    ExpectArm(0xE0203291);  // mla r0, r1, r2, r3
    ExpectArm(0xE0100291);  // muls r0, r1, r2
}

TEST(Lower, BranchesWriteTheLinkRegisterAndKnowTheirTarget) {
    ExpectArm(0xEA000000);  // b
    ExpectArm(0xEB000000);  // bl
    ExpectArm(0xFA000000);  // blx, to Thumb

    const ir::Instruction call = LowerArm(0xEB000000);
    ASSERT_TRUE(call.branch_target.has_value());
    EXPECT_EQ(call.branch_target.value_or(0), test::Code + 8);
}

TEST(Lower, BranchesThroughARegisterExchangeTheInstructionSet) {
    const auto to_thumb = [](Machine& machine) {
        DefaultSetup(machine);
        machine.state.registers.at(2) = test::Code + 0x41;
    };
    ExpectArm(0xE12FFF12, to_thumb);  // bx r2
    ExpectArm(0xE12FFF32, to_thumb);  // blx r2
    ExpectArm(0xE12FFF1E);            // bx lr
    const auto halfword = [](Machine& machine) {
        DefaultSetup(machine);
        machine.state.registers.at(2) = test::Code + 0x42;
    };
    ExpectArm(0xE12FFF12, halfword);  // an ARM target that is only halfword aligned
}

TEST(Lower, HintsDoNothing) {
    ExpectArm(0xE320F000);  // nop
    EXPECT_TRUE(LowerArm(0xE320F000).ops.empty());
}

TEST(Lower, TheThreadIdRegistersAndTheBarriers) {
    const auto with_ids = [](Machine& machine) {
        DefaultSetup(machine);
        machine.state.thread_id_read_write = 0xABCD0001;
        machine.state.thread_id_read_only = 0xABCD0002;
    };
    ExpectArm(0xEE1D0F70, with_ids);  // mrc p15, 0, r0, c13, c0, 3
    ExpectArm(0xEE1D0F50, with_ids);  // mrc p15, 0, r0, c13, c0, 2
    ExpectArm(0xEE0D0F50, with_ids);  // mcr p15, 0, r0, c13, c0, 2
    ExpectArm(0xEE070FBA, with_ids);  // mcr p15, 0, r0, c7, c10, 5
    ExpectArm(0xEE070F9A, with_ids);  // mcr p15, 0, r0, c7, c10, 4
}

TEST(Lower, BlockLoadsInEveryAddressingMode) {
    ExpectArm(0xE891000C);  // ldm r1, {r2, r3}
    ExpectArm(0xE8B1000C);  // ldm r1!, {r2, r3}
    ExpectArm(0xE931000C);  // ldmdb r1!, {r2, r3}
    ExpectArm(0xE991000C);  // ldmib r1, {r2, r3}
    ExpectArm(0xE811000C);  // ldmda r1, {r2, r3}
}

TEST(Lower, BlockStoresInEveryAddressingMode) {
    ExpectArm(0xE881000C);  // stm r1, {r2, r3}
    ExpectArm(0xE8A1000C);  // stm r1!, {r2, r3}
    ExpectArm(0xE921000C);  // stmdb r1!, {r2, r3}
    ExpectArm(0xE981000C);  // stmib r1, {r2, r3}
    ExpectArm(0xE801000C);  // stmda r1, {r2, r3}
    ExpectArm(0xE8818004);  // stm r1, {r2, pc}
    ExpectArm(0xE8A10006);  // stm r1!, {r1, r2}, the base stored is the original
}

TEST(Lower, PushAndPopOnTheStack) {
    const auto with_return_address = [](Machine& machine) {
        DefaultSetup(machine);
        machine.state.registers.at(arm::StackPointer) = test::Stack + 0x100;
        EXPECT_TRUE(machine.memory.Write32(test::Stack + 0x104, test::Code + 0x40));
    };
    ExpectArm(0xE92D4010);                       // push {r4, lr}
    ExpectArm(0xE8BD8010, with_return_address);  // pop {r4, pc}
    ExpectThumb(0xB510);                         // push {r4, lr}
    ExpectThumb(0xBD10, with_return_address);    // pop {r4, pc}
    ExpectThumb(0xC90C);                         // ldm r1!, {r2, r3}
    ExpectThumb(0xC10C);                         // stm r1!, {r2, r3}
}

TEST(Lower, ABlockLoadIntoThePcExchangesAndAMisalignedBlockBails) {
    const auto to_thumb = [](Machine& machine) {
        DefaultSetup(machine);
        EXPECT_TRUE(machine.memory.Write32(Data + 0x104, test::Code + 0x41));
    };
    ExpectArm(0xE8918004, to_thumb);  // ldm r1, {r2, pc}
    ExpectArm(0xE8918004);            // a target that is only halfword aligned
    const auto unaligned = [](Machine& machine) {
        DefaultSetup(machine);
        machine.state.registers.at(1) = Data + 0x102;
    };
    ExpectArm(0xE891000C, unaligned);
    ExpectArm(0xE881000C, unaligned);
}

TEST(Lower, ABlockTransferThatFaultsPartWayLeavesWhatTheInterpreterLeaves) {
    // The second word of a block of four is on an unmapped page.
    const auto at_the_end_of_the_page = [](Machine& machine) {
        DefaultSetup(machine);
        machine.state.registers.at(1) = Data + 0x1000 - 8;
    };
    ExpectArm(0xE881003C, at_the_end_of_the_page);  // stm r1, {r2, r3, r4, r5}
    ExpectArm(0xE891003C, at_the_end_of_the_page);  // ldm r1, {r2, r3, r4, r5}
}

TEST(Lower, ABlockWithTheBaseInItsListGoesToTheInterpreterWhenTheArchitectureSaysSo) {
    ExpectArm(0xE8A20006, DefaultSetup, Expect::Interpreted);  // stm r2!, {r1, r2}
}

TEST(Lower, ExtensionsWithAndWithoutAnAddend) {
    ExpectArm(0xE6FF0071);  // uxth r0, r1
    ExpectArm(0xE6EF0071);  // uxtb r0, r1
    ExpectArm(0xE6AF0071);  // sxtb r0, r1
    ExpectArm(0xE6BF0071);  // sxth r0, r1
    ExpectArm(0xE6EF0471);  // uxtb r0, r1, ror #8
    ExpectArm(0xE6E20071);  // uxtab r0, r2, r1
    ExpectArm(0xE6B20871);  // sxtah r0, r2, r1, ror #16
}

TEST(Lower, WhatIsNotTranslatedGoesToTheInterpreterWhole) {
    ExpectArm(0xEF000011, DefaultSetup, Expect::Interpreted);  // svc #0x11
    EXPECT_TRUE(LowerArm(0xEF000011).IsInterpreted());
}

// The families whose work the interpreter's handler does, called directly.
TEST(Lower, FloatingPointAndTheRarerTransfersGoToTheirHandler) {
    ExpectArm(0xEE300A01, DefaultSetup, Expect::Handled);  // vadd.f32 s0, s0, s2
    ExpectArm(0xE1910F9F, DefaultSetup, Expect::Handled);  // ldrex r0, [r1]
    ExpectArm(0xE1812F90, DefaultSetup, Expect::Handled);  // strex r2, r0, [r1]
    ExpectArm(0xE0810392, DefaultSetup, Expect::Handled);  // umull r0, r1, r2, r3
    ExpectArm(0xE16F0F11, DefaultSetup, Expect::Handled);  // clz r0, r1
    ExpectArm(0xE1C120F8, DefaultSetup, Expect::Handled);  // strd r2, r3, [r1, #8]
}

TEST(Lower, ARegisterCombinationTheArchitectureCallsUnpredictableGoesToTheInterpreter) {
    const ir::Instruction movs_pc = LowerArm(0xE1B0F00E);  // movs pc, lr
    EXPECT_TRUE(movs_pc.IsInterpreted());
    ExpectArm(0xE1B0F00E, DefaultSetup, Expect::Interpreted);
}

TEST(Lower, ThumbInstructions) {
    ExpectThumb(0x2100);  // movs r1, #0
    ExpectThumb(0x3001);  // adds r0, #1
    ExpectThumb(0x2800);  // cmp r0, #0
    ExpectThumb(0x6848);  // ldr r0, [r1, #4]
    ExpectThumb(0x6048);  // str r0, [r1, #4]
    ExpectThumb(0x4770);  // bx lr
    ExpectThumb(0x4710);  // bx r2
    ExpectThumb(0x4611);  // mov r1, r2
    ExpectThumb(0x4801);  // ldr r0, [pc, #4]
    ExpectThumb(0xA001);  // adr r0, #4
    ExpectThumb(0x0088);  // lsls r0, r1, #2
}

TEST(Describe, ReadsAsOneOperationToALine) {
    const std::string text = ir::Describe(LowerArm(0xE0810002));  // add r0, r1, r2
    EXPECT_NE(text.find("read-register #1"), std::string::npos);
    EXPECT_NE(text.find("alu"), std::string::npos);
    EXPECT_NE(text.find("write-register #0"), std::string::npos);
}

}  // namespace
}  // namespace hexlith::domain::lift
