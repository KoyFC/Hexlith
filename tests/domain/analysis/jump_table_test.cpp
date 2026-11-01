#include "domain/analysis/jump_table.h"
#include "domain/arm/decode_arm.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

namespace hexlith::domain::analysis {
namespace {

namespace arm = domain::arm;

constexpr std::uint32_t Base = 0x00100000;

// Encodings checked against an independent disassembler.
constexpr std::uint32_t CompareWithTwo = 0xE3500002;      // cmp r0, #2
constexpr std::uint32_t DispatchLessOrSame = 0x979FF100;  // ldrls pc, [pc, r0, lsl #2]
constexpr std::uint32_t DispatchLower = 0x379FF100;       // ldrlo pc, [pc, r0, lsl #2]
constexpr std::uint32_t DispatchAlways = 0xE79FF100;      // ldr pc, [pc, r0, lsl #2]
constexpr std::uint32_t ShiftedByOne = 0xE79FF080;        // ldr pc, [pc, r0, lsl #1]
constexpr std::uint32_t LiteralLoad = 0xE59FF000;         // ldr pc, [pc]
constexpr std::uint32_t WritesBack = 0xE7BFF100;          // ldr pc, [pc, r0, lsl #2]!
constexpr std::uint32_t Default = 0xEA000010;             // b somewhere
constexpr std::uint32_t Nop = 0xE320F000;

constexpr std::uint32_t NotAnAddress = 0x12345678;

class ImageBuilder {
public:
    void Word(std::uint32_t word) {
        for (unsigned index = 0; index < 4; ++index) {
            bytes_.push_back(static_cast<std::uint8_t>(word >> (8U * index)));
        }
    }

    [[nodiscard]] WalkImage Image() const {
        return WalkImage{.bytes = bytes_, .base = Base};
    }

    [[nodiscard]] std::uint32_t End() const {
        return Base + static_cast<std::uint32_t>(bytes_.size());
    }

private:
    std::vector<std::uint8_t> bytes_;
};

// A dispatch at Base + 4 behind a compare, with the entries that follow.
ImageBuilder Switch(std::uint32_t dispatch, const std::vector<std::uint32_t>& entries) {
    ImageBuilder image;
    image.Word(CompareWithTwo);  // Base
    image.Word(dispatch);        // Base + 4
    image.Word(Default);         // Base + 8, never read as an entry
    for (const std::uint32_t entry : entries) {
        image.Word(entry);  // Base + 12 onwards
    }
    return image;
}

std::optional<JumpTable> Read(const ImageBuilder& image) {
    const std::uint32_t at = Base + 4;
    const arm::Instruction dispatch =
        arm::DecodeArm(core::ReadU32At(image.Image().bytes, 4).ValueOr(0));
    return ReadJumpTable(image.Image(), at, dispatch, Base, image.End());
}

TEST(IsTableDispatch, RecognisesALoadIntoThePcThroughAShiftedIndex) {
    EXPECT_TRUE(IsTableDispatch(arm::DecodeArm(DispatchLessOrSame)));
    EXPECT_TRUE(IsTableDispatch(arm::DecodeArm(DispatchAlways)));
}

TEST(IsTableDispatch, RefusesOtherLoadsIntoThePc) {
    EXPECT_FALSE(IsTableDispatch(arm::DecodeArm(ShiftedByOne))) << "entries are four bytes";
    EXPECT_FALSE(IsTableDispatch(arm::DecodeArm(LiteralLoad)));
    EXPECT_FALSE(IsTableDispatch(arm::DecodeArm(WritesBack)));
    EXPECT_FALSE(IsTableDispatch(arm::DecodeArm(Nop)));
}

TEST(ReadJumpTable, TakesTheEntryCountFromACompareAndLowerOrSame) {
    // cmp r0, #2 ; ldrls: the index runs 0, 1 and 2.
    const ImageBuilder image = Switch(DispatchLessOrSame, {Base, Base + 4, Base + 8, NotAnAddress});
    const std::optional<JumpTable> read = Read(image);
    ASSERT_TRUE(read.has_value());
    const JumpTable table = read.value_or(JumpTable{});
    EXPECT_EQ(table.address, Base + 12);
    EXPECT_EQ(table.targets, (std::vector<std::uint32_t>{Base, Base + 4, Base + 8}));
}

TEST(ReadJumpTable, TakesTheEntryCountFromACompareAndLower) {
    // cmp r0, #2 ; ldrlo: the index runs 0 and 1.
    const ImageBuilder image = Switch(DispatchLower, {Base, Base + 4, Base + 8});
    const std::optional<JumpTable> read = Read(image);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read.value_or(JumpTable{}).targets.size(), 2U);
}

TEST(ReadJumpTable, RefusesATableThatStopsShortOfItsBound) {
    const ImageBuilder image = Switch(DispatchLessOrSame, {Base, NotAnAddress, Base + 8});
    EXPECT_FALSE(Read(image).has_value())
        << "the compare promised three entries, and one is not an address";
}

TEST(ReadJumpTable, RefusesAnEntryThatIsNotWordAligned) {
    const ImageBuilder image = Switch(DispatchLower, {Base, Base + 6});
    EXPECT_FALSE(Read(image).has_value());
}

TEST(ReadJumpTable, ReadsAnUnboundedTableAsFarAsTheWordsAreAddresses) {
    ImageBuilder image;
    image.Word(Nop);
    image.Word(DispatchAlways);  // Base + 4
    image.Word(Default);
    image.Word(Base);
    image.Word(Base + 4);
    image.Word(NotAnAddress);
    const arm::Instruction dispatch = arm::DecodeArm(DispatchAlways);
    const std::optional<JumpTable> read =
        ReadJumpTable(image.Image(), Base + 4, dispatch, Base, image.End());
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read.value_or(JumpTable{}).targets.size(), 2U);
}

TEST(ReadJumpTable, WantsMoreThanOneEntryWhenNothingBoundsTheTable) {
    ImageBuilder image;
    image.Word(Nop);
    image.Word(DispatchAlways);
    image.Word(Default);
    image.Word(Base);
    image.Word(NotAnAddress);
    const arm::Instruction dispatch = arm::DecodeArm(DispatchAlways);
    EXPECT_FALSE(ReadJumpTable(image.Image(), Base + 4, dispatch, Base, image.End()).has_value());
}

TEST(ReadJumpTable, SaysNothingForAnInstructionThatIsNotADispatch) {
    ImageBuilder image;
    image.Word(Nop);
    image.Word(Nop);
    image.Word(Nop);
    const arm::Instruction nop = arm::DecodeArm(Nop);
    EXPECT_FALSE(ReadJumpTable(image.Image(), Base + 4, nop, Base, image.End()).has_value());
}

}  // namespace
}  // namespace hexlith::domain::analysis
