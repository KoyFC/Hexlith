#include "domain/analysis/function_bounds.h"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::analysis {
namespace {

namespace arm = domain::arm;

constexpr std::uint32_t Base = 0x00100000;

// An address no fixture here covers.
constexpr std::uint32_t PastTheEnd = 0x1000;

constexpr std::uint32_t ArmPush = 0xE92D4010;    // push {r4, lr}
constexpr std::uint32_t ArmPop = 0xE8BD8010;     // pop {r4, pc}
constexpr std::uint32_t ArmMove = 0xE1A01002;    // mov r1, r2
constexpr std::uint32_t ArmReturn = 0xE12FFF1E;  // bx lr

// b with a displacement of zero, which reaches the instruction two on.
constexpr std::uint32_t ArmBranchTwoAhead = 0xEA000000;

// b with a displacement of minus four words, back two instructions.
constexpr std::uint32_t ArmBranchBackTwo = 0xEAFFFFFC;

// beq with a displacement of minus four words, the bottom of a loop.
constexpr std::uint32_t ArmBranchBackThree = 0x0AFFFFFB;

class ImageBuilder {
public:
    void Arm(std::uint32_t word) {
        for (unsigned index = 0; index < 4; ++index) {
            bytes_.push_back(static_cast<std::uint8_t>(word >> (8U * index)));
        }
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

FunctionRange Entry(std::uint32_t address) {
    return FunctionRange{.address = address,
                         .size = UnknownSize,
                         .instruction_set = arm::InstructionSet::Arm,
                         .alternate_entries = {},
                         .source = Provenance::CrsExport};
}

Region CodeRegion(std::uint32_t address, std::uint32_t size) {
    return Region{.address = address,
                  .size = size,
                  .kind = RegionKind::Code,
                  .instruction_set = arm::InstructionSet::Arm,
                  .source = Provenance::Heuristic};
}

const FunctionRange* FunctionAt(const BoundsResult& result, std::uint32_t address) {
    const auto found =
        std::ranges::find_if(result.functions, [address](const FunctionRange& function) {
            return function.address == address;
        });
    return found == result.functions.end() ? nullptr : &*found;
}

// A function that jumps back into code before its own entry and runs through
// another function's entry point there. That entry is not a second way into
// this function, which starts after it, and a table that said so could not be
// read back. Found in a CRO module of the reference title.
TEST(FindFunctionBounds, TakesNoSecondEntryPointFromBeforeTheFunction) {
    ImageBuilder image;
    image.Arm(ArmMove);
    const std::uint32_t earlier = image.AddressOfNext();
    image.Arm(ArmReturn);
    const std::uint32_t later = image.AddressOfNext();
    image.Arm(ArmBranchBackTwo);

    const std::vector<FunctionRange> entries = {Entry(earlier), Entry(later)};
    const WalkImage walk{.bytes = image.Bytes(), .base = Base};
    const std::vector<Region> regions = {
        CodeRegion(Base, static_cast<std::uint32_t>(image.Bytes().size()))};

    const BoundsResult result = FindFunctionBounds(walk, entries, regions);

    const FunctionRange* function = FunctionAt(result, later);
    ASSERT_NE(function, nullptr);
    EXPECT_TRUE(function->alternate_entries.empty());
    EXPECT_NE(FunctionAt(result, earlier), nullptr);
}

TEST(FindFunctionBounds, MeasuresAFunctionToItsReturn) {
    ImageBuilder image;
    image.Arm(ArmPush);
    image.Arm(ArmMove);
    image.Arm(ArmPop);
    image.Arm(ArmMove);  // after the function, and nothing reaches it

    const std::vector<FunctionRange> entries = {Entry(Base)};
    const std::vector<Region> regions = {CodeRegion(Base, 12)};

    const BoundsResult result =
        FindFunctionBounds(WalkImage{.bytes = image.Bytes(), .base = Base}, entries, regions);

    ASSERT_EQ(result.functions.size(), 1U);
    EXPECT_EQ(result.functions.front().size, 12U);
    EXPECT_EQ(result.report.bytes_covered, 12U);
    EXPECT_EQ(result.report.without_an_extent, 0U);
    EXPECT_EQ(result.report.overlapping, 0U);
}

TEST(FindFunctionBounds, FollowsABranchForwardsInsideTheFunction) {
    ImageBuilder image;
    image.Arm(ArmPush);            // 0x00100000
    image.Arm(ArmBranchTwoAhead);  // 0x00100004, skips the next instruction
    image.Arm(ArmMove);            // 0x00100008, jumped over
    image.Arm(ArmPop);             // 0x0010000C, the branch lands here

    const std::vector<FunctionRange> entries = {Entry(Base)};
    const std::vector<Region> regions = {CodeRegion(Base, 16)};

    const BoundsResult result =
        FindFunctionBounds(WalkImage{.bytes = image.Bytes(), .base = Base}, entries, regions);

    ASSERT_EQ(result.functions.size(), 1U);
    EXPECT_EQ(result.functions.front().size, 16U)
        << "the end is the furthest instruction reached, not the first return";
}

TEST(FindFunctionBounds, ABranchBackwardsDoesNotShortenTheFunction) {
    ImageBuilder image;
    image.Arm(ArmPush);             // 0x00100000
    image.Arm(ArmMove);             // 0x00100004
    image.Arm(ArmBranchBackThree);  // 0x00100008, conditionally back to the start
    image.Arm(ArmPop);              // 0x0010000C

    const std::vector<FunctionRange> entries = {Entry(Base)};
    const std::vector<Region> regions = {CodeRegion(Base, 16)};

    const BoundsResult result =
        FindFunctionBounds(WalkImage{.bytes = image.Bytes(), .base = Base}, entries, regions);

    ASSERT_EQ(result.functions.size(), 1U);
    EXPECT_EQ(result.functions.front().size, 16U);
}

TEST(FindFunctionBounds, TwoFunctionsDoNotSwallowEachOther) {
    ImageBuilder image;
    image.Arm(ArmPush);  // 0x00100000, the first function
    image.Arm(ArmPop);
    const std::uint32_t second = image.AddressOfNext();
    image.Arm(ArmPush);  // 0x00100008, the second
    image.Arm(ArmMove);
    image.Arm(ArmPop);

    const std::vector<FunctionRange> entries = {Entry(Base), Entry(second)};
    const std::vector<Region> regions = {CodeRegion(Base, 20)};

    const BoundsResult result =
        FindFunctionBounds(WalkImage{.bytes = image.Bytes(), .base = Base}, entries, regions);

    ASSERT_EQ(result.functions.size(), 2U);
    EXPECT_EQ(result.functions[0].size, 8U);
    EXPECT_EQ(result.functions[1].size, 12U);
    EXPECT_EQ(result.report.alternate_entries, 0U);
    EXPECT_EQ(result.report.overlapping, 0U);
}

TEST(FindFunctionBounds, DoesNotFollowATailCallIntoAnotherFunction) {
    ImageBuilder image;
    image.Arm(ArmPush);            // 0x00100000
    image.Arm(ArmBranchTwoAhead);  // 0x00100004, branches to 0x0010000C
    image.Arm(ArmMove);            // 0x00100008
    const std::uint32_t second = image.AddressOfNext();
    image.Arm(ArmPush);  // 0x0010000C, a function of its own
    image.Arm(ArmPop);

    const std::vector<FunctionRange> entries = {Entry(Base), Entry(second)};
    const std::vector<Region> regions = {CodeRegion(Base, 20)};

    const BoundsResult result =
        FindFunctionBounds(WalkImage{.bytes = image.Bytes(), .base = Base}, entries, regions);

    ASSERT_EQ(result.functions.size(), 2U);

    // Eight bytes and not twelve. The branch is unconditional, so the function
    // ends at it, the instruction after it is unreachable and is claimed by
    // nothing, and the branch target belongs to the other function.
    EXPECT_EQ(result.functions[0].size, 8U);
    EXPECT_EQ(result.functions[1].size, 8U);
    EXPECT_EQ(result.report.alternate_entries, 0U);
}

TEST(FindFunctionBounds, AnEntryInsideAFunctionBecomesASecondWayIn) {
    // Two entry points into one body, which is what a compiler produces when it
    // folds two functions whose endings agree.
    ImageBuilder image;
    image.Arm(ArmPush);  // 0x00100000, the first entry
    image.Arm(ArmMove);
    const std::uint32_t inside = image.AddressOfNext();
    image.Arm(ArmMove);  // 0x00100008, the second entry, reached by falling through
    image.Arm(ArmPop);

    const std::vector<FunctionRange> entries = {Entry(Base), Entry(inside)};
    const std::vector<Region> regions = {CodeRegion(Base, 16)};

    const BoundsResult result =
        FindFunctionBounds(WalkImage{.bytes = image.Bytes(), .base = Base}, entries, regions);

    ASSERT_EQ(result.functions.size(), 1U);
    EXPECT_EQ(result.functions.front().address, Base);
    EXPECT_EQ(result.functions.front().size, 16U);
    EXPECT_EQ(result.functions.front().alternate_entries, std::vector<std::uint32_t>({inside}));
    EXPECT_EQ(result.report.alternate_entries, 1U);
    EXPECT_EQ(result.report.functions, 1U);
    EXPECT_EQ(result.report.entry_points, 2U);
}

TEST(FindFunctionBounds, SaysSoWhenItCannotMeasureAnExtent) {
    ImageBuilder image;
    image.Arm(ArmPush);
    image.Arm(ArmPop);

    // An entry point the classification pass never reached, so no region covers
    // it. Its size stays unknown rather than being guessed from the gap.
    const std::vector<FunctionRange> entries = {Entry(Base), Entry(Base + PastTheEnd)};
    const std::vector<Region> regions = {CodeRegion(Base, 8)};

    const BoundsResult result =
        FindFunctionBounds(WalkImage{.bytes = image.Bytes(), .base = Base}, entries, regions);

    ASSERT_EQ(result.functions.size(), 2U);
    EXPECT_EQ(result.report.without_an_extent, 1U);
    const FunctionRange* unmeasured = FunctionAt(result, Base + PastTheEnd);
    ASSERT_NE(unmeasured, nullptr);
    EXPECT_EQ(unmeasured->size, UnknownSize);
}

TEST(FindFunctionBounds, StopsAtTheEndOfTheRegion) {
    ImageBuilder image;
    image.Arm(ArmPush);
    image.Arm(ArmMove);
    image.Arm(ArmMove);
    image.Arm(ArmMove);

    // The walk reached only the first two words, so the measurement stops there
    // even though the bytes after them decode.
    const std::vector<FunctionRange> entries = {Entry(Base)};
    const std::vector<Region> regions = {CodeRegion(Base, 8)};

    const BoundsResult result =
        FindFunctionBounds(WalkImage{.bytes = image.Bytes(), .base = Base}, entries, regions);

    ASSERT_EQ(result.functions.size(), 1U);
    EXPECT_EQ(result.functions.front().size, 8U);
}

TEST(FindFunctionBounds, TakesTheInstructionSetFromTheRegion) {
    ImageBuilder image;
    image.Arm(ArmReturn);

    // The entry claims Thumb and the region says ARM. The region wins, because it
    // is what the walk established by following a path there.
    std::vector<FunctionRange> entries = {Entry(Base)};
    entries.front().instruction_set = arm::InstructionSet::Thumb;

    const std::vector<Region> regions = {CodeRegion(Base, 4)};

    const BoundsResult result =
        FindFunctionBounds(WalkImage{.bytes = image.Bytes(), .base = Base}, entries, regions);

    ASSERT_EQ(result.functions.size(), 1U);
    EXPECT_EQ(result.functions.front().instruction_set, arm::InstructionSet::Arm);
}

TEST(FindFunctionBounds, ProducesATableTheWriterAccepts) {
    ImageBuilder image;
    image.Arm(ArmPush);
    image.Arm(ArmMove);
    const std::uint32_t inside = image.AddressOfNext();
    image.Arm(ArmMove);
    image.Arm(ArmPop);

    const std::vector<FunctionRange> entries = {Entry(Base), Entry(inside)};
    const std::vector<Region> regions = {CodeRegion(Base, 16)};

    const BoundsResult result =
        FindFunctionBounds(WalkImage{.bytes = image.Bytes(), .base = Base}, entries, regions);

    const core::Result<std::vector<FunctionRange>> read =
        ParseFunctions(RenderFunctions(result.functions));
    ASSERT_TRUE(read.Ok()) << read.Failure().Message();
    ASSERT_EQ(read.Value().size(), 1U);
    EXPECT_EQ(read.Value().front().alternate_entries.size(), 1U);
}

}  // namespace
}  // namespace hexlith::domain::analysis
