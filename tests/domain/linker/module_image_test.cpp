#include "core/byte_reader.h"
#include "domain/ctr/layout.h"
#include "domain/linker/module_image.h"

#include "support/synthetic_cro.h"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::linker {
namespace {

namespace cro = ctr::cro;
using ctr::CroSegmentType;
using test::AnonymousImport;
using test::BuildSyntheticCro;
using test::ImportsFrom;
using test::NamedImport;
using test::SyntheticCroExport;
using test::SyntheticCroRelocation;
using test::SyntheticCroSegment;
using test::SyntheticCroSpec;

constexpr std::uint32_t ModuleAddress = 0x006A5000;
constexpr Placement Buffers{
    .module_address = ModuleAddress, .data_address = 0x08066000, .bss_address = 0x08067000};
constexpr BufferSizes Room{.data = 0x100, .bss = 0x100};

constexpr std::uint32_t Code = 0;
constexpr std::uint32_t Data = 2;
constexpr std::uint32_t Bss = 3;

// The four segments of a small module, and the words of its code that its
// imports patch.
constexpr std::size_t CodeSize = 0x100;
constexpr std::size_t ReadOnlySize = 0x40;
constexpr std::size_t DataSize = 0x20;
constexpr std::size_t BssSize = 0x40;
constexpr std::uint32_t NamedImportWord = 0x10;
constexpr std::uint32_t NamedImportSecondWord = 0x14;
constexpr std::uint32_t AnonymousImportWord = 0x18;
constexpr std::uint32_t StaticFunction = 0x20;

// The static module's one segment, the main executable's text.
constexpr std::size_t StaticTextSize = 0x1000;
constexpr std::uint32_t StaticText = 0x00100000;

SyntheticCroRelocation Patch(std::uint32_t segment, std::uint32_t offset) {
    return SyntheticCroRelocation{.segment_index = segment, .offset_in_segment = offset};
}

SyntheticCroSpec ModuleSpec() {
    SyntheticCroSpec spec;
    spec.name = "DllImage";
    spec.segments = {
        SyntheticCroSegment{.type = CroSegmentType::Code, .size = CodeSize},
        SyntheticCroSegment{.type = CroSegmentType::ReadOnlyData, .size = ReadOnlySize},
        SyntheticCroSegment{.type = CroSegmentType::Data, .size = DataSize},
        SyntheticCroSegment{.type = CroSegmentType::Bss, .size = BssSize},
    };
    spec.named_exports = {
        SyntheticCroExport{.name = "DllImageMain", .segment_index = Code, .offset_in_segment = 0}};
    spec.named_imports = {NamedImport(
        "SomeFunction", {Patch(Code, NamedImportWord), Patch(Code, NamedImportSecondWord)})};
    spec.import_modules = {ImportsFrom(
        "|static|", {AnonymousImport(Code, StaticFunction, {Patch(Code, AnonymousImportWord)})})};
    return spec;
}

std::uint32_t Field(const std::vector<std::uint8_t>& image, std::size_t index) {
    return core::ReadU32At(image, cro::FieldOffset(index)).ValueOr(0);
}

std::uint32_t WordAt(const std::vector<std::uint8_t>& image, std::uint32_t address) {
    return core::ReadU32At(image, address - ModuleAddress).ValueOr(0);
}

void SetField(std::vector<std::uint8_t>& image, std::size_t index, std::uint32_t value) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
        image.at(cro::FieldOffset(index) + byte) = static_cast<std::uint8_t>(value >> (8U * byte));
    }
}

// A table entry's word, once the header holds the table's address.
std::uint32_t EntryWord(const std::vector<std::uint8_t>& image,
                        std::size_t table_field,
                        std::size_t entry_size,
                        std::size_t index,
                        std::size_t word) {
    return WordAt(image,
                  Field(image, table_field) + static_cast<std::uint32_t>(index * entry_size) +
                      static_cast<std::uint32_t>(word));
}

std::vector<std::uint8_t> Rebased(std::vector<std::uint8_t> image) {
    const core::Status done = RebaseModuleImage(image, ModuleAddress, Buffers, Room, false);
    EXPECT_TRUE(done.Ok()) << done.Failure().Message();
    return image;
}

TEST(RebaseModuleImage, TurnsTheHeadersOffsetsIntoAddresses) {
    const std::vector<std::uint8_t> file = BuildSyntheticCro(ModuleSpec());
    const std::vector<std::uint8_t> image = Rebased(file);
    for (const std::size_t field : {cro::CodeOffsetField,
                                    cro::ModuleNameOffsetField,
                                    cro::SegmentTableOffsetField,
                                    cro::ExternalRelocationTableOffsetField}) {
        EXPECT_EQ(Field(image, field), Field(file, field) + ModuleAddress) << "field " << field;
    }
    EXPECT_EQ(Field(image, cro::SegmentCountField), Field(file, cro::SegmentCountField))
        << "counts stay counts";
    EXPECT_EQ(Field(image, cro::StaticAnonymousSymbolTableOffsetField) != 0,
              Field(file, cro::StaticAnonymousSymbolTableOffsetField) != 0)
        << "an offset of zero stays zero";
}

TEST(RebaseModuleImage, PutsDataAndBssInTheirBuffersAndTheRestWithTheModule) {
    const std::vector<std::uint8_t> file = BuildSyntheticCro(ModuleSpec());
    const std::vector<std::uint8_t> image = Rebased(file);
    auto segment = [&](const std::vector<std::uint8_t>& bytes, std::size_t index) {
        const std::uint32_t table = Field(bytes, cro::SegmentTableOffsetField);
        const std::uint32_t base = &bytes == &file ? table : table - ModuleAddress;
        return core::ReadU32At(bytes, base + (index * cro::SegmentEntrySize)).ValueOr(0);
    };
    EXPECT_EQ(segment(image, Code), segment(file, Code) + ModuleAddress);
    EXPECT_EQ(segment(image, Data), Buffers.data_address);
    EXPECT_EQ(segment(image, Bss), Buffers.bss_address);
}

TEST(RebaseModuleImage, PointsTheTablesAtTheirStringsAndBatches) {
    const std::vector<std::uint8_t> image = Rebased(BuildSyntheticCro(ModuleSpec()));
    const std::uint32_t export_name = EntryWord(image,
                                                cro::NamedExportTableOffsetField,
                                                cro::NamedExportEntrySize,
                                                0,
                                                cro::NamedExportNameOffsetOffset);
    EXPECT_GE(export_name, Field(image, cro::ExportStringsOffsetField));

    const std::uint32_t import_name = EntryWord(
        image, cro::NamedImportTableOffsetField, cro::ImportEntrySize, 0, cro::ImportWhatOffset);
    EXPECT_GE(import_name, Field(image, cro::ImportStringsOffsetField));
    const std::uint32_t batch = EntryWord(
        image, cro::NamedImportTableOffsetField, cro::ImportEntrySize, 0, cro::ImportBatchOffset);
    EXPECT_EQ(batch, Field(image, cro::ExternalRelocationTableOffsetField))
        << "the first named import's batch opens the table";

    const std::uint32_t module_name = EntryWord(image,
                                                cro::ImportModuleTableOffsetField,
                                                cro::ImportModuleEntrySize,
                                                0,
                                                cro::ImportModuleNameOffsetOffset);
    EXPECT_GE(module_name, Field(image, cro::ImportStringsOffsetField));
}

TEST(RebaseModuleImage, MarksEveryBatchUnresolved) {
    std::vector<std::uint8_t> file = BuildSyntheticCro(ModuleSpec());
    const std::uint32_t table = Field(file, cro::ExternalRelocationTableOffsetField);
    // Two batches, the named import's two entries and the anonymous one.
    const std::size_t second_batch = table + (2 * cro::RelocationEntrySize);
    file.at(table + cro::RelocationBatchResolvedOffset) = 1;
    file.at(second_batch + cro::RelocationBatchResolvedOffset) = 1;
    const std::vector<std::uint8_t> image = Rebased(file);
    EXPECT_EQ(image.at(table + cro::RelocationBatchResolvedOffset), 0);
    EXPECT_EQ(image.at(second_batch + cro::RelocationBatchResolvedOffset), 0);
}

TEST(RebaseModuleImage, LeavesTheStaticModulesSegmentsWhereTheyAre) {
    SyntheticCroSpec spec;
    spec.static_module = true;
    spec.segments = {SyntheticCroSegment{
        .type = CroSegmentType::Code, .size = StaticTextSize, .virtual_address = StaticText}};
    std::vector<std::uint8_t> image = BuildSyntheticCro(spec);
    const std::uint32_t table = Field(image, cro::SegmentTableOffsetField);
    ASSERT_TRUE(RebaseModuleImage(image, ModuleAddress, {}, {}, true).Ok());
    EXPECT_EQ(core::ReadU32At(image, table).ValueOr(0), StaticText);
}

TEST(RebaseModuleImage, RefusesAModuleAlreadyFixedOrListed) {
    SyntheticCroSpec fixed = ModuleSpec();
    fixed.fixed_up = true;
    std::vector<std::uint8_t> image = BuildSyntheticCro(fixed);
    EXPECT_FALSE(RebaseModuleImage(image, ModuleAddress, Buffers, Room, false).Ok());

    image = BuildSyntheticCro(ModuleSpec());
    SetField(image, cro::NextModuleField, ModuleAddress);
    EXPECT_FALSE(RebaseModuleImage(image, ModuleAddress, Buffers, Room, false).Ok());
}

TEST(RebaseModuleImage, RefusesADataBufferSmallerThanTheSegment) {
    std::vector<std::uint8_t> image = BuildSyntheticCro(ModuleSpec());
    const BufferSizes small{.data = DataSize - 1, .bss = Room.bss};
    EXPECT_FALSE(RebaseModuleImage(image, ModuleAddress, Buffers, small, false).Ok());
}

// A module rebased and fixed at level 1, and where the tables it keeps end.
struct FixedAtLevelOne {
    FixedAtLevelOne()
        : image(Rebased(BuildSyntheticCro(ModuleSpec()))),
          kept_end(Field(image, cro::ImportStringsOffsetField) +
                   Field(image, cro::ImportStringsSizeField)),
          fixed_size(Fix(image)) {}

    static std::uint32_t Fix(std::vector<std::uint8_t>& bytes) {
        const core::Result<std::uint32_t> fixed = FixModuleImage(bytes, ModuleAddress, 1);
        EXPECT_TRUE(fixed.Ok()) << fixed.Failure().Message();
        return fixed.ValueOr(0);
    }

    std::vector<std::uint8_t> image;
    std::uint32_t kept_end = 0;
    std::uint32_t fixed_size = 0;
};

TEST(FixModuleImage, MarksTheModuleFixed) {
    const FixedAtLevelOne fixed;
    EXPECT_TRUE(core::MatchesAt(fixed.image, cro::FieldOffset(cro::MagicField), cro::FixedUpMagic));
}

TEST(FixModuleImage, CropsTheTablesPastTheLevelsBarrier) {
    const FixedAtLevelOne fixed;
    for (const std::size_t field : {cro::StaticAnonymousSymbolTableOffsetField,
                                    cro::InternalRelocationTableOffsetField,
                                    cro::StaticRelocationTableOffsetField}) {
        EXPECT_GE(Field(fixed.image, field), fixed.kept_end) << "field " << field;
        EXPECT_EQ(Field(fixed.image, field + 1), 0U) << "field " << field + 1;
    }
}

TEST(FixModuleImage, KeepsWholePagesUpToTheLastTableKept) {
    constexpr std::uint32_t Page = 0x1000;
    const FixedAtLevelOne fixed;
    EXPECT_EQ(fixed.fixed_size % Page, 0U);
    EXPECT_EQ(Field(fixed.image, cro::FixedSizeField), fixed.fixed_size);
    EXPECT_GE(ModuleAddress + fixed.fixed_size, fixed.kept_end);
}

TEST(FixModuleImage, KeepsEverythingAtLevelZero) {
    std::vector<std::uint8_t> image = Rebased(BuildSyntheticCro(ModuleSpec()));
    const std::uint32_t internal = Field(image, cro::InternalRelocationTableOffsetField);
    ASSERT_TRUE(FixModuleImage(image, ModuleAddress, 0).Ok());
    EXPECT_TRUE(core::MatchesAt(image, cro::FieldOffset(cro::MagicField), "CRO0"));
    EXPECT_EQ(Field(image, cro::InternalRelocationTableOffsetField), internal);
}

}  // namespace
}  // namespace hexlith::domain::linker
