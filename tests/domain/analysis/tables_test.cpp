#include "domain/analysis/record_text.h"
#include "domain/analysis/tables.h"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

namespace hexlith::domain::analysis {
namespace {

// The records of a rendered table, with the banner and the format line
// dropped, so that a test can say what the file holds without repeating the
// banner it does not care about.
std::vector<std::string> RecordLines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t position = 0;

    while (position < text.size()) {
        const std::size_t break_at = text.find('\n', position);
        const std::string_view line = text.substr(position, break_at - position);
        position = break_at + 1;
        if (!line.empty() && line.front() != '#' && !line.starts_with("format ")) {
            lines.emplace_back(line);
        }
    }

    return lines;
}

constexpr std::uint32_t FirstAddress = 0x100;
constexpr std::uint32_t SecondAddress = 0x200;
constexpr std::uint32_t ThirdAddress = 0x300;

Symbol SymbolAt(std::uint32_t address, std::string name) {
    return Symbol{.address = address,
                  .size = 4,
                  .kind = SymbolKind::Function,
                  .name = std::move(name),
                  .source = Provenance::CrsExport};
}

TEST(SymbolKind, RoundTripsThroughItsSpelling) {
    for (const SymbolKind kind : {SymbolKind::Function, SymbolKind::Data, SymbolKind::Unknown}) {
        EXPECT_EQ(ParseSymbolKind(Describe(kind)), kind);
    }
    EXPECT_FALSE(ParseSymbolKind("function").has_value());
}

TEST(RegionKind, RoundTripsThroughItsSpelling) {
    for (const RegionKind kind :
         {RegionKind::Code, RegionKind::Data, RegionKind::JumpTable, RegionKind::Padding}) {
        EXPECT_EQ(ParseRegionKind(Describe(kind)), kind);
    }
    EXPECT_FALSE(ParseRegionKind("jumptable").has_value());
}

TEST(Symbols, RenderMatchesTheFormatOfTheDecision) {
    const std::vector<std::string> lines = RecordLines(RenderSymbols({
        Symbol{.address = 0x0012A4B0,
               .size = 0xC4,
               .kind = SymbolKind::Function,
               .name = "FieldMain",
               .source = Provenance::CrsExport},
    }));

    ASSERT_EQ(lines.size(), 1U);
    EXPECT_EQ(lines.front(),
              "0x0012A4B0 size=0x000000C4 kind=func name=FieldMain source=crs-export");
}

TEST(Symbols, RenderSortsByAddressAndThenByName) {
    const std::vector<std::string> lines = RecordLines(RenderSymbols({
        SymbolAt(SecondAddress, "zeta"),
        SymbolAt(FirstAddress, "beta"),
        SymbolAt(FirstAddress, "alpha"),
    }));

    ASSERT_EQ(lines.size(), 3U);
    EXPECT_NE(lines[0].find("name=alpha"), std::string::npos);
    EXPECT_NE(lines[1].find("name=beta"), std::string::npos);
    EXPECT_NE(lines[2].find("name=zeta"), std::string::npos);
}

TEST(Symbols, RenderDoesNotDependOnTheOrderItWasGiven) {
    const std::vector<Symbol> records = {SymbolAt(FirstAddress, "a"),
                                         SymbolAt(FirstAddress, "a2"),
                                         SymbolAt(SecondAddress, "b"),
                                         SymbolAt(ThirdAddress, "c")};
    const std::string once = RenderSymbols(records);

    std::vector<Symbol> reversed = records;
    std::ranges::reverse(reversed);
    EXPECT_EQ(RenderSymbols(reversed), once);

    std::vector<Symbol> rotated = records;
    std::ranges::rotate(rotated, rotated.begin() + 2);
    EXPECT_EQ(RenderSymbols(rotated), once);
}

TEST(Symbols, ARenderedTableReadsBackAsWhatWentIn) {
    const std::vector<Symbol> records = {
        Symbol{.address = 0x100,
               .size = UnknownSize,
               .kind = SymbolKind::Unknown,
               .name = "unnamed_100",
               .source = Provenance::CrsExport},
        Symbol{.address = 0x200,
               .size = 0x10,
               .kind = SymbolKind::Data,
               .name = "table",
               .source = Provenance::Manual},
    };

    const core::Result<std::vector<Symbol>> read = ParseSymbols(RenderSymbols(records));
    ASSERT_TRUE(read.Ok()) << read.Failure().Message();
    ASSERT_EQ(read.Value().size(), 2U);
    EXPECT_EQ(read.Value()[0].address, 0x100U);
    EXPECT_EQ(read.Value()[0].size, UnknownSize);
    EXPECT_EQ(read.Value()[0].kind, SymbolKind::Unknown);
    EXPECT_EQ(read.Value()[0].name, "unnamed_100");
    EXPECT_EQ(read.Value()[0].source, Provenance::CrsExport);
    EXPECT_EQ(read.Value()[1].kind, SymbolKind::Data);
    EXPECT_EQ(read.Value()[1].source, Provenance::Manual);
}

TEST(Symbols, RenderingTwiceIsByteForByteTheSame) {
    const std::vector<Symbol> records = {SymbolAt(FirstAddress, "a"), SymbolAt(SecondAddress, "b")};
    const std::string once = RenderSymbols(records);
    const core::Result<std::vector<Symbol>> read = ParseSymbols(once);
    ASSERT_TRUE(read.Ok()) << read.Failure().Message();
    EXPECT_EQ(RenderSymbols(read.Value()), once);
}

TEST(Symbols, RefusesARecordMissingAField) {
    const std::string prefix = RenderVersionLine();
    EXPECT_FALSE(ParseSymbols(prefix + "0x100 kind=func name=a source=manual\n").Ok());
    EXPECT_FALSE(ParseSymbols(prefix + "0x100 size=0x4 name=a source=manual\n").Ok());
    EXPECT_FALSE(ParseSymbols(prefix + "0x100 size=0x4 kind=func source=manual\n").Ok());
    EXPECT_FALSE(ParseSymbols(prefix + "0x100 size=0x4 kind=func name=a\n").Ok());
}

TEST(Symbols, RefusesAKindOrSourceItDoesNotKnow) {
    const std::string prefix = RenderVersionLine();
    const core::Result<std::vector<Symbol>> kind =
        ParseSymbols(prefix + "0x100 size=0x4 kind=maybe name=a source=manual\n");
    ASSERT_FALSE(kind.Ok());
    EXPECT_NE(kind.Failure().Message().find("not a symbol kind"), std::string::npos);

    const core::Result<std::vector<Symbol>> source =
        ParseSymbols(prefix + "0x100 size=0x4 kind=func name=a source=vibes\n");
    ASSERT_FALSE(source.Ok());
    EXPECT_NE(source.Failure().Message().find("not a source"), std::string::npos);
}

TEST(Functions, RenderWritesTheInstructionSetAndTheAlternateEntries) {
    const std::vector<std::string> lines = RecordLines(RenderFunctions({
        FunctionRange{.address = 0x100,
                      .size = 0x40,
                      .instruction_set = arm::InstructionSet::Thumb,
                      .alternate_entries = {0x120, 0x108},
                      .source = Provenance::Heuristic},
    }));

    ASSERT_EQ(lines.size(), 1U);
    EXPECT_EQ(lines.front(),
              "0x00000100 size=0x00000040 isa=thumb alt-entries=0x00000108,0x00000120 "
              "source=heuristic");
}

TEST(Functions, LeavesTheAlternateEntryFieldOutWhenThereAreNone) {
    const std::vector<std::string> lines = RecordLines(RenderFunctions({
        FunctionRange{.address = 0x100,
                      .size = 0x40,
                      .alternate_entries = {},
                      .source = Provenance::ExHeader},
    }));

    ASSERT_EQ(lines.size(), 1U);
    EXPECT_EQ(lines.front(), "0x00000100 size=0x00000040 isa=arm source=exheader");
}

TEST(Functions, ARenderedTableReadsBackAsWhatWentIn) {
    const std::vector<FunctionRange> records = {
        FunctionRange{.address = 0x100,
                      .size = 0x40,
                      .instruction_set = arm::InstructionSet::Thumb,
                      .alternate_entries = {0x108, 0x120},
                      .source = Provenance::Heuristic},
        FunctionRange{.address = 0x200,
                      .size = UnknownSize,
                      .alternate_entries = {},
                      .source = Provenance::Manual},
    };

    const core::Result<std::vector<FunctionRange>> read = ParseFunctions(RenderFunctions(records));
    ASSERT_TRUE(read.Ok()) << read.Failure().Message();
    ASSERT_EQ(read.Value().size(), 2U);
    EXPECT_EQ(read.Value()[0].instruction_set, arm::InstructionSet::Thumb);
    EXPECT_EQ(read.Value()[0].alternate_entries, std::vector<std::uint32_t>({0x108, 0x120}));
    EXPECT_TRUE(read.Value()[1].alternate_entries.empty());
    EXPECT_EQ(read.Value()[1].instruction_set, arm::InstructionSet::Arm);
}

TEST(Functions, RefusesAnAlternateEntryOutsideTheFunction) {
    const std::string prefix = RenderVersionLine();
    const core::Result<std::vector<FunctionRange>> outside =
        ParseFunctions(prefix + "0x100 size=0x40 isa=arm alt-entries=0x200 source=manual\n");
    ASSERT_FALSE(outside.Ok());
    EXPECT_NE(outside.Failure().Message().find("outside the function"), std::string::npos);

    const core::Result<std::vector<FunctionRange>> itself =
        ParseFunctions(prefix + "0x100 size=0x40 isa=arm alt-entries=0x100 source=manual\n");
    ASSERT_FALSE(itself.Ok());
    EXPECT_NE(itself.Failure().Message().find("own entry point"), std::string::npos);
}

TEST(Functions, AcceptsAnAlternateEntryWhenTheExtentIsNotKnownYet) {
    const core::Result<std::vector<FunctionRange>> read = ParseFunctions(
        RenderVersionLine() + "0x100 size=0x0 isa=arm alt-entries=0x200 source=manual\n");
    ASSERT_TRUE(read.Ok()) << read.Failure().Message();
    EXPECT_EQ(read.Value().front().alternate_entries.front(), 0x200U);
}

TEST(Functions, RefusesTheSameAlternateEntryTwice) {
    const core::Result<std::vector<FunctionRange>> read = ParseFunctions(
        RenderVersionLine() + "0x100 size=0x40 isa=arm alt-entries=0x108,0x108 source=manual\n");
    ASSERT_FALSE(read.Ok());
    EXPECT_NE(read.Failure().Message().find("twice"), std::string::npos);
}

TEST(Functions, RefusesAnInstructionSetItDoesNotKnow) {
    const core::Result<std::vector<FunctionRange>> read =
        ParseFunctions(RenderVersionLine() + "0x100 size=0x40 isa=jazelle source=manual\n");
    ASSERT_FALSE(read.Ok());
    EXPECT_NE(read.Failure().Message().find("arm or thumb"), std::string::npos);
}

TEST(Regions, CodeCarriesAnInstructionSetAndTheOthersDoNot) {
    const std::vector<std::string> lines = RecordLines(RenderRegions({
        Region{.address = 0x100,
               .size = 0x40,
               .kind = RegionKind::Code,
               .instruction_set = arm::InstructionSet::Arm,
               .source = Provenance::Heuristic},
        Region{.address = 0x140,
               .size = 0x10,
               .kind = RegionKind::JumpTable,
               .instruction_set = std::nullopt,
               .source = Provenance::Manual},
    }));

    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines[0], "0x00000100 size=0x00000040 kind=code isa=arm source=heuristic");
    EXPECT_EQ(lines[1], "0x00000140 size=0x00000010 kind=jump-table source=manual");
}

TEST(Regions, RefusesCodeWithoutAnInstructionSet) {
    const core::Result<std::vector<Region>> read =
        ParseRegions(RenderVersionLine() + "0x100 size=0x40 kind=code source=manual\n");
    ASSERT_FALSE(read.Ok());
    EXPECT_NE(read.Failure().Message().find("which instruction set"), std::string::npos);
}

TEST(Regions, RefusesAnInstructionSetOnSomethingThatIsNotCode) {
    const core::Result<std::vector<Region>> read =
        ParseRegions(RenderVersionLine() + "0x100 size=0x40 kind=data isa=arm source=manual\n");
    ASSERT_FALSE(read.Ok());
    EXPECT_NE(read.Failure().Message().find("has no instruction set"), std::string::npos);
}

TEST(Regions, ARenderedTableReadsBackAsWhatWentIn) {
    const std::vector<Region> records = {
        Region{.address = 0x100,
               .size = 0x40,
               .kind = RegionKind::Code,
               .instruction_set = arm::InstructionSet::Thumb,
               .source = Provenance::Heuristic},
        Region{.address = 0x140,
               .size = 4,
               .kind = RegionKind::Padding,
               .instruction_set = std::nullopt,
               .source = Provenance::Heuristic},
    };

    const core::Result<std::vector<Region>> read = ParseRegions(RenderRegions(records));
    ASSERT_TRUE(read.Ok()) << read.Failure().Message();
    ASSERT_EQ(read.Value().size(), 2U);
    EXPECT_EQ(read.Value()[0].instruction_set, arm::InstructionSet::Thumb);
    EXPECT_FALSE(read.Value()[1].instruction_set.has_value());
    EXPECT_EQ(read.Value()[1].kind, RegionKind::Padding);
}

TEST(Pointers, RenderAndReadBack) {
    const std::vector<CodePointer> records = {
        CodePointer{.address = 0x53C120, .target = 0x123456, .source = Provenance::Heuristic},
        CodePointer{.address = 0x53C100, .target = 0x100000, .source = Provenance::Manual},
    };

    const std::string text = RenderPointers(records);
    const std::vector<std::string> lines = RecordLines(text);
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines[0], "0x0053C100 target=0x00100000 source=manual");
    EXPECT_EQ(lines[1], "0x0053C120 target=0x00123456 source=heuristic");

    const core::Result<std::vector<CodePointer>> read = ParsePointers(text);
    ASSERT_TRUE(read.Ok()) << read.Failure().Message();
    ASSERT_EQ(read.Value().size(), 2U);
    EXPECT_EQ(read.Value()[0].address, 0x53C100U);
    EXPECT_EQ(read.Value()[0].target, 0x100000U);
}

TEST(Pointers, RefusesATargetThatIsNotAnAddress) {
    EXPECT_FALSE(ParsePointers(RenderVersionLine() + "0x100 target=later source=manual\n").Ok());
    EXPECT_FALSE(ParsePointers(RenderVersionLine() + "0x100 source=manual\n").Ok());
}

TEST(EveryTable, WritesABannerAndTheFormatVersion) {
    for (const std::string& text :
         {RenderSymbols({}), RenderFunctions({}), RenderRegions({}), RenderPointers({})}) {
        EXPECT_TRUE(text.starts_with("# ")) << text;
        EXPECT_NE(text.find(RenderVersionLine()), std::string::npos) << text;
        EXPECT_TRUE(RecordLines(text).empty()) << text;
        EXPECT_EQ(text.find(" \n"), std::string::npos) << "a line ends in whitespace";
    }
}

TEST(EveryTable, ReadsBackTheEmptyFileItWrites) {
    EXPECT_TRUE(ParseSymbols(RenderSymbols({})).Ok());
    EXPECT_TRUE(ParseFunctions(RenderFunctions({})).Ok());
    EXPECT_TRUE(ParseRegions(RenderRegions({})).Ok());
    EXPECT_TRUE(ParsePointers(RenderPointers({})).Ok());
}

}  // namespace
}  // namespace hexlith::domain::analysis
