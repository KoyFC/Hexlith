#include "domain/analysis/merge.h"
#include "domain/analysis/tables.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::domain::analysis {
namespace {

constexpr std::uint32_t Named = 0x00100000;
constexpr std::uint32_t Renamed = 0x00100100;
constexpr std::uint32_t Fresh = 0x00100200;

Symbol SymbolAt(std::uint32_t address, std::string name, Provenance source) {
    return Symbol{.address = address,
                  .size = UnknownSize,
                  .kind = SymbolKind::Unknown,
                  .name = std::move(name),
                  .source = source};
}

std::vector<std::string> NamesOf(const std::vector<Symbol>& records) {
    std::vector<std::string> names;
    names.reserve(records.size());
    for (const Symbol& record : records) {
        names.push_back(record.name);
    }
    return names;
}

TEST(MergeRecords, KeepsWhatNothingCanDeriveAgain) {
    const std::vector<Symbol> existing = {SymbolAt(Renamed, "FieldMain", Provenance::Manual)};
    const std::vector<Symbol> derived = {SymbolAt(Fresh, "sub_100200", Provenance::Heuristic)};

    MergeReport report;
    const std::vector<Symbol> merged = MergeRecords(existing, derived, report);

    EXPECT_EQ(NamesOf(merged), std::vector<std::string>({"FieldMain", "sub_100200"}));
    EXPECT_EQ(report.preserved, 1U);
    EXPECT_EQ(report.discarded, 0U);
    EXPECT_EQ(report.taken, 1U);
    EXPECT_EQ(report.suppressed, 0U);
}

TEST(MergeRecords, ReplacesWhatThePassDerivesAgain) {
    const std::vector<Symbol> existing = {SymbolAt(Named, "old_name", Provenance::CrsExport),
                                          SymbolAt(Fresh, "guessed", Provenance::Heuristic)};
    const std::vector<Symbol> derived = {SymbolAt(Named, "new_name", Provenance::CrsExport)};

    MergeReport report;
    const std::vector<Symbol> merged = MergeRecords(existing, derived, report);

    EXPECT_EQ(NamesOf(merged), std::vector<std::string>({"new_name"}));
    EXPECT_EQ(report.preserved, 0U);
    EXPECT_EQ(report.discarded, 2U);
    EXPECT_EQ(report.taken, 1U);
}

TEST(MergeRecords, DropsAPassResultForAnAddressAPersonHasDecided) {
    const std::vector<Symbol> existing = {SymbolAt(Renamed, "FieldMain", Provenance::Manual)};
    const std::vector<Symbol> derived = {SymbolAt(Renamed, "sub_100100", Provenance::Heuristic),
                                         SymbolAt(Fresh, "sub_100200", Provenance::Heuristic)};

    MergeReport report;
    const std::vector<Symbol> merged = MergeRecords(existing, derived, report);

    EXPECT_EQ(NamesOf(merged), std::vector<std::string>({"FieldMain", "sub_100200"}));
    EXPECT_EQ(report.suppressed, 1U);
    EXPECT_EQ(report.taken, 1U);
}

TEST(MergeRecords, TreatsADecompilerImportTheSameWayAsAPerson) {
    const std::vector<Symbol> existing = {SymbolAt(Renamed, "FUN_00100100", Provenance::Ghidra)};
    const std::vector<Symbol> derived = {SymbolAt(Renamed, "sub_100100", Provenance::Heuristic)};

    MergeReport report;
    const std::vector<Symbol> merged = MergeRecords(existing, derived, report);

    EXPECT_EQ(NamesOf(merged), std::vector<std::string>({"FUN_00100100"}));
    EXPECT_EQ(report.suppressed, 1U);
}

TEST(MergeRecords, SuppressesEverySymbolAPassProducesForADecidedAddress) {
    const std::vector<Symbol> existing = {SymbolAt(Renamed, "FieldMain", Provenance::Manual),
                                          SymbolAt(Renamed, "FieldMainAlias", Provenance::Manual)};
    const std::vector<Symbol> derived = {SymbolAt(Renamed, "a", Provenance::Heuristic),
                                         SymbolAt(Renamed, "b", Provenance::Heuristic)};

    MergeReport report;
    const std::vector<Symbol> merged = MergeRecords(existing, derived, report);

    EXPECT_EQ(merged.size(), 2U);
    EXPECT_EQ(report.preserved, 2U);
    EXPECT_EQ(report.suppressed, 2U);
}

TEST(MergeRecords, AnEmptyPassLeavesOnlyWhatWasPreserved) {
    const std::vector<Symbol> existing = {SymbolAt(Named, "derived", Provenance::ExHeader),
                                          SymbolAt(Renamed, "kept", Provenance::Manual)};

    MergeReport report;
    const std::vector<Symbol> merged = MergeRecords(existing, {}, report);

    EXPECT_EQ(NamesOf(merged), std::vector<std::string>({"kept"}));
    EXPECT_EQ(report.discarded, 1U);
    EXPECT_EQ(report.taken, 0U);
}

TEST(MergeRecords, AFirstRunTakesEverythingThePassFound) {
    const std::vector<Symbol> derived = {SymbolAt(Named, "a", Provenance::CrsExport),
                                         SymbolAt(Fresh, "b", Provenance::CrsExport)};

    MergeReport report;
    const std::vector<Symbol> merged = MergeRecords({}, derived, report);

    EXPECT_EQ(merged.size(), 2U);
    EXPECT_EQ(report.taken, 2U);
    EXPECT_EQ(report.preserved, 0U);
}

TEST(MergeRecords, MergingTwiceChangesNothingTheSecondTime) {
    const std::vector<Symbol> existing = {SymbolAt(Renamed, "FieldMain", Provenance::Manual)};
    const std::vector<Symbol> derived = {SymbolAt(Renamed, "sub_100100", Provenance::Heuristic),
                                         SymbolAt(Fresh, "sub_100200", Provenance::Heuristic)};

    MergeReport first;
    const std::vector<Symbol> once = MergeRecords(existing, derived, first);

    MergeReport second;
    const std::vector<Symbol> twice = MergeRecords(once, derived, second);

    EXPECT_EQ(RenderSymbols(twice), RenderSymbols(once));
    EXPECT_EQ(second.preserved, 1U);
    EXPECT_EQ(second.discarded, 1U);
}

TEST(MergeRecords, AppliesToEveryTable) {
    MergeReport report;

    const std::vector<FunctionRange> functions = MergeRecords(
        std::vector<FunctionRange>{FunctionRange{.address = Renamed,
                                                 .size = 0x10,
                                                 .instruction_set = arm::InstructionSet::Thumb,
                                                 .alternate_entries = {},
                                                 .source = Provenance::Manual}},
        std::vector<FunctionRange>{FunctionRange{.address = Renamed,
                                                 .size = 0x20,
                                                 .instruction_set = arm::InstructionSet::Arm,
                                                 .alternate_entries = {},
                                                 .source = Provenance::Heuristic}},
        report);
    ASSERT_EQ(functions.size(), 1U);
    EXPECT_EQ(functions.front().instruction_set, arm::InstructionSet::Thumb);

    const std::vector<Region> regions =
        MergeRecords(std::vector<Region>{Region{.address = Named,
                                                .size = 0x10,
                                                .kind = RegionKind::JumpTable,
                                                .instruction_set = std::nullopt,
                                                .source = Provenance::Manual}},
                     std::vector<Region>{Region{.address = Named,
                                                .size = 0x10,
                                                .kind = RegionKind::Code,
                                                .instruction_set = arm::InstructionSet::Arm,
                                                .source = Provenance::Heuristic}},
                     report);
    ASSERT_EQ(regions.size(), 1U);
    EXPECT_EQ(regions.front().kind, RegionKind::JumpTable);

    const std::vector<CodePointer> pointers =
        MergeRecords(std::vector<CodePointer>{},
                     std::vector<CodePointer>{CodePointer{
                         .address = Named, .target = Fresh, .source = Provenance::Heuristic}},
                     report);
    EXPECT_EQ(pointers.size(), 1U);
}

}  // namespace
}  // namespace hexlith::domain::analysis
