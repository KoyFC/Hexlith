#include "domain/analysis/code_walk.h"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::analysis {
namespace {

namespace arm = domain::arm;

constexpr std::uint32_t Base = 0x00100000;

// The places the fixtures below use, named so that a test reads as a sequence of
// addresses rather than of offsets.
constexpr std::uint32_t SecondWord = Base + 4;
constexpr std::uint32_t ThirdWord = Base + 8;
constexpr std::uint32_t FourthWord = Base + 12;
constexpr std::uint32_t SixthWord = Base + 20;

// Filler that is not an instruction and not an address.
constexpr std::uint8_t Padding = 0xFF;
constexpr std::uint32_t NotAnAddress = 0x12345678;
constexpr std::uint32_t FarOutsideTheImage = 0x7FFFFFFF;
constexpr std::uint32_t PastTheEnd = 0x1000;

// Encodings checked against an independent disassembler. A test that assembles
// its own fixtures needs them to be right, and nothing here is a guess.
constexpr std::uint32_t ArmPush = 0xE92D4010;      // push {r4, lr}
constexpr std::uint32_t ArmPop = 0xE8BD8010;       // pop {r4, pc}
constexpr std::uint32_t ArmReturn = 0xE12FFF1E;    // bx lr
constexpr std::uint32_t ArmNop = 0xE320F000;       // nop
constexpr std::uint32_t ArmEmptyPop = 0xE8BD0000;  // not an instruction
constexpr std::uint32_t ArmMove = 0xE1A01002;      // mov r1, r2
constexpr std::uint16_t ThumbPush = 0xB510;        // push {r4, lr}
constexpr std::uint16_t ThumbPop = 0xBD10;         // pop {r4, pc}
constexpr std::uint16_t ThumbMove = 0x2100;        // movs r1, #0

// A branch with link forwards by one instruction beyond the next, which lands
// four instructions after the branch.
constexpr std::uint32_t ArmCallTwoAhead = 0xEB000002;

// A branch with link that exchanges into Thumb at the address the program
// counter already points to.
constexpr std::uint32_t ArmExchangeToHere = 0xFA000000;

// ldr r0, [pc, #0], which reads the word two instructions on.
constexpr std::uint32_t ArmLoadLiteral = 0xE59F0000;

// sub sp, sp, #8, the other shape a function starts with.
constexpr std::uint32_t ArmAdjustStack = 0xE24DD008;

// bl a long way back, which lands outside any image a test builds.
constexpr std::uint32_t ArmCallFarBack = 0xEBFFF000;

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

    void Word(std::uint32_t value) {
        Arm(value);
    }

    void PadTo(std::size_t size) {
        bytes_.resize(size, Padding);
    }

    [[nodiscard]] std::uint32_t AddressOfNext() const {
        return Base + static_cast<std::uint32_t>(bytes_.size());
    }

    [[nodiscard]] const std::vector<std::uint8_t>& Bytes() const {
        return bytes_;
    }

private:
    std::vector<std::uint8_t> bytes_;
};

WalkOptions Options(const ImageBuilder& image) {
    const auto end = static_cast<std::uint32_t>(Base + image.Bytes().size());
    return WalkOptions{.follow_literal_pointers = true,
                       .code_begin = Base,
                       .code_end = end,
                       .pointer_scan_begin = 0,
                       .pointer_scan_end = 0};
}

WalkSeed ArmSeed(std::uint32_t address) {
    return WalkSeed{.address = address,
                    .instruction_set = arm::InstructionSet::Arm,
                    .source = Provenance::ExHeader};
}

const Region* RegionAt(const WalkResult& result, std::uint32_t address) {
    const auto found = std::ranges::find_if(
        result.regions, [address](const Region& region) { return region.address == address; });
    return found == result.regions.end() ? nullptr : &*found;
}

bool HasFunction(const WalkResult& result, std::uint32_t address, arm::InstructionSet set) {
    return std::ranges::any_of(result.functions, [address, set](const FunctionRange& function) {
        return function.address == address && function.instruction_set == set;
    });
}

TEST(WalkCode, FollowsACallAndStopsAtTheReturn) {
    ImageBuilder image;
    image.Arm(ArmPush);          // 0x00100000
    image.Arm(ArmCallTwoAhead);  // 0x00100004, calls 0x00100014
    image.Arm(ArmPop);           // 0x00100008, and the walk stops here
    image.Arm(ArmEmptyPop);      // 0x0010000C, never reached
    image.Arm(ArmEmptyPop);      // 0x00100010
    image.Arm(ArmPush);          // 0x00100014, the function that was called
    image.Arm(ArmPop);           // 0x00100018

    const std::vector<WalkSeed> seeds = {ArmSeed(Base)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    EXPECT_EQ(result.report.instructions, 5U);
    EXPECT_EQ(result.report.undefined_stops, 0U);
    EXPECT_TRUE(result.report.instruction_set_conflicts.empty());
    EXPECT_TRUE(HasFunction(result, SixthWord, arm::InstructionSet::Arm));

    // Two regions: the caller, and the callee past the two words nothing reaches.
    ASSERT_EQ(result.regions.size(), 2U);
    EXPECT_EQ(result.regions[0].address, Base);
    EXPECT_EQ(result.regions[0].size, 12U);
    EXPECT_EQ(result.regions[0].kind, RegionKind::Code);
    EXPECT_EQ(result.regions[0].instruction_set, arm::InstructionSet::Arm);
    EXPECT_EQ(result.regions[1].address, SixthWord);
    EXPECT_EQ(result.regions[1].size, 8U);
}

TEST(WalkCode, SaysNothingAboutWhatItDoesNotReach) {
    ImageBuilder image;
    image.Arm(ArmReturn);
    image.Arm(ArmMove);
    image.Arm(ArmMove);

    const std::vector<WalkSeed> seeds = {ArmSeed(Base)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    // The two words after the return decode perfectly well and are not claimed to
    // be anything, because nothing reached them. An absent region is the absence
    // of knowledge rather than a claim that the bytes are data.
    ASSERT_EQ(result.regions.size(), 1U);
    EXPECT_EQ(result.regions.front().size, 4U);
}

TEST(WalkCode, StopsAtAWordThatIsNotAnInstruction) {
    ImageBuilder image;
    image.Arm(ArmNop);
    image.Arm(ArmEmptyPop);
    image.Arm(ArmNop);

    const std::vector<WalkSeed> seeds = {ArmSeed(Base)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    EXPECT_EQ(result.report.instructions, 1U);
    EXPECT_EQ(result.report.undefined_stops, 1U);
    ASSERT_EQ(result.regions.size(), 1U);
    EXPECT_EQ(result.regions.front().size, 4U);
}

TEST(WalkCode, ChangesInstructionSetAtAnExchangingBranch) {
    ImageBuilder image;
    image.Arm(ArmPush);            // 0x00100000
    image.Arm(ArmExchangeToHere);  // 0x00100004, exchanges into 0x0010000C
    image.Arm(ArmPop);             // 0x00100008
    image.Thumb(ThumbPush);        // 0x0010000C, in Thumb from here
    image.Thumb(ThumbMove);
    image.Thumb(ThumbPop);

    const std::vector<WalkSeed> seeds = {ArmSeed(Base)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    EXPECT_TRUE(result.report.instruction_set_conflicts.empty());
    EXPECT_TRUE(HasFunction(result, FourthWord, arm::InstructionSet::Thumb))
        << "the exchange establishes which encoding the target is in";

    const Region* thumb = RegionAt(result, FourthWord);
    ASSERT_NE(thumb, nullptr);
    EXPECT_EQ(thumb->kind, RegionKind::Code);
    EXPECT_EQ(thumb->instruction_set, arm::InstructionSet::Thumb);
    EXPECT_EQ(thumb->size, 6U);
}

TEST(WalkCode, ReportsAnAddressReachedInBothInstructionSets) {
    ImageBuilder image;
    image.Arm(ArmPush);            // 0x00100000
    image.Arm(ArmExchangeToHere);  // 0x00100004, reaches 0x0010000C as Thumb
    image.Arm(ArmPop);             // 0x00100008
    image.Thumb(ThumbPush);        // 0x0010000C
    image.Thumb(ThumbPop);

    // A second seed claims the same address is ARM, which contradicts the branch.
    const std::vector<WalkSeed> seeds = {ArmSeed(Base), ArmSeed(FourthWord)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    EXPECT_EQ(result.report.instruction_set_conflicts, std::vector<std::uint32_t>{FourthWord})
        << "one of the two paths is wrong and the walk has to say where";
}

TEST(WalkCode, MarksALiteralPoolWordAsDataAndFollowsWhatItPointsAt) {
    ImageBuilder image;
    image.Arm(ArmLoadLiteral);  // 0x00100000, reads 0x00100008
    image.Arm(ArmReturn);       // 0x00100004
    image.Word(FourthWord);     // 0x00100008, the pool entry
    image.Arm(ArmPush);         // 0x0010000C, what it points at
    image.Arm(ArmPop);

    const std::vector<WalkSeed> seeds = {ArmSeed(Base)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    EXPECT_EQ(result.report.literal_pool_words, 1U);
    EXPECT_EQ(result.report.pointers_followed, 1U);

    const Region* pool = RegionAt(result, Base + 8);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(pool->kind, RegionKind::Data);
    EXPECT_EQ(pool->size, 4U);
    EXPECT_FALSE(pool->instruction_set.has_value());

    ASSERT_EQ(result.pointers.size(), 1U);
    EXPECT_EQ(result.pointers.front().address, Base + 8);
    EXPECT_EQ(result.pointers.front().target, FourthWord);
    EXPECT_EQ(result.pointers.front().source, Provenance::Heuristic);
    EXPECT_TRUE(HasFunction(result, FourthWord, arm::InstructionSet::Arm));
}

TEST(WalkCode, TakesTheInterworkingBitOfAPointerAsTheInstructionSet) {
    ImageBuilder image;
    image.Arm(ArmLoadLiteral);
    image.Arm(ArmReturn);
    image.Word(FourthWord + 1);  // odd, so what it points at is Thumb
    image.Thumb(ThumbPush);
    image.Thumb(ThumbPop);

    const std::vector<WalkSeed> seeds = {ArmSeed(Base)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    ASSERT_EQ(result.pointers.size(), 1U);
    EXPECT_EQ(result.pointers.front().target, FourthWord) << "the bit is not part of the address";
    EXPECT_TRUE(HasFunction(result, FourthWord, arm::InstructionSet::Thumb));
}

TEST(WalkCode, LeavesAWordThatIsNotAnAddressAlone) {
    ImageBuilder image;
    image.Arm(ArmLoadLiteral);
    image.Arm(ArmReturn);
    image.Word(NotAnAddress);  // outside the code range
    image.Arm(ArmPush);

    const std::vector<WalkSeed> seeds = {ArmSeed(Base)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    EXPECT_EQ(result.report.pointers_ignored, 1U);
    EXPECT_EQ(result.report.pointers_followed, 0U);
    EXPECT_TRUE(result.pointers.empty());
    EXPECT_EQ(RegionAt(result, FourthWord), nullptr) << "nothing reached it";
}

TEST(WalkCode, RefusesASeedItCannotStartFrom) {
    ImageBuilder image;
    image.Arm(ArmReturn);

    const std::vector<WalkSeed> seeds = {
        ArmSeed(Base + 2),           // ARM has to start at a multiple of four
        ArmSeed(Base + PastTheEnd),  // past the end
    };
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    EXPECT_EQ(result.report.misaligned_seeds, 1U);
    EXPECT_EQ(result.report.seeds_outside_the_image, 1U);
    EXPECT_EQ(result.report.instructions, 0U);
}

TEST(WalkCode, CountsABranchOutOfTheImageWithoutFollowingIt) {
    ImageBuilder image;
    image.Arm(ArmCallFarBack);
    image.Arm(ArmReturn);

    const std::vector<WalkSeed> seeds = {ArmSeed(Base)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    EXPECT_EQ(result.report.targets_outside_the_image, 1U);
    EXPECT_EQ(result.report.instructions, 2U) << "a call comes back, so the walk carries on";
}

TEST(WalkCode, SweepsDataForEntryPointsAndRequiresAPrologue) {
    ImageBuilder image;
    image.Arm(ArmPush);  // 0x00100000, a function nothing calls
    image.Arm(ArmPop);
    image.Arm(ArmMove);  // 0x00100008, not the start of anything
    image.Arm(ArmReturn);

    const std::uint32_t data = image.AddressOfNext();
    image.Word(Base);                // points at the push
    image.Word(ThirdWord);           // points at a move, which is not a prologue
    image.Word(FarOutsideTheImage);  // not an address in range

    WalkOptions options = Options(image);
    options.pointer_scan_begin = data;
    options.pointer_scan_end = static_cast<std::uint32_t>(Base + image.Bytes().size());

    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, {}, options);

    EXPECT_EQ(result.report.words_in_range, 2U) << "two of the three point into the code";
    EXPECT_EQ(result.report.words_without_a_prologue, 1U);
    ASSERT_EQ(result.pointers.size(), 1U);
    EXPECT_EQ(result.pointers.front().target, Base);
    EXPECT_TRUE(HasFunction(result, Base, arm::InstructionSet::Arm));

    // The walk reached the function the sweep found, and nothing else.
    ASSERT_EQ(result.regions.size(), 1U);
    EXPECT_EQ(result.regions.front().address, Base);
    EXPECT_EQ(result.regions.front().size, 8U);
}

TEST(LooksLikeAnEntryPoint, RecognisesTheShapesAFunctionStartsWith) {
    ImageBuilder image;
    image.Arm(ArmPush);         // 0x00100000
    image.Arm(ArmAdjustStack);  // 0x00100004
    image.Arm(ArmMove);         // 0x00100008
    image.Thumb(ThumbPush);     // 0x0010000C
    image.Thumb(ThumbMove);     // 0x0010000E

    const WalkImage walked{.bytes = image.Bytes(), .base = Base};

    EXPECT_TRUE(LooksLikeAnEntryPoint(walked, Base, arm::InstructionSet::Arm));
    EXPECT_TRUE(LooksLikeAnEntryPoint(walked, SecondWord, arm::InstructionSet::Arm));
    EXPECT_FALSE(LooksLikeAnEntryPoint(walked, ThirdWord, arm::InstructionSet::Arm));
    EXPECT_TRUE(LooksLikeAnEntryPoint(walked, FourthWord, arm::InstructionSet::Thumb));
    EXPECT_FALSE(LooksLikeAnEntryPoint(walked, FourthWord + 2, arm::InstructionSet::Thumb));

    EXPECT_FALSE(LooksLikeAnEntryPoint(walked, Base + 2, arm::InstructionSet::Arm))
        << "an ARM function cannot start at an address that is not a multiple of four";
    EXPECT_FALSE(LooksLikeAnEntryPoint(walked, Base + PastTheEnd, arm::InstructionSet::Arm));
}

TEST(WalkCode, ProducesTablesTheWritersAccept) {
    ImageBuilder image;
    image.Arm(ArmPush);
    image.Arm(ArmLoadLiteral);
    image.Arm(ArmPop);
    image.Word(Base + 16);
    image.Arm(ArmPush);
    image.Arm(ArmPop);

    const std::vector<WalkSeed> seeds = {ArmSeed(Base)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    EXPECT_TRUE(ParseRegions(RenderRegions(result.regions)).Ok());
    EXPECT_TRUE(ParseFunctions(RenderFunctions(result.functions)).Ok());
    EXPECT_TRUE(ParsePointers(RenderPointers(result.pointers)).Ok());
}

}  // namespace
}  // namespace hexlith::domain::analysis
