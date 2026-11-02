#include "domain/lift/control_flow.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <unordered_set>
#include <vector>

namespace hexlith::domain::lift {
namespace {

namespace arm = domain::arm;
namespace analysis = domain::analysis;

constexpr std::uint32_t Base = 0x00100000;
constexpr std::uint32_t WordBytes = 4;

// Encodings checked against an independent disassembler.
constexpr std::uint32_t ArmPush = 0xE92D4010;                // push {r4, lr}
constexpr std::uint32_t ArmPop = 0xE8BD8010;                 // pop {r4, pc}
constexpr std::uint32_t ArmMove = 0xE1A01002;                // mov r1, r2
constexpr std::uint32_t ArmAddOne = 0xE2800001;              // add r0, r0, #1
constexpr std::uint32_t ArmCompareZero = 0xE3500000;         // cmp r0, #0
constexpr std::uint32_t ArmReturn = 0xE12FFF1E;              // bx lr
constexpr std::uint32_t ArmReturnIfEqual = 0x012FFF1E;       // bxeq lr
constexpr std::uint32_t ArmPopPc = 0xE49DF004;               // pop {pc}
constexpr std::uint32_t ArmPopPcIfEqual = 0x08BD8000;        // ldmeq sp!, {pc}
constexpr std::uint32_t ArmBranchToR3 = 0xE12FFF13;          // bx r3
constexpr std::uint32_t ArmCallR3 = 0xE12FFF33;              // blx r3
constexpr std::uint32_t ArmMovePcFromR3 = 0xE1A0F003;        // mov pc, r3
constexpr std::uint32_t ArmSystemCall = 0xEF000000;          // svc #0
constexpr std::uint32_t ArmUndefined = 0xE7F000F0;           // udf #0
constexpr std::uint32_t ArmCompareWithTwo = 0xE3500002;      // cmp r0, #2
constexpr std::uint32_t ArmDispatch = 0x979FF100;            // ldrls pc, [pc, r0, lsl #2]
constexpr std::uint32_t ArmJumpThroughLiteral = 0xE51FF004;  // ldr pc, [pc, #-4]

constexpr std::uint16_t ThumbCompareZero = 0x2800;  // cmp r0, #0
constexpr std::uint16_t ThumbBranchEqual = 0xD000;  // beq, to the instruction after the next
constexpr std::uint16_t ThumbMove = 0x2100;         // movs r1, #0
constexpr std::uint16_t ThumbReturn = 0x4770;       // bx lr

constexpr std::uint32_t Equal = 0x0;
constexpr std::uint32_t NotEqual = 0x1;
constexpr std::uint32_t Always = 0xE;
constexpr std::uint32_t Link = 0x01000000;

// A branch (or a branch with link) from one address to another.
std::uint32_t
BranchWord(std::uint32_t condition, std::uint32_t from, std::uint32_t to, std::uint32_t link = 0) {
    const std::uint32_t displacement = (to - (from + 8)) / WordBytes;
    return (condition << 28U) | 0x0A000000U | link | (displacement & 0x00FFFFFFU);
}

class ImageBuilder {
public:
    void Arm(std::uint32_t word) {
        for (unsigned index = 0; index < 4; ++index) {
            bytes_.push_back(static_cast<std::uint8_t>(word >> (8U * index)));
        }
    }

    void Thumb(std::uint16_t halfword) {
        bytes_.push_back(static_cast<std::uint8_t>(halfword));
        bytes_.push_back(static_cast<std::uint8_t>(halfword >> 8U));
    }

    [[nodiscard]] std::uint32_t Next() const {
        return Base + static_cast<std::uint32_t>(bytes_.size());
    }

    [[nodiscard]] CodeImage Image() const {
        return CodeImage{.bytes = bytes_, .base = Base};
    }

private:
    std::vector<std::uint8_t> bytes_;
};

Function Build(const ImageBuilder& image,
               arm::InstructionSet set = arm::InstructionSet::Arm,
               const std::unordered_set<std::uint32_t>& entries = {},
               std::span<const analysis::Region> regions = {}) {
    const FlowContext context{.entries = &entries, .regions = regions};
    return BuildFunction(image.Image(), Base, set, context);
}

// For the tests that fold a literal: a caller who knows the bytes do not change.
Function BuildWithConstantLiterals(const ImageBuilder& image,
                                   const std::unordered_set<std::uint32_t>& entries) {
    const FlowContext context{.entries = &entries, .regions = {}, .literals_are_constant = true};
    return BuildFunction(image.Image(), Base, arm::InstructionSet::Arm, context);
}

TEST(BuildFunction, ACallStraightThroughIsOneBlockEndingInAReturn) {
    ImageBuilder image;
    image.Arm(ArmPush);
    image.Arm(ArmMove);
    image.Arm(ArmPop);

    const Function function = Build(image);
    ASSERT_EQ(function.blocks.size(), 1U);
    EXPECT_EQ(function.blocks.front().Cost(), 3U);
    EXPECT_EQ(function.blocks.front().exit.kind, ExitKind::Return);
    EXPECT_FALSE(function.blocks.front().exit.HasNext());
    EXPECT_EQ(function.blocks.front().end, Base + 12);
}

TEST(BuildFunction, AConditionalBranchLeavesTwoWaysOn) {
    // cmp ; beq else ; mov ; b end ; else: mov ; end: pop
    ImageBuilder image;
    image.Arm(ArmCompareZero);                            // Base
    image.Arm(BranchWord(Equal, Base + 4, Base + 16));    // Base + 4
    image.Arm(ArmMove);                                   // Base + 8
    image.Arm(BranchWord(Always, Base + 12, Base + 20));  // Base + 12
    image.Arm(ArmMove);                                   // Base + 16, else
    image.Arm(ArmPop);                                    // Base + 20, end

    const Function function = Build(image);
    ASSERT_EQ(function.blocks.size(), 4U);

    const BasicBlock* head = function.BlockAt(Base);
    ASSERT_NE(head, nullptr);
    EXPECT_EQ(head->Cost(), 2U);
    EXPECT_EQ(head->exit.kind, ExitKind::Jump);
    EXPECT_EQ(head->exit.condition, arm::Condition::Equal);
    EXPECT_EQ(head->exit.target, Base + 16);
    EXPECT_EQ(head->exit.next, Base + 8);

    const BasicBlock* then_part = function.BlockAt(Base + 8);
    ASSERT_NE(then_part, nullptr);
    EXPECT_EQ(then_part->exit.kind, ExitKind::Jump);
    EXPECT_EQ(then_part->exit.condition, arm::Condition::Always);
    EXPECT_FALSE(then_part->exit.HasNext());

    const BasicBlock* else_part = function.BlockAt(Base + 16);
    ASSERT_NE(else_part, nullptr);
    EXPECT_EQ(else_part->exit.kind, ExitKind::Fallthrough)
        << "joins the end, entered from elsewhere";
    EXPECT_EQ(else_part->exit.next, Base + 20);
}

TEST(BuildFunction, ABackwardBranchSplitsTheBlockItLandsIn) {
    // mov ; loop: add ; cmp ; bne loop ; pop
    ImageBuilder image;
    image.Arm(ArmMove);                                    // Base
    image.Arm(ArmAddOne);                                  // Base + 4, loop
    image.Arm(ArmCompareZero);                             // Base + 8
    image.Arm(BranchWord(NotEqual, Base + 12, Base + 4));  // Base + 12
    image.Arm(ArmPop);                                     // Base + 16

    const Function function = Build(image);
    ASSERT_EQ(function.blocks.size(), 3U);
    EXPECT_EQ(function.BlockAt(Base)->Cost(), 1U);
    EXPECT_EQ(function.BlockAt(Base)->exit.kind, ExitKind::Fallthrough);
    EXPECT_EQ(function.BlockAt(Base + 4)->Cost(), 3U);
    EXPECT_EQ(function.BlockAt(Base + 4)->exit.target, Base + 4) << "a loop is a jump to itself";
    EXPECT_EQ(function.BlockAt(Base + 16)->exit.kind, ExitKind::Return);
}

TEST(BuildFunction, ACallEndsItsBlockAndTheCodeAfterItIsABlockOfItsOwn) {
    ImageBuilder image;
    image.Arm(ArmPush);                                           // Base
    image.Arm(BranchWord(Always, Base + 4, Base + 0x100, Link));  // Base + 4
    image.Arm(ArmMove);                                           // Base + 8
    image.Arm(ArmPop);                                            // Base + 12

    const Function function = Build(image);
    ASSERT_EQ(function.blocks.size(), 2U);
    const BasicBlock& before = function.blocks.at(0);
    EXPECT_EQ(before.Cost(), 2U) << "the call itself is paid for before it is made";
    EXPECT_EQ(before.exit.kind, ExitKind::Call);
    EXPECT_EQ(before.exit.target, Base + 0x100);
    EXPECT_EQ(before.exit.target_set, arm::InstructionSet::Arm);
    EXPECT_EQ(before.exit.next, Base + 8);
    EXPECT_EQ(function.blocks.at(1).start, Base + 8);
}

TEST(BuildFunction, ABranchToAnotherFunctionIsATailCall) {
    ImageBuilder image;
    image.Arm(ArmMove);
    image.Arm(BranchWord(Always, Base + 4, Base + 0x40));

    const std::unordered_set<std::uint32_t> entries = {Base, Base + 0x40};
    const Function function = Build(image, arm::InstructionSet::Arm, entries);
    ASSERT_EQ(function.blocks.size(), 1U);
    EXPECT_EQ(function.blocks.front().exit.kind, ExitKind::TailCall);
    EXPECT_EQ(function.blocks.front().exit.target, Base + 0x40);
}

TEST(BuildFunction, ABranchOutOfTheImageIsATailCallToo) {
    ImageBuilder image;
    image.Arm(BranchWord(Always, Base, Base + 0x10000));
    const Function function = Build(image);
    ASSERT_EQ(function.blocks.size(), 1U);
    EXPECT_EQ(function.blocks.front().exit.kind, ExitKind::TailCall);
}

TEST(BuildFunction, ASupervisorCallEndsItsBlockAndContinuesAfterIt) {
    ImageBuilder image;
    image.Arm(ArmMove);
    image.Arm(ArmSystemCall);
    image.Arm(ArmReturn);

    const Function function = Build(image);
    ASSERT_EQ(function.blocks.size(), 2U);
    EXPECT_EQ(function.blocks.front().exit.kind, ExitKind::SystemCall);
    EXPECT_EQ(function.blocks.front().Cost(), 2U);
    EXPECT_EQ(function.blocks.front().exit.next, Base + 8);
}

TEST(BuildFunction, TellsReturnsFromJumpsThroughRegisters) {
    const std::vector<std::pair<std::uint32_t, ExitKind>> cases = {
        {ArmReturn, ExitKind::Return},
        {ArmPopPc, ExitKind::Return},
        {ArmBranchToR3, ExitKind::IndirectJump},
        {ArmMovePcFromR3, ExitKind::IndirectJump},
        {ArmCallR3, ExitKind::IndirectCall},
    };
    for (const auto& [word, kind] : cases) {
        ImageBuilder image;
        image.Arm(word);
        image.Arm(ArmReturn);
        const Function function = Build(image);
        EXPECT_EQ(function.blocks.front().exit.kind, kind) << std::hex << word;
    }
}

TEST(BuildFunction, AConditionalReturnFallsThroughWhenItIsNotTaken) {
    for (const std::uint32_t word : {ArmReturnIfEqual, ArmPopPcIfEqual}) {
        ImageBuilder image;
        image.Arm(word);
        image.Arm(ArmMove);
        image.Arm(ArmReturn);
        const Function function = Build(image);
        ASSERT_EQ(function.blocks.size(), 2U);
        EXPECT_EQ(function.blocks.front().exit.kind, ExitKind::Return);
        EXPECT_EQ(function.blocks.front().exit.condition, arm::Condition::Equal);
        EXPECT_EQ(function.blocks.front().exit.next, Base + 4);
    }
}

TEST(BuildFunction, ACallReturnsToTheNextInstructionWhateverItsCondition) {
    ImageBuilder image;
    image.Arm(ArmCallR3);
    image.Arm(ArmReturn);
    const Function function = Build(image);
    EXPECT_EQ(function.blocks.front().exit.next, Base + 4);
}

TEST(BuildFunction, ReadsTheCasesOfASwitchFromItsTableRegion) {
    ImageBuilder image;
    image.Arm(ArmCompareWithTwo);  // Base
    image.Arm(ArmDispatch);        // Base + 4, the table starts at Base + 12
    image.Arm(ArmReturn);          // Base + 8, the default
    image.Arm(Base + 24);          // Base + 12, case 0
    image.Arm(Base + 28);          // Base + 16
    image.Arm(Base + 24);          // Base + 20
    image.Arm(ArmMove);            // Base + 24
    image.Arm(ArmReturn);          // Base + 28

    const std::vector<analysis::Region> regions = {
        analysis::Region{.address = Base + 12,
                         .size = 12,
                         .kind = analysis::RegionKind::JumpTable,
                         .instruction_set = std::nullopt,
                         .source = analysis::Provenance::Heuristic}};
    const Function function = Build(image, arm::InstructionSet::Arm, {}, regions);

    const BasicBlock* head = function.BlockAt(Base);
    ASSERT_NE(head, nullptr);
    EXPECT_EQ(head->exit.kind, ExitKind::Switch);
    EXPECT_EQ(head->exit.table, Base + 12);
    EXPECT_EQ(head->exit.cases, (std::vector<std::uint32_t>{Base + 24, Base + 28, Base + 24}));
    EXPECT_EQ(head->exit.condition, arm::Condition::LowerOrSame);
    EXPECT_EQ(head->exit.next, Base + 8) << "an index past the table takes the default";
    EXPECT_NE(function.BlockAt(Base + 24), nullptr);
    EXPECT_NE(function.BlockAt(Base + 28), nullptr);
}

TEST(BuildFunction, ALoadIntoThePcWithNoTableIsAnIndirectJump) {
    ImageBuilder image;
    image.Arm(ArmCompareWithTwo);
    image.Arm(ArmDispatch);
    image.Arm(ArmReturn);
    const Function function = Build(image);
    EXPECT_EQ(function.blocks.front().exit.kind, ExitKind::IndirectJump);
    EXPECT_EQ(function.blocks.front().exit.next, Base + 8);
}

TEST(BuildFunction, AWordThatIsNotAnInstructionIsATrapWithNothingBeforeIt) {
    ImageBuilder image;
    image.Arm(ArmMove);
    image.Arm(ArmUndefined);
    const Function function = Build(image);
    ASSERT_EQ(function.blocks.size(), 2U);
    EXPECT_EQ(function.blocks.front().exit.kind, ExitKind::Fallthrough);
    const BasicBlock& trap = function.blocks.back();
    EXPECT_EQ(trap.start, Base + 4);
    EXPECT_EQ(trap.Cost(), 0U);
    EXPECT_EQ(trap.exit.kind, ExitKind::Trap);
    EXPECT_EQ(trap.exit.target, Base + 4);
}

TEST(BuildFunction, RunningOffTheEndOfTheImageIsATrap) {
    ImageBuilder image;
    image.Arm(ArmMove);
    const Function function = Build(image);
    ASSERT_EQ(function.blocks.size(), 2U);
    EXPECT_EQ(function.blocks.back().exit.kind, ExitKind::Trap);
    EXPECT_EQ(function.blocks.back().start, Base + 4);
}

TEST(BuildFunction, TwoPathsThatMeetMakeTheMeetingPointABlock) {
    // beq over one move to a pop that the fall-through reaches as well.
    ImageBuilder image;
    image.Arm(BranchWord(Equal, Base, Base + 12));  // Base
    image.Arm(ArmMove);                             // Base + 4
    image.Arm(ArmAddOne);                           // Base + 8
    image.Arm(ArmPop);                              // Base + 12
    const Function function = Build(image);
    ASSERT_EQ(function.blocks.size(), 3U);
    EXPECT_NE(function.BlockAt(Base + 12), nullptr);
    EXPECT_EQ(function.BlockAt(Base + 4)->exit.kind, ExitKind::Fallthrough);
}

TEST(BuildFunction, ALoadFromTheLiteralAfterItIsABranchToAConstant) {
    // ldr pc, [pc, #-4] with the address in the word that follows it.
    ImageBuilder image;
    image.Arm(ArmJumpThroughLiteral);  // Base
    image.Arm(Base + 0x40);            // Base + 4, the literal

    const std::unordered_set<std::uint32_t> entries = {Base, Base + 0x40};
    const Function function = BuildWithConstantLiterals(image, entries);
    ASSERT_EQ(function.blocks.size(), 1U);
    EXPECT_EQ(function.blocks.front().exit.kind, ExitKind::TailCall);
    EXPECT_EQ(function.blocks.front().exit.target, Base + 0x40);
    EXPECT_EQ(function.blocks.front().exit.target_set, arm::InstructionSet::Arm);
}

TEST(BuildFunction, TheInterworkingBitOfALiteralChoosesTheInstructionSet) {
    ImageBuilder image;
    image.Arm(ArmJumpThroughLiteral);
    image.Arm((Base + 0x40) | 1U);

    const Function function = BuildWithConstantLiterals(image, {});
    EXPECT_EQ(function.blocks.front().exit.target, Base + 0x40);
    EXPECT_EQ(function.blocks.front().exit.target_set, arm::InstructionSet::Thumb);
}

TEST(BuildFunction, LeavesALiteralJumpAloneUnlessTheCallerSaysTheLiteralIsConstant) {
    // In a module, and in the import stubs of the main executable, the loader
    // writes the address after the file is read.
    ImageBuilder image;
    image.Arm(ArmJumpThroughLiteral);
    image.Arm(Base + 0x40);

    const std::unordered_set<std::uint32_t> entries;
    const FlowContext context{.entries = &entries, .regions = {}, .literals_are_constant = false};
    const Function function = BuildFunction(image.Image(), Base, arm::InstructionSet::Arm, context);
    EXPECT_EQ(function.blocks.front().exit.kind, ExitKind::IndirectJump);
}

TEST(BuildFunction, CountsTheInstructionsOfEveryBlock) {
    ImageBuilder image;
    image.Arm(ArmMove);
    image.Arm(ArmMove);
    image.Arm(ArmReturn);
    EXPECT_EQ(Build(image).InstructionCount(), 3U);
}

TEST(BuildFunction, FollowsThumbWithItsOwnWidths) {
    // cmp r0, #0 ; beq to the instruction after the next ; movs ; bx lr
    ImageBuilder image;
    image.Thumb(ThumbCompareZero);
    image.Thumb(ThumbBranchEqual);
    image.Thumb(ThumbMove);
    image.Thumb(ThumbReturn);

    const Function function = Build(image, arm::InstructionSet::Thumb);
    ASSERT_EQ(function.blocks.size(), 3U);
    const BasicBlock& head = function.blocks.front();
    EXPECT_EQ(head.Cost(), 2U);
    EXPECT_EQ(head.exit.kind, ExitKind::Jump);
    EXPECT_EQ(head.exit.target, Base + 6);
    EXPECT_EQ(head.exit.next, Base + 4);
    EXPECT_EQ(function.blocks.back().exit.kind, ExitKind::Return);
}

}  // namespace
}  // namespace hexlith::domain::lift
