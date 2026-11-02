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

// Encodings for the switch tests, checked against an independent disassembler.
constexpr std::uint32_t ArmCompareWithTwo = 0xE3500002;      // cmp r0, #2
constexpr std::uint32_t ArmDispatchLessOrSame = 0x979FF100;  // ldrls pc, [pc, r0, lsl #2]
constexpr std::uint32_t ArmPushPair = 0xE92D0030;            // push {r4, r5}
constexpr std::uint32_t ArmLoadField = 0xE5900008;           // ldr r0, [r0, #8]
constexpr std::uint32_t ArmBranchToThirdWord = 0xEA000000;   // b, two instructions on

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

TEST(WalkCode, WalksAStatedSeedBeforeAGuessAtTheSameAddress) {
    ImageBuilder image;
    image.Arm(ArmPush);  // 0x00100000, decodes in Thumb too, as ands r0, r2
    image.Arm(ArmPop);

    // The guess comes last, which is the order that once let it claim the bytes
    // first and leave the stated seed to stop at a conflict.
    const std::vector<WalkSeed> seeds = {
        ArmSeed(Base),
        WalkSeed{.address = Base,
                 .instruction_set = arm::InstructionSet::Thumb,
                 .source = Provenance::Heuristic},
    };
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    const Region* region = RegionAt(result, Base);
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(region->instruction_set, arm::InstructionSet::Arm);
    EXPECT_EQ(region->size, 8U);
    EXPECT_EQ(result.report.instruction_set_conflicts, std::vector<std::uint32_t>{Base})
        << "the guess is the path that stops";
}

// A function nothing in the image calls or points at, which a run executed.
TEST(WalkCode, WalksWhatARunExecutedWithoutCallingItAFunction) {
    ImageBuilder image;
    image.Arm(ArmPush);  // 0x00100000
    image.Arm(ArmMove);
    image.Arm(ArmPop);

    const std::vector<WalkSeed> seeds = {WalkSeed{.address = Base,
                                                  .instruction_set = arm::InstructionSet::Arm,
                                                  .source = Provenance::Observed}};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    const Region* region = RegionAt(result, Base);
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(region->size, 12U);
    EXPECT_TRUE(result.functions.empty()) << "running an address does not make it an entry";
}

// What a run executed is walked after every guess, so that the sweep of data
// judges its words against what the image alone reached.
TEST(WalkCode, WalksWhatARunExecutedAfterAGuess) {
    ImageBuilder image;
    image.Arm(ArmPush);  // 0x00100000, decodes in Thumb too
    image.Arm(ArmPop);

    const std::vector<WalkSeed> seeds = {
        WalkSeed{.address = Base,
                 .instruction_set = arm::InstructionSet::Arm,
                 .source = Provenance::Observed},
        WalkSeed{.address = Base,
                 .instruction_set = arm::InstructionSet::Thumb,
                 .source = Provenance::Heuristic},
    };
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    const Region* region = RegionAt(result, Base);
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(region->instruction_set, arm::InstructionSet::Thumb);
    EXPECT_EQ(result.report.instruction_set_conflicts, std::vector<std::uint32_t>{Base})
        << "the conflict is reported, and the run says which side is right";
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

// Where the pieces of the switch below land, as offsets from the start of the
// image: the table, the first and second cases after it.
constexpr std::uint32_t SwitchTable = Base + 16;
constexpr std::uint32_t FirstCase = Base + 28;
constexpr std::uint32_t SecondCase = Base + 32;
constexpr std::uint32_t CaseBytes = 12;
constexpr std::uint32_t TableBytes = 12;

// The leaves of the table-of-functions fixtures, and where the table of
// pointers to them begins.
constexpr std::uint32_t FirstLeaf = Base + 8;
constexpr std::uint32_t SecondLeaf = Base + 16;
constexpr std::uint32_t ThirdLeaf = Base + 24;
constexpr std::size_t LeafTableOffset = 32;

// cmp, the dispatch, the word the default case branches from, three entries,
// and the cases right after them, as a compiler lays a switch out.
ImageBuilder SwitchImage(std::uint32_t dispatch) {
    ImageBuilder image;
    image.Arm(ArmPush);            // 0x00100000
    image.Arm(ArmCompareWithTwo);  // 0x00100004
    image.Arm(dispatch);           // 0x00100008, the table starts at 0x00100010
    image.Arm(ArmPop);             // 0x0010000C, the default case
    image.Word(FirstCase);         // 0x00100010, case 0
    image.Word(SecondCase);        // 0x00100014, case 1
    image.Word(FirstCase);         // 0x00100018, case 2 shares case 0
    image.Arm(ArmPop);             // 0x0010001C
    image.Arm(ArmMove);            // 0x00100020
    image.Arm(ArmReturn);          // 0x00100024
    return image;
}

TEST(WalkCode, ReadsASwitchTableAndFollowsEachCase) {
    const ImageBuilder image = SwitchImage(ArmDispatchLessOrSame);
    const std::vector<WalkSeed> seeds = {ArmSeed(Base)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    EXPECT_EQ(result.report.jump_tables, 1U);
    EXPECT_EQ(result.report.jump_table_entries, 3U);

    const Region* table = RegionAt(result, SwitchTable);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->kind, RegionKind::JumpTable);
    EXPECT_EQ(table->size, TableBytes);

    const Region* cases = RegionAt(result, FirstCase);
    ASSERT_NE(cases, nullptr) << "the cases are code the instruction stream never reaches";
    EXPECT_EQ(cases->kind, RegionKind::Code);
    EXPECT_EQ(cases->size, CaseBytes);
}

TEST(WalkCode, DoesNotMakeTheCasesOfASwitchIntoFunctions) {
    const ImageBuilder image = SwitchImage(ArmDispatchLessOrSame);
    const std::vector<WalkSeed> seeds = {ArmSeed(Base)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));

    EXPECT_FALSE(HasFunction(result, FirstCase, arm::InstructionSet::Arm));
    EXPECT_FALSE(HasFunction(result, SecondCase, arm::InstructionSet::Arm));
}

TEST(WalkCode, LeavesWordsThatAreNotATableAsCode) {
    // The same bytes behind a load that is not a table dispatch.
    ImageBuilder image;
    image.Arm(ArmPush);
    image.Arm(ArmCompareWithTwo);
    image.Arm(ArmNop);
    image.Arm(ArmPop);
    image.Word(SixthWord);
    image.Arm(ArmPop);
    const std::vector<WalkSeed> seeds = {ArmSeed(Base)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));
    EXPECT_EQ(result.report.jump_tables, 0U);
    EXPECT_EQ(result.report.jump_tables_rejected, 0U);
}

TEST(WalkCode, RejectsALoadThatLooksLikeADispatchWithNoTableBehindIt) {
    ImageBuilder image;
    image.Arm(ArmPush);
    image.Arm(ArmCompareWithTwo);
    image.Arm(ArmDispatchLessOrSame);
    image.Arm(ArmPop);
    image.Word(NotAnAddress);
    image.Word(NotAnAddress);
    image.Word(NotAnAddress);
    const std::vector<WalkSeed> seeds = {ArmSeed(Base)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));
    EXPECT_EQ(result.report.jump_tables, 0U);
    EXPECT_EQ(result.report.jump_tables_rejected, 1U);
    EXPECT_EQ(RegionAt(result, SwitchTable), nullptr) << "the words are left unclaimed";
}

TEST(WalkCode, TakesAnUnconditionalBranchToAPushForAFunction) {
    ImageBuilder image;
    image.Arm(ArmBranchToThirdWord);  // 0x00100000
    image.Arm(ArmNop);                // 0x00100004
    image.Arm(ArmPushPair);           // 0x00100008
    image.Arm(ArmReturn);             // 0x0010000C
    const std::vector<WalkSeed> seeds = {ArmSeed(Base)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));
    EXPECT_TRUE(HasFunction(result, ThirdWord, arm::InstructionSet::Arm));
}

TEST(WalkCode, LeavesABranchToAnythingElseAsAJump) {
    ImageBuilder image;
    image.Arm(ArmBranchToThirdWord);  // 0x00100000
    image.Arm(ArmNop);                // 0x00100004
    image.Arm(ArmMove);               // 0x00100008, not a prologue
    image.Arm(ArmReturn);             // 0x0010000C
    const std::vector<WalkSeed> seeds = {ArmSeed(Base)};
    const WalkResult result =
        WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base}, seeds, Options(image));
    EXPECT_FALSE(HasFunction(result, ThirdWord, arm::InstructionSet::Arm));
}

// A function nothing walks to, then a table of three pointers to leaf functions
// with no prologue among them.
ImageBuilder LeavesAndTable(std::uint32_t entry_flag) {
    ImageBuilder image;
    image.Arm(ArmPush);  // 0x00100000
    image.Arm(ArmPop);   // 0x00100004
    for (unsigned leaf = 0; leaf < 3; ++leaf) {
        image.Arm(ArmLoadField);  // a leaf: no prologue
        image.Arm(ArmReturn);
    }
    image.Word(FirstLeaf | entry_flag);
    image.Word(SecondLeaf | entry_flag);
    image.Word(ThirdLeaf | entry_flag);
    return image;
}

WalkOptions SweepOptions(const ImageBuilder& image, std::size_t table_start) {
    WalkOptions options = Options(image);
    options.pointer_scan_begin = static_cast<std::uint32_t>(Base + table_start);
    options.pointer_scan_end = static_cast<std::uint32_t>(Base + image.Bytes().size());
    return options;
}

TEST(WalkCode, TakesEveryPointerInARunOfCodeAddressesEvenWithoutAPrologue) {
    const ImageBuilder image = LeavesAndTable(0);
    const WalkResult result = WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base},
                                       std::vector<WalkSeed>{ArmSeed(Base)},
                                       SweepOptions(image, LeafTableOffset));
    EXPECT_EQ(result.report.words_taken_from_tables, 3U);
    EXPECT_TRUE(HasFunction(result, FirstLeaf, arm::InstructionSet::Arm));
    EXPECT_TRUE(HasFunction(result, SecondLeaf, arm::InstructionSet::Arm));
    EXPECT_TRUE(HasFunction(result, ThirdLeaf, arm::InstructionSet::Arm));
}

TEST(WalkCode, WantsThreeConsecutiveAddressesBeforeTrustingARun) {
    const ImageBuilder image = LeavesAndTable(0);
    WalkOptions options = SweepOptions(image, LeafTableOffset);
    options.minimum_pointer_run = 4;
    const WalkResult result = WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base},
                                       std::vector<WalkSeed>{ArmSeed(Base)},
                                       options);
    EXPECT_EQ(result.report.words_taken_from_tables, 0U);
    EXPECT_FALSE(HasFunction(result, FirstLeaf, arm::InstructionSet::Arm));
}

TEST(WalkCode, IgnoresARunThatPointsIntoCodeAlreadyWalked) {
    // A seed function of moves, and a table whose entries point at the middle
    // of it: where a case label points, not a function.
    ImageBuilder image;
    image.Arm(ArmMove);    // 0x00100000
    image.Arm(ArmMove);    // 0x00100004
    image.Arm(ArmMove);    // 0x00100008
    image.Arm(ArmReturn);  // 0x0010000C
    image.Word(Base + 4);
    image.Word(Base + 8);
    image.Word(Base + 4);
    const WalkResult result = WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base},
                                       std::vector<WalkSeed>{ArmSeed(Base)},
                                       SweepOptions(image, 16));
    EXPECT_EQ(result.report.words_into_walked_code, 3U);
    EXPECT_FALSE(HasFunction(result, Base + 4, arm::InstructionSet::Arm));
}

TEST(WalkCode, IgnoresAThumbAddressInARunWithoutAPrologue) {
    const ImageBuilder image = LeavesAndTable(1);
    const WalkResult result = WalkCode(WalkImage{.bytes = image.Bytes(), .base = Base},
                                       std::vector<WalkSeed>{ArmSeed(Base)},
                                       SweepOptions(image, LeafTableOffset));
    EXPECT_EQ(result.report.words_taken_from_tables, 0U);
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
