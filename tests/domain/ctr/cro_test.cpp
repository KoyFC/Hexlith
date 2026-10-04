#include "domain/ctr/cro.h"

#include "support/synthetic_cro.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::domain::ctr {
namespace {

using test::BuildSyntheticCro;
using test::DefaultCroSpec;
using test::StaticModuleSpec;
using test::SyntheticCroExport;
using test::SyntheticCroSpec;

// The addresses the extended header gives the main executable, which the CRS
// repeats because the segments it describes are already mapped.
constexpr std::uint32_t StaticTextAddress = 0x00100000;

constexpr std::uint8_t UnknownSegmentType = 9;

// A segment index no module declares, for a position that resolves nowhere.
constexpr std::uint32_t AbsentSegmentIndex = 9;

// An odd position in a code segment, which is how a Thumb entry point is spelt.
constexpr std::uint32_t ThumbCodeOffset = 0x101;

// The number of named exports the static module publishes, measured on the
// reference image and the only free source of symbol names the project has.
constexpr std::size_t StaticNamedExportCount = 461;

// Every count in the header, so that a test can set each one to a different
// value and check that the parser reads it back from the field it belongs to.
// The table offsets and counts alternate in the header, which is what makes a
// pair easy to slip by one.
using Count = std::size_t CroTableCounts::*;

constexpr std::array<Count, 12> EveryCount = {
    &CroTableCounts::segments,
    &CroTableCounts::named_exports,
    &CroTableCounts::indexed_exports,
    &CroTableCounts::export_tree_nodes,
    &CroTableCounts::import_modules,
    &CroTableCounts::external_relocations,
    &CroTableCounts::named_imports,
    &CroTableCounts::indexed_imports,
    &CroTableCounts::anonymous_imports,
    &CroTableCounts::static_anonymous_symbols,
    &CroTableCounts::internal_relocations,
    &CroTableCounts::static_relocations,
};

// Distinct, and large enough that none of them could be mistaken for a
// segment count or a length the builder computes for itself.
constexpr std::size_t FirstSentinel = 101;

// Enough of a count or size field to run a table past the end of the module.
constexpr std::uint8_t OversizedFieldByte = 0xFF;

CroHeader ParseOrFail(const std::vector<std::uint8_t>& module) {
    const core::Result<CroHeader> header = ParseCroHeader(core::ByteSpan{module});
    EXPECT_TRUE(header.Ok()) << header.Failure().Message();
    return header.Ok() ? header.Value() : CroHeader{};
}

TEST(ParseCroHeader, ReadsTheModuleName) {
    const CroHeader header = ParseOrFail(BuildSyntheticCro(DefaultCroSpec()));
    EXPECT_EQ(header.name, "DllField");
    EXPECT_FALSE(header.IsStaticModule());
    EXPECT_FALSE(header.fixed_up);
}

TEST(ParseCroHeader, ReadsTheSizeAndTypeOfEverySegment) {
    const SyntheticCroSpec spec = DefaultCroSpec();
    const CroHeader header = ParseOrFail(BuildSyntheticCro(spec));

    ASSERT_EQ(header.segments.size(), spec.segments.size());
    for (std::size_t index = 0; index < spec.segments.size(); ++index) {
        EXPECT_EQ(header.segments[index].size, spec.segments[index].size) << index;
        EXPECT_EQ(header.segments[index].type, spec.segments[index].type) << index;
    }
}

TEST(ParseCroHeader, NumbersTheSegmentsInTableOrder) {
    const CroHeader header = ParseOrFail(BuildSyntheticCro(DefaultCroSpec()));

    for (std::size_t index = 0; index < header.segments.size(); ++index) {
        EXPECT_EQ(header.segments[index].index, index);
        EXPECT_TRUE(header.segments[index].type_known) << index;
    }
}

TEST(ParseCroHeader, SumsTheBytesOfEachSegmentType) {
    const SyntheticCroSpec spec = DefaultCroSpec();
    const CroHeader header = ParseOrFail(BuildSyntheticCro(spec));

    EXPECT_EQ(header.SegmentBytes(CroSegmentType::Code), spec.segments[0].size);
    EXPECT_EQ(header.SegmentBytes(CroSegmentType::ReadOnlyData), spec.segments[1].size);
    EXPECT_EQ(header.SegmentBytes(CroSegmentType::Data), spec.segments[2].size);
    EXPECT_EQ(header.SegmentBytes(CroSegmentType::Bss), spec.segments[3].size);
}

TEST(ParseCroHeader, ReadsTheMeasuredCountsOfTheLargestModule) {
    const SyntheticCroSpec spec = DefaultCroSpec();
    const CroHeader header = ParseOrFail(BuildSyntheticCro(spec));

    EXPECT_EQ(header.counts.named_exports, spec.counts.named_exports);
    EXPECT_EQ(header.counts.import_modules, spec.counts.import_modules);
    EXPECT_EQ(header.counts.external_relocations, spec.counts.external_relocations);
    EXPECT_EQ(header.counts.anonymous_imports, spec.counts.anonymous_imports);
    EXPECT_EQ(header.counts.internal_relocations, spec.counts.internal_relocations);
}

// docs/formats/cro0.md records that the external relocation table was
// labelled an import table during the initial survey, and that every number
// reported before that was caught was wrong. Giving each count a distinct
// value is what makes a field read one position off show up as the wrong
// number rather than as a plausible one.
TEST(ParseCroHeader, ReadsEveryCountFromTheFieldItBelongsTo) {
    SyntheticCroSpec spec = DefaultCroSpec();
    for (std::size_t index = 0; index < EveryCount.size(); ++index) {
        spec.counts.*EveryCount[index] = FirstSentinel + index;
    }

    // Three counts describe tables the builder lays out for real, so they come
    // from those tables rather than from the spec and are checked where the
    // tables are read.
    spec.counts.segments = spec.segments.size();
    spec.counts.named_exports = spec.named_exports.size();
    spec.counts.indexed_exports = spec.indexed_exports.size();

    const CroHeader header = ParseOrFail(BuildSyntheticCro(spec));
    for (std::size_t index = 0; index < EveryCount.size(); ++index) {
        EXPECT_EQ(header.counts.*EveryCount[index], spec.counts.*EveryCount[index]) << index;
    }
}

TEST(ParseCroHeader, RecognisesTheStaticModule) {
    const SyntheticCroSpec spec = StaticModuleSpec();
    const CroHeader header = ParseOrFail(BuildSyntheticCro(spec));

    EXPECT_EQ(header.name, cro::StaticModuleName);
    EXPECT_TRUE(header.IsStaticModule());
    EXPECT_EQ(header.counts.named_exports, spec.counts.named_exports);
}

// The static module's segment offsets are absolute virtual addresses rather
// than file offsets, because the segments it describes are already mapped. On
// the reference image they match the extended header's code set addresses,
// which is what cross-checks both readings.
TEST(ParseCroHeader, ReportsTheStaticModulesVirtualAddressesWithoutCheckingThemAgainstTheFile) {
    const std::vector<std::uint8_t> module = BuildSyntheticCro(StaticModuleSpec());
    const CroHeader header = ParseOrFail(module);

    ASSERT_FALSE(header.segments.empty());
    EXPECT_EQ(header.segments[0].offset, StaticTextAddress);
    EXPECT_GT(header.segments[0].offset, module.size())
        << "a virtual address is not an offset into the file";
    EXPECT_GT(header.segments[0].size, module.size());
}

TEST(ParseCroHeader, ReportsAFixedUpModule) {
    SyntheticCroSpec spec = DefaultCroSpec();
    spec.fixed_up = true;
    const CroHeader header = ParseOrFail(BuildSyntheticCro(spec));
    EXPECT_TRUE(header.fixed_up);
}

TEST(ParseCroHeader, ReportsASegmentTypeItDoesNotKnow) {
    std::vector<std::uint8_t> module = BuildSyntheticCro(DefaultCroSpec());
    const CroHeader header = ParseOrFail(module);
    const std::size_t table =
        core::ReadU32At(core::ByteSpan{module}, cro::FieldOffset(cro::SegmentTableOffsetField))
            .Value();
    module[table + cro::SegmentTypeOffset] = UnknownSegmentType;

    const CroHeader changed = ParseOrFail(module);
    ASSERT_FALSE(changed.segments.empty());
    EXPECT_FALSE(changed.segments[0].type_known);
    EXPECT_EQ(changed.segments[0].raw_type, UnknownSegmentType);
    EXPECT_EQ(changed.SegmentBytes(CroSegmentType::Code), 0U)
        << "a segment of an unknown type is not counted as code";
    EXPECT_EQ(header.segments[0].raw_type, 0U);
}

TEST(ParseCroHeader, RefusesSomethingThatIsNotAModule) {
    std::vector<std::uint8_t> module = BuildSyntheticCro(DefaultCroSpec());
    module[cro::FieldOffset(cro::MagicField)] = 'X';

    const core::Result<CroHeader> header = ParseCroHeader(core::ByteSpan{module});
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("no CRO0 or FIXD magic"), std::string::npos);
}

TEST(ParseCroHeader, RefusesAFileTooShortForTheHeader) {
    const std::vector<std::uint8_t> module = BuildSyntheticCro(DefaultCroSpec());
    const core::Result<CroHeader> header =
        ParseCroHeader(core::ByteSpan{module}.subspan(0, cro::HeaderSize - 1));
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("a CRO header is"), std::string::npos);
}

TEST(ParseCroHeader, RefusesASegmentTableRunningPastTheEnd) {
    std::vector<std::uint8_t> module = BuildSyntheticCro(DefaultCroSpec());
    module[cro::FieldOffset(cro::SegmentCountField) + 1] = OversizedFieldByte;

    const core::Result<CroHeader> header = ParseCroHeader(core::ByteSpan{module});
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("the segment table"), std::string::npos)
        << header.Failure().Message();
}

TEST(ParseCroHeader, RefusesASegmentWhoseContentsRunPastTheEnd) {
    std::vector<std::uint8_t> module = BuildSyntheticCro(DefaultCroSpec());
    const std::size_t table =
        core::ReadU32At(core::ByteSpan{module}, cro::FieldOffset(cro::SegmentTableOffsetField))
            .Value();
    module[table + cro::SegmentSizeOffset + 2] = OversizedFieldByte;

    const core::Result<CroHeader> header = ParseCroHeader(core::ByteSpan{module});
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("segment 0"), std::string::npos)
        << header.Failure().Message();
}

TEST(ParseCroHeader, RefusesAModuleWithNoName) {
    std::vector<std::uint8_t> module = BuildSyntheticCro(DefaultCroSpec());
    for (std::size_t index = 0; index < 4; ++index) {
        module[cro::FieldOffset(cro::ModuleNameSizeField) + index] = 0;
    }

    const core::Result<CroHeader> header = ParseCroHeader(core::ByteSpan{module});
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("declares no name"), std::string::npos);
}

TEST(ParseCroHeader, RefusesANameThatIsNotText) {
    std::vector<std::uint8_t> module = BuildSyntheticCro(DefaultCroSpec());
    module[cro::HeaderSize + 1] = 0x01;

    const core::Result<CroHeader> header = ParseCroHeader(core::ByteSpan{module});
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("the module name"), std::string::npos)
        << header.Failure().Message();
}

TEST(ParseCroHeader, RefusesAModuleDeclaringMoreBytesThanTheFileHolds) {
    std::vector<std::uint8_t> module = BuildSyntheticCro(DefaultCroSpec());
    module[cro::FieldOffset(cro::FileSizeField) + 2] = OversizedFieldByte;

    const core::Result<CroHeader> header = ParseCroHeader(core::ByteSpan{module});
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("the module declares"), std::string::npos)
        << header.Failure().Message();
}

// A file is a module because of the magic the format puts at 0x80, not
// because of its extension. static.crs is a module and does not end in .cro,
// and .crr/static.crr is not one although its name suggests it.
// The export readers. The tag encoding is the part worth testing hardest: the
// segment index sits in the low four bits and the offset in the other 28, so a
// reader that shifted the wrong way would produce addresses that still look
// plausible.

TEST(ReadCroNamedExports, ReadsEveryNameAndPosition) {
    const SyntheticCroSpec spec = DefaultCroSpec();
    const std::vector<std::uint8_t> module = BuildSyntheticCro(spec);
    const CroHeader header = ParseOrFail(module);

    const core::Result<std::vector<CroNamedExport>> exports = ReadCroNamedExports(module, header);
    ASSERT_TRUE(exports.Ok()) << exports.Failure().Message();
    ASSERT_EQ(exports.Value().size(), spec.named_exports.size());

    EXPECT_EQ(exports.Value()[0].name, "DllFieldMain");
    EXPECT_EQ(exports.Value()[1].name, "DllFieldOnLoad");
    EXPECT_EQ(exports.Value()[0].tag.SegmentIndex(), 0U);
    EXPECT_EQ(exports.Value()[1].tag.OffsetInSegment(), spec.named_exports[1].offset_in_segment);
}

TEST(ReadCroNamedExports, ResolvesAPositionAgainstItsOwnSegment) {
    SyntheticCroSpec spec = DefaultCroSpec();
    spec.named_exports = {
        SyntheticCroExport{.name = "in_code", .segment_index = 0, .offset_in_segment = 0x20},
        SyntheticCroExport{.name = "in_rodata", .segment_index = 1, .offset_in_segment = 0x8},
    };

    const std::vector<std::uint8_t> module = BuildSyntheticCro(spec);
    const CroHeader header = ParseOrFail(module);

    const core::Result<std::vector<CroNamedExport>> exports = ReadCroNamedExports(module, header);
    ASSERT_TRUE(exports.Ok()) << exports.Failure().Message();
    EXPECT_EQ(exports.Value()[0].position, header.segments[0].offset + 0x20);
    EXPECT_EQ(exports.Value()[1].position, header.segments[1].offset + 0x8);
    EXPECT_NE(header.segments[0].offset, header.segments[1].offset);
}

TEST(ReadCroNamedExports, ResolvesTheStaticModuleAgainstVirtualAddresses) {
    const SyntheticCroSpec spec = StaticModuleSpec();
    const std::vector<std::uint8_t> module = BuildSyntheticCro(spec);
    const CroHeader header = ParseOrFail(module);

    const core::Result<std::vector<CroNamedExport>> exports = ReadCroNamedExports(module, header);
    ASSERT_TRUE(exports.Ok()) << exports.Failure().Message();
    ASSERT_EQ(exports.Value().size(), StaticNamedExportCount);

    // The first export sits at the start of the text segment, which the static
    // module reports as a virtual address because the segment is already
    // mapped.
    EXPECT_EQ(exports.Value().front().position, StaticTextAddress);
}

TEST(ReadCroNamedExports, KeepsTheLowBitOfAThumbPosition) {
    SyntheticCroSpec spec = DefaultCroSpec();
    spec.named_exports = {
        SyntheticCroExport{
            .name = "thumb_entry", .segment_index = 0, .offset_in_segment = ThumbCodeOffset},
    };

    const std::vector<std::uint8_t> module = BuildSyntheticCro(spec);
    const CroHeader header = ParseOrFail(module);

    const core::Result<std::vector<CroNamedExport>> exports = ReadCroNamedExports(module, header);
    ASSERT_TRUE(exports.Ok()) << exports.Failure().Message();

    // The low bit is the interworking flag and the reader does not strip it.
    // Deciding what it means is the job of whatever turns an export into a
    // symbol, and a reader that normalised it would throw the fact away.
    EXPECT_EQ(exports.Value().front().tag.OffsetInSegment() % 2, 1U);
}

TEST(ReadCroNamedExports, RefusesAPositionInASegmentTheModuleDoesNotHave) {
    SyntheticCroSpec spec = DefaultCroSpec();
    spec.named_exports = {
        SyntheticCroExport{
            .name = "nowhere", .segment_index = AbsentSegmentIndex, .offset_in_segment = 0},
    };

    const std::vector<std::uint8_t> module = BuildSyntheticCro(spec);
    const CroHeader header = ParseOrFail(module);

    const core::Result<std::vector<CroNamedExport>> exports = ReadCroNamedExports(module, header);
    ASSERT_FALSE(exports.Ok());
    EXPECT_NE(exports.Failure().Message().find("segment 9"), std::string::npos)
        << exports.Failure().Message();
}

TEST(ReadCroNamedExports, RefusesAPositionPastTheEndOfItsSegment) {
    SyntheticCroSpec spec = DefaultCroSpec();
    const auto code_size = static_cast<std::uint32_t>(spec.segments[0].size);
    spec.named_exports = {
        SyntheticCroExport{.name = "past_it", .segment_index = 0, .offset_in_segment = code_size},
    };

    const std::vector<std::uint8_t> module = BuildSyntheticCro(spec);
    const CroHeader header = ParseOrFail(module);

    const core::Result<std::vector<CroNamedExport>> exports = ReadCroNamedExports(module, header);
    ASSERT_FALSE(exports.Ok());
    EXPECT_NE(exports.Failure().Message().find("bytes long"), std::string::npos)
        << exports.Failure().Message();
}

TEST(ReadCroNamedExports, RefusesANameOutsideTheStringTable) {
    const SyntheticCroSpec spec = DefaultCroSpec();
    std::vector<std::uint8_t> module = BuildSyntheticCro(spec);
    const CroHeader header = ParseOrFail(module);

    const std::size_t table =
        core::ReadU32At(core::ByteSpan{module}, cro::FieldOffset(cro::NamedExportTableOffsetField))
            .Value();
    module[table + cro::NamedExportNameOffsetOffset] = 0;
    module[table + cro::NamedExportNameOffsetOffset + 1] = 0;
    module[table + cro::NamedExportNameOffsetOffset + 2] = 0;
    module[table + cro::NamedExportNameOffsetOffset + 3] = 0;

    const core::Result<std::vector<CroNamedExport>> exports = ReadCroNamedExports(module, header);
    ASSERT_FALSE(exports.Ok());
    EXPECT_NE(exports.Failure().Message().find("outside the export string table"),
              std::string::npos)
        << exports.Failure().Message();
}

TEST(ReadCroNamedExports, RefusesAnEmptyName) {
    const SyntheticCroSpec spec = DefaultCroSpec();
    std::vector<std::uint8_t> module = BuildSyntheticCro(spec);
    const CroHeader header = ParseOrFail(module);

    const std::size_t strings =
        core::ReadU32At(core::ByteSpan{module}, cro::FieldOffset(cro::ExportStringsOffsetField))
            .Value();
    module[strings] = 0;

    const core::Result<std::vector<CroNamedExport>> exports = ReadCroNamedExports(module, header);
    ASSERT_FALSE(exports.Ok());
    EXPECT_NE(exports.Failure().Message().find("empty name"), std::string::npos)
        << exports.Failure().Message();
}

TEST(ReadCroNamedExports, ReadsNothingFromAModuleThatExportsNothing) {
    SyntheticCroSpec spec = DefaultCroSpec();
    spec.named_exports.clear();
    spec.counts.named_exports = 0;

    const std::vector<std::uint8_t> module = BuildSyntheticCro(spec);
    const CroHeader header = ParseOrFail(module);

    const core::Result<std::vector<CroNamedExport>> exports = ReadCroNamedExports(module, header);
    ASSERT_TRUE(exports.Ok()) << exports.Failure().Message();
    EXPECT_TRUE(exports.Value().empty());
}

TEST(ReadCroIndexedExports, NumbersThePositionsInTableOrder) {
    SyntheticCroSpec spec = DefaultCroSpec();
    spec.indexed_exports = {
        SyntheticCroExport{.name = {}, .segment_index = 0, .offset_in_segment = 0x40},
        SyntheticCroExport{.name = {}, .segment_index = 1, .offset_in_segment = 0x10},
    };

    const std::vector<std::uint8_t> module = BuildSyntheticCro(spec);
    const CroHeader header = ParseOrFail(module);

    const core::Result<std::vector<CroIndexedExport>> exports =
        ReadCroIndexedExports(module, header);
    ASSERT_TRUE(exports.Ok()) << exports.Failure().Message();
    ASSERT_EQ(exports.Value().size(), 2U);
    EXPECT_EQ(exports.Value()[0].index, 0U);
    EXPECT_EQ(exports.Value()[0].position, header.segments[0].offset + 0x40);
    EXPECT_EQ(exports.Value()[1].index, 1U);
    EXPECT_EQ(exports.Value()[1].position, header.segments[1].offset + 0x10);
}

TEST(ReadCroIndexedExports, RefusesAPositionInASegmentTheModuleDoesNotHave) {
    SyntheticCroSpec spec = DefaultCroSpec();
    spec.indexed_exports = {SyntheticCroExport{
        .name = {}, .segment_index = AbsentSegmentIndex, .offset_in_segment = 0}};

    const std::vector<std::uint8_t> module = BuildSyntheticCro(spec);
    const CroHeader header = ParseOrFail(module);

    const core::Result<std::vector<CroIndexedExport>> exports =
        ReadCroIndexedExports(module, header);
    ASSERT_FALSE(exports.Ok());
    EXPECT_NE(exports.Failure().Message().find("indexed export 0"), std::string::npos)
        << exports.Failure().Message();
}

TEST(CroSegmentTag, SplitsTheIndexFromTheOffset) {
    // The encoding the format uses, from SegmentTag in cro_helper.h: four bits
    // of segment index, then 28 of byte offset.
    EXPECT_EQ(CroSegmentTag{.raw = 0x00007850}.SegmentIndex(), 0U);
    EXPECT_EQ(CroSegmentTag{.raw = 0x00007850}.OffsetInSegment(), 0x785U);
    EXPECT_EQ(CroSegmentTag{.raw = 0x00000003}.SegmentIndex(), 3U);
    EXPECT_EQ(CroSegmentTag{.raw = 0x00000003}.OffsetInSegment(), 0U);
    EXPECT_EQ(CroSegmentTag{.raw = 0xFFFFFFFF}.SegmentIndex(), 0xFU);
    EXPECT_EQ(CroSegmentTag{.raw = 0xFFFFFFFF}.OffsetInSegment(), 0x0FFFFFFFU);
}

TEST(LooksLikeCroModule, DecidesByTheMagicRatherThanTheName) {
    EXPECT_TRUE(LooksLikeCroModule(core::ByteSpan{BuildSyntheticCro(DefaultCroSpec())}));
    EXPECT_TRUE(LooksLikeCroModule(core::ByteSpan{BuildSyntheticCro(StaticModuleSpec())}));

    SyntheticCroSpec fixed = DefaultCroSpec();
    fixed.fixed_up = true;
    EXPECT_TRUE(LooksLikeCroModule(core::ByteSpan{BuildSyntheticCro(fixed)}));

    const std::vector<std::uint8_t> not_a_module(cro::HeaderSize, 0x5A);
    EXPECT_FALSE(LooksLikeCroModule(core::ByteSpan{not_a_module}));
    EXPECT_FALSE(LooksLikeCroModule(core::ByteSpan{}));
}

TEST(Describe, NamesEverySegmentType) {
    EXPECT_EQ(Describe(CroSegmentType::Code), "code");
    EXPECT_EQ(Describe(CroSegmentType::ReadOnlyData), "rodata");
    EXPECT_EQ(Describe(CroSegmentType::Data), "data");
    EXPECT_EQ(Describe(CroSegmentType::Bss), "bss");
}

}  // namespace
}  // namespace hexlith::domain::ctr
