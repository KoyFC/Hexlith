#include "domain/analysis/export_seed.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::domain::analysis {
namespace {

namespace ctr = domain::ctr;

// The addresses the static module reports, which are virtual because the
// segments it describes are already mapped.
constexpr std::uint32_t TextAddress = 0x00100000;
constexpr std::uint32_t ReadOnlyAddress = 0x0053C000;
constexpr std::uint32_t SegmentSize = 0x1000;

// A segment whose type the format has gained since this was written, and an
// address for it that belongs to nothing else here.
constexpr std::uint32_t UnknownSegmentRawType = 9;
constexpr std::uint32_t UnknownSegmentAddress = 0x00700000;

ctr::CroSegment Segment(std::size_t index, ctr::CroSegmentType type, std::uint32_t address) {
    return ctr::CroSegment{.index = index,
                           .offset = address,
                           .size = SegmentSize,
                           .raw_type = static_cast<std::uint32_t>(type),
                           .type = type,
                           .type_known = true};
}

ctr::CroHeader StaticHeader() {
    ctr::CroHeader header;
    header.name = std::string{ctr::cro::StaticModuleName};
    header.segments = {Segment(0, ctr::CroSegmentType::Code, TextAddress),
                       Segment(1, ctr::CroSegmentType::ReadOnlyData, ReadOnlyAddress)};
    return header;
}

ctr::CroNamedExport Export(std::string name, std::uint32_t segment, std::uint32_t offset) {
    const std::uint32_t base = segment == 0 ? TextAddress : ReadOnlyAddress;
    return ctr::CroNamedExport{.name = std::move(name),
                               .tag = ctr::CroSegmentTag{.raw = (offset << 4U) | segment},
                               .position = base + offset};
}

SeededTables SeedOrFail(const ctr::CroHeader& header,
                        const std::vector<ctr::CroNamedExport>& exports) {
    const core::Result<SeededTables> seeded = SeedFromExports(header, exports);
    EXPECT_TRUE(seeded.Ok()) << seeded.Failure().Message();
    return seeded.Ok() ? seeded.Value() : SeededTables{};
}

TEST(SeedFromExports, AnArmEntryPointKeepsItsAddress) {
    const SeededTables seeded = SeedOrFail(StaticHeader(), {Export("nnroAeabiAtexit_", 0, 0x20)});

    ASSERT_EQ(seeded.symbols.size(), 1U);
    EXPECT_EQ(seeded.symbols.front().address, TextAddress + 0x20);
    EXPECT_EQ(seeded.symbols.front().kind, SymbolKind::Function);
    EXPECT_EQ(seeded.symbols.front().name, "nnroAeabiAtexit_");
    EXPECT_EQ(seeded.symbols.front().size, UnknownSize);
    EXPECT_EQ(seeded.symbols.front().source, Provenance::CrsExport);

    ASSERT_EQ(seeded.functions.size(), 1U);
    EXPECT_EQ(seeded.functions.front().address, TextAddress + 0x20);
    EXPECT_EQ(seeded.functions.front().instruction_set, arm::InstructionSet::Arm);
    EXPECT_EQ(seeded.functions.front().size, UnknownSize);
    EXPECT_EQ(seeded.functions.front().source, Provenance::CrsExport);
}

TEST(SeedFromExports, AThumbEntryPointLosesTheInterworkingBit) {
    const SeededTables seeded = SeedOrFail(StaticHeader(), {Export("_ZNSsaSEPKc", 0, 0x1CCD)});

    // The address is the instruction, which is even. The bit that said Thumb
    // is in the function record instead of in the address, where it would
    // point one byte into the first halfword.
    ASSERT_EQ(seeded.symbols.size(), 1U);
    EXPECT_EQ(seeded.symbols.front().address, TextAddress + 0x1CCC);
    ASSERT_EQ(seeded.functions.size(), 1U);
    EXPECT_EQ(seeded.functions.front().address, TextAddress + 0x1CCC);
    EXPECT_EQ(seeded.functions.front().instruction_set, arm::InstructionSet::Thumb);
}

TEST(SeedFromExports, AnExportInReadOnlyDataIsDataAndNotAFunction) {
    const SeededTables seeded =
        SeedOrFail(StaticHeader(), {Export("_ZTIN4__rw15__rw_facet_baseE", 1, 0x40)});

    ASSERT_EQ(seeded.symbols.size(), 1U);
    EXPECT_EQ(seeded.symbols.front().kind, SymbolKind::Data);
    EXPECT_EQ(seeded.symbols.front().address, ReadOnlyAddress + 0x40);
    EXPECT_TRUE(seeded.functions.empty());
}

TEST(SeedFromExports, LeavesAnOddDataAddressAlone) {
    // The 16 odd exports in the reference image's read-only data are all type
    // name strings. A character array sits wherever the linker put it, so
    // clearing the low bit of one would move it.
    const SeededTables seeded = SeedOrFail(StaticHeader(), {Export("_ZTSSi", 1, 0x41)});

    ASSERT_EQ(seeded.symbols.size(), 1U);
    EXPECT_EQ(seeded.symbols.front().address, ReadOnlyAddress + 0x41);
}

TEST(SeedFromExports, TwoNamesAtOneAddressAreTwoSymbolsAndOneFunction) {
    // What the static module does with a constructor and its base-object
    // variant: the same code under two mangled names.
    const SeededTables seeded = SeedOrFail(StaticHeader(),
                                           {Export("_ZNSt14__rw_exceptionC1Ev", 0, 0x785),
                                            Export("_ZNSt14__rw_exceptionC2Ev", 0, 0x785)});

    EXPECT_EQ(seeded.symbols.size(), 2U);
    EXPECT_EQ(seeded.symbols[0].address, seeded.symbols[1].address);
    ASSERT_EQ(seeded.functions.size(), 1U);
    EXPECT_EQ(seeded.functions.front().instruction_set, arm::InstructionSet::Thumb);
}

TEST(SeedFromExports, AdmitsThatItDoesNotKnowWhatAnUnknownSegmentHolds) {
    ctr::CroHeader header = StaticHeader();
    header.segments.push_back(ctr::CroSegment{.index = 2,
                                              .offset = UnknownSegmentAddress,
                                              .size = SegmentSize,
                                              .raw_type = UnknownSegmentRawType,
                                              .type = ctr::CroSegmentType::Code,
                                              .type_known = false});

    const SeededTables seeded = SeedOrFail(header, {Export("in_the_unknown", 2, 0x10)});

    ASSERT_EQ(seeded.symbols.size(), 1U);
    EXPECT_EQ(seeded.symbols.front().kind, SymbolKind::Unknown);
    EXPECT_TRUE(seeded.functions.empty());
}

TEST(SeedFromExports, RefusesAnExportInASegmentTheHeaderDoesNotDeclare) {
    const core::Result<SeededTables> seeded =
        SeedFromExports(StaticHeader(), std::vector{Export("nowhere", 5, 0)});

    ASSERT_FALSE(seeded.Ok());
    EXPECT_NE(seeded.Failure().Message().find("segment 5"), std::string::npos)
        << seeded.Failure().Message();
}

TEST(SeedFromExports, SeedsNothingFromAModuleThatExportsNothing) {
    const SeededTables seeded = SeedOrFail(StaticHeader(), {});
    EXPECT_TRUE(seeded.symbols.empty());
    EXPECT_TRUE(seeded.functions.empty());
}

TEST(SeedFromExports, ProducesTablesTheWritersAccept) {
    const SeededTables seeded = SeedOrFail(
        StaticHeader(),
        {Export("thumb_one", 0, 0x101), Export("arm_one", 0, 0x200), Export("a_string", 1, 0x41)});

    const core::Result<std::vector<Symbol>> symbols = ParseSymbols(RenderSymbols(seeded.symbols));
    ASSERT_TRUE(symbols.Ok()) << symbols.Failure().Message();
    EXPECT_EQ(symbols.Value().size(), 3U);

    const core::Result<std::vector<FunctionRange>> functions =
        ParseFunctions(RenderFunctions(seeded.functions));
    ASSERT_TRUE(functions.Ok()) << functions.Failure().Message();
    EXPECT_EQ(functions.Value().size(), 2U);
}

}  // namespace
}  // namespace hexlith::domain::analysis
