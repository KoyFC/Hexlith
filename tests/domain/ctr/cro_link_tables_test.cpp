#include "domain/ctr/cro_link_tables.h"
#include "domain/ctr/layout.h"

#include "support/synthetic_cro.h"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

namespace hexlith::domain::ctr {
namespace {

using test::BuildSyntheticCro;
using test::SyntheticCroImport;
using test::SyntheticCroImportModule;
using test::SyntheticCroRelocation;
using test::SyntheticCroSegment;
using test::SyntheticCroSpec;

constexpr std::size_t CodeSize = 0x200;
constexpr std::size_t ReadOnlySize = 0x80;
constexpr std::size_t DataSize = 0x40;
constexpr std::size_t BssSize = 0x100;

constexpr std::uint32_t CodeSegment = 0;
constexpr std::uint32_t ReadOnlySegment = 1;
constexpr std::uint32_t BssSegment = 3;

// Positions in the exporting modules. The first is odd, which in a code
// segment is how a Thumb entry point is spelt.
constexpr std::uint32_t StaticThumbFunction = 0x1235;
constexpr std::uint32_t StaticReadOnlyObject = 0x40;
constexpr std::uint32_t FieldFunction = 0x800;

// Words in this module's code that receive an address, which is what a
// literal pool is. Each import's batch patches a run of them.
constexpr std::uint32_t FirstPool = 0x10;
constexpr std::uint32_t SecondPool = 0x30;
constexpr std::uint32_t FieldPool = 0x40;
constexpr std::uint32_t LatePool = 0x80;
constexpr std::uint32_t PoolWord = 4;

// An internal relocation from a literal pool to a function later in the code,
// and one from read-only data into the BSS.
constexpr std::uint32_t InternalPool = 0x100;
constexpr std::uint32_t InternalTarget = 0x24;
constexpr std::uint32_t BssObject = 0x10;

// A position in the static module's code that a static relocation patches,
// and the position in this module it receives.
constexpr std::uint32_t StaticPatchedWord = 0x4000;
constexpr std::uint32_t HandedToStatic = 0x60;

// Where the undocumented hook at header index 8 points in the code.
constexpr std::uint32_t UnknownHookOffset = 0x40;

constexpr auto Absolute = static_cast<std::uint8_t>(CroRelocationType::AbsoluteAddress);

// A byte no relocation type in cro_helper.h uses.
constexpr std::uint8_t UnknownRelocationType = 7;

// A segment index no module in these tests declares.
constexpr std::uint8_t AbsentSegment = 9;

SyntheticCroRelocation PatchCodeAt(std::uint32_t offset, std::uint32_t addend = 0) {
    return SyntheticCroRelocation{
        .segment_index = CodeSegment, .offset_in_segment = offset, .addend = addend};
}

SyntheticCroImport AnonymousImport(std::uint32_t segment_index,
                                   std::uint32_t offset_in_segment,
                                   std::vector<SyntheticCroRelocation> batch) {
    SyntheticCroImport entry;
    entry.segment_index = segment_index;
    entry.offset_in_segment = offset_in_segment;
    entry.batch = std::move(batch);
    return entry;
}

SyntheticCroImport NamedImport(std::string name, std::vector<SyntheticCroRelocation> batch) {
    SyntheticCroImport entry;
    entry.name = std::move(name);
    entry.batch = std::move(batch);
    return entry;
}

SyntheticCroImport IndexedImport(std::uint32_t export_index,
                                 std::vector<SyntheticCroRelocation> batch) {
    SyntheticCroImport entry;
    entry.export_index = export_index;
    entry.batch = std::move(batch);
    return entry;
}

SyntheticCroImportModule ImportsFrom(std::string name, std::vector<SyntheticCroImport> anonymous) {
    SyntheticCroImportModule module;
    module.name = std::move(name);
    module.anonymous = std::move(anonymous);
    return module;
}

// A module shaped like the title's: it imports by position from the static
// module and from one other, and patches its own literal pools.
SyntheticCroSpec LinkingSpec() {
    SyntheticCroSpec spec;
    spec.name = "DllFieldEventPlayer";
    spec.segments = {
        SyntheticCroSegment{.type = CroSegmentType::Code, .size = CodeSize},
        SyntheticCroSegment{.type = CroSegmentType::ReadOnlyData, .size = ReadOnlySize},
        SyntheticCroSegment{.type = CroSegmentType::Data, .size = DataSize},
        SyntheticCroSegment{.type = CroSegmentType::Bss, .size = BssSize},
    };
    spec.import_modules = {
        ImportsFrom("|static|",
                    {AnonymousImport(0,
                                     StaticThumbFunction,
                                     {PatchCodeAt(FirstPool), PatchCodeAt(FirstPool + PoolWord)}),
                     AnonymousImport(1, StaticReadOnlyObject, {PatchCodeAt(SecondPool, 8)})}),
        ImportsFrom("DllField",
                    {AnonymousImport(0,
                                     FieldFunction,
                                     {PatchCodeAt(FieldPool),
                                      PatchCodeAt(FieldPool + PoolWord),
                                      PatchCodeAt(FieldPool + (2 * PoolWord))})}),
    };
    spec.internal_relocations = {
        SyntheticCroRelocation{.segment_index = CodeSegment,
                               .offset_in_segment = InternalPool,
                               .type = Absolute,
                               .addend = InternalTarget,
                               .symbol_segment = CodeSegment},
        SyntheticCroRelocation{.segment_index = ReadOnlySegment,
                               .offset_in_segment = 8,
                               .type = Absolute,
                               .addend = BssObject,
                               .symbol_segment = BssSegment},
    };
    return spec;
}

CroLinkTables ReadOrFail(const std::vector<std::uint8_t>& module) {
    const core::Result<CroHeader> header = ParseCroHeader(core::ByteSpan{module});
    EXPECT_TRUE(header.Ok()) << header.Failure().Message();
    if (!header) {
        return {};
    }
    const core::Result<CroLinkTables> tables =
        ReadCroLinkTables(core::ByteSpan{module}, header.Value());
    EXPECT_TRUE(tables.Ok()) << tables.Failure().Message();
    return tables.Ok() ? tables.Value() : CroLinkTables{};
}

core::Status ReadStatus(const std::vector<std::uint8_t>& module) {
    const core::Result<CroHeader> header = ParseCroHeader(core::ByteSpan{module});
    if (!header) {
        return header.Failure();
    }
    const core::Result<CroLinkTables> tables =
        ReadCroLinkTables(core::ByteSpan{module}, header.Value());
    if (!tables) {
        return tables.Failure();
    }
    return {};
}

std::uint32_t FieldOf(const std::vector<std::uint8_t>& module, std::size_t index) {
    return core::ReadU32At(core::ByteSpan{module}, cro::FieldOffset(index)).Value();
}

void PutU32(std::vector<std::uint8_t>& module, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        module.at(offset + index) = static_cast<std::uint8_t>(value >> (8 * index));
    }
}

TEST(ReadCroLinkTables, GroupsAnonymousImportsByTheModuleTheyComeFrom) {
    const CroLinkTables tables = ReadOrFail(BuildSyntheticCro(LinkingSpec()));

    ASSERT_EQ(tables.import_modules.size(), 2U);
    EXPECT_EQ(tables.import_modules[0].name, "|static|");
    EXPECT_EQ(tables.import_modules[1].name, "DllField");
    EXPECT_EQ(tables.import_modules[0].anonymous.size(), 2U);
    EXPECT_EQ(tables.import_modules[1].anonymous.size(), 1U);
    EXPECT_TRUE(tables.import_modules[0].indexed.empty());
}

TEST(ReadCroLinkTables, KeepsAnAnonymousImportAsAPositionInTheExportingModule) {
    const CroLinkTables tables = ReadOrFail(BuildSyntheticCro(LinkingSpec()));

    const CroAnonymousImport& thumb = tables.import_modules[0].anonymous[0];
    EXPECT_EQ(thumb.position.SegmentIndex(), 0U);
    EXPECT_EQ(thumb.position.OffsetInSegment(), StaticThumbFunction);

    const CroAnonymousImport& rodata = tables.import_modules[0].anonymous[1];
    EXPECT_EQ(rodata.position.SegmentIndex(), 1U);
    EXPECT_EQ(rodata.position.OffsetInSegment(), StaticReadOnlyObject);
}

TEST(ReadCroLinkTables, GivesEachImportTheWholeOfItsBatch) {
    const CroLinkTables tables = ReadOrFail(BuildSyntheticCro(LinkingSpec()));

    const CroBatch first = tables.import_modules[0].anonymous[0].batch;
    const CroBatch second = tables.import_modules[0].anonymous[1].batch;
    const CroBatch third = tables.import_modules[1].anonymous[0].batch;
    EXPECT_EQ(first.first, 0U);
    EXPECT_EQ(first.count, 2U);
    EXPECT_EQ(second.first, 2U);
    EXPECT_EQ(second.count, 1U);
    EXPECT_EQ(third.first, 3U);
    EXPECT_EQ(third.count, 3U);
    EXPECT_EQ(tables.external_relocations.size(), 6U);
}

TEST(ReadCroLinkTables, ReadsWhatEachExternalRelocationPatches) {
    const CroLinkTables tables = ReadOrFail(BuildSyntheticCro(LinkingSpec()));

    const CroBatchRelocation& entry = tables.external_relocations[2];
    EXPECT_EQ(entry.target.SegmentIndex(), CodeSegment);
    EXPECT_EQ(entry.target.OffsetInSegment(), SecondPool);
    EXPECT_EQ(entry.type, CroRelocationType::AbsoluteAddress);
    EXPECT_TRUE(entry.type_known);
    EXPECT_EQ(entry.addend, 8U);
    EXPECT_TRUE(entry.batch_end);
    EXPECT_FALSE(entry.batch_resolved);

    EXPECT_FALSE(tables.external_relocations[0].batch_end);
    EXPECT_TRUE(tables.external_relocations[1].batch_end);
}

TEST(ReadCroLinkTables, ReadsTheSegmentAnInternalRelocationPointsInto) {
    const CroLinkTables tables = ReadOrFail(BuildSyntheticCro(LinkingSpec()));

    ASSERT_EQ(tables.internal_relocations.size(), 2U);
    const CroInternalRelocation& into_bss = tables.internal_relocations[1];
    EXPECT_EQ(into_bss.target.SegmentIndex(), ReadOnlySegment);
    EXPECT_EQ(into_bss.target.OffsetInSegment(), 8U);
    EXPECT_EQ(into_bss.symbol_segment, BssSegment);
    EXPECT_EQ(into_bss.addend, BssObject);
    EXPECT_EQ(into_bss.type, CroRelocationType::AbsoluteAddress);
}

TEST(ReadCroLinkTables, ReadsNamedImportsAndTheirNames) {
    SyntheticCroSpec spec = LinkingSpec();
    spec.named_imports = {
        NamedImport("__aeabi_atexit", {PatchCodeAt(LatePool)}),
        NamedImport("nnroControlObject_", {PatchCodeAt(LatePool + PoolWord)}),
    };
    const CroLinkTables tables = ReadOrFail(BuildSyntheticCro(spec));

    ASSERT_EQ(tables.named_imports.size(), 2U);
    EXPECT_EQ(tables.named_imports[0].name, "__aeabi_atexit");
    EXPECT_EQ(tables.named_imports[1].name, "nnroControlObject_");
    EXPECT_EQ(tables.named_imports[1].batch.first, 1U);
    EXPECT_EQ(tables.named_imports[1].batch.count, 1U);

    // The named batches come first in the table and the module batches follow.
    EXPECT_EQ(tables.import_modules[0].anonymous[0].batch.first, 2U);
}

TEST(ReadCroLinkTables, ReadsIndexedImportsByExportNumber) {
    SyntheticCroSpec spec = LinkingSpec();
    spec.import_modules[1].indexed = {
        IndexedImport(3, {PatchCodeAt(LatePool)}),
    };
    const CroLinkTables tables = ReadOrFail(BuildSyntheticCro(spec));

    ASSERT_EQ(tables.import_modules[1].indexed.size(), 1U);
    EXPECT_EQ(tables.import_modules[1].indexed[0].export_index, 3U);
    EXPECT_EQ(tables.import_modules[1].indexed[0].batch.count, 1U);
}

TEST(ReadCroLinkTables, ReadsStaticAnonymousSymbolsAgainstTheStaticRelocationTable) {
    SyntheticCroSpec spec = LinkingSpec();
    spec.static_anonymous_symbols = {
        AnonymousImport(
            CodeSegment,
            HandedToStatic,
            {PatchCodeAt(StaticPatchedWord), PatchCodeAt(StaticPatchedWord + PoolWord)}),
    };
    const CroLinkTables tables = ReadOrFail(BuildSyntheticCro(spec));

    ASSERT_EQ(tables.static_anonymous_symbols.size(), 1U);
    EXPECT_EQ(tables.static_anonymous_symbols[0].position.OffsetInSegment(), HandedToStatic);
    EXPECT_EQ(tables.static_anonymous_symbols[0].batch.count, 2U);

    // The static relocations patch the static module, so their targets are
    // positions this module does not have and are not refused for it.
    ASSERT_EQ(tables.static_relocations.size(), 2U);
    EXPECT_EQ(tables.static_relocations[0].target.OffsetInSegment(), StaticPatchedWord);
}

TEST(ReadCroLinkTables, ReadsTheHooksAsTheyAreWritten) {
    SyntheticCroSpec spec = LinkingSpec();
    spec.unknown_hook = UnknownHookOffset << cro::SegmentTagOffsetShift;
    const CroLinkTables tables = ReadOrFail(BuildSyntheticCro(spec));

    EXPECT_EQ(tables.hooks.unknown.SegmentIndex(), CodeSegment);
    EXPECT_EQ(tables.hooks.unknown.OffsetInSegment(), UnknownHookOffset);
    EXPECT_EQ(tables.hooks.on_unresolved.raw, test::NoSegmentTag);
}

TEST(ReadCroLinkTables, ReadsNothingFromAModuleThatLinksWithNothing) {
    SyntheticCroSpec spec = LinkingSpec();
    spec.import_modules.clear();
    spec.internal_relocations.clear();
    const CroLinkTables tables = ReadOrFail(BuildSyntheticCro(spec));

    EXPECT_TRUE(tables.import_modules.empty());
    EXPECT_TRUE(tables.external_relocations.empty());
    EXPECT_TRUE(tables.internal_relocations.empty());
}

TEST(ReadCroLinkTables, ReportsARelocationTypeItDoesNotKnow) {
    SyntheticCroSpec spec = LinkingSpec();
    spec.internal_relocations[0].type = UnknownRelocationType;
    const CroLinkTables tables = ReadOrFail(BuildSyntheticCro(spec));

    EXPECT_FALSE(tables.internal_relocations[0].type_known);
    EXPECT_EQ(tables.internal_relocations[0].raw_type, UnknownRelocationType);
}

TEST(ReadCroLinkTables, RefusesTablesOutOfOrder) {
    std::vector<std::uint8_t> module = BuildSyntheticCro(LinkingSpec());
    // Move the internal relocation table in front of the import strings.
    PutU32(module,
           cro::FieldOffset(cro::InternalRelocationTableOffsetField),
           FieldOf(module, cro::ImportStringsOffsetField) - 4);

    EXPECT_FALSE(ReadStatus(module).Ok());
}

TEST(ReadCroLinkTables, RefusesABatchOffsetInTheMiddleOfABatch) {
    std::vector<std::uint8_t> module = BuildSyntheticCro(LinkingSpec());
    const std::size_t first_import = FieldOf(module, cro::AnonymousImportTableOffsetField);
    const std::uint32_t batch = FieldOf(module, cro::ExternalRelocationTableOffsetField);
    PutU32(module, first_import + cro::ImportBatchOffset, batch + cro::RelocationEntrySize);

    const core::Status status = ReadStatus(module);
    ASSERT_FALSE(status.Ok());
    EXPECT_NE(status.Failure().Message().find("middle of a batch"), std::string::npos);
}

TEST(ReadCroLinkTables, RefusesABatchOffsetOutsideTheTable) {
    std::vector<std::uint8_t> module = BuildSyntheticCro(LinkingSpec());
    const std::size_t first_import = FieldOf(module, cro::AnonymousImportTableOffsetField);
    PutU32(module, first_import + cro::ImportBatchOffset, 0);

    EXPECT_FALSE(ReadStatus(module).Ok());
}

TEST(ReadCroLinkTables, RefusesARelocationPastTheEndOfItsSegment) {
    SyntheticCroSpec spec = LinkingSpec();
    spec.import_modules[0].anonymous[0].batch[0].offset_in_segment = CodeSize;

    EXPECT_FALSE(ReadStatus(BuildSyntheticCro(spec)).Ok());
}

TEST(ReadCroLinkTables, RefusesAnInternalRelocationIntoASegmentTheModuleDoesNotHave) {
    SyntheticCroSpec spec = LinkingSpec();
    spec.internal_relocations[0].symbol_segment = AbsentSegment;

    EXPECT_FALSE(ReadStatus(BuildSyntheticCro(spec)).Ok());
}

TEST(ReadCroLinkTables, RefusesAnExternalTableWhoseLastEntryEndsNoBatch) {
    std::vector<std::uint8_t> module = BuildSyntheticCro(LinkingSpec());
    const std::size_t table = FieldOf(module, cro::ExternalRelocationTableOffsetField);
    const std::size_t count = FieldOf(module, cro::ExternalRelocationCountField);
    module.at(table + ((count - 1) * cro::RelocationEntrySize) + cro::RelocationBatchEndOffset) = 0;

    EXPECT_FALSE(ReadStatus(module).Ok());
}

TEST(ReadCroLinkTables, RefusesAModuleNameOutsideTheImportStrings) {
    std::vector<std::uint8_t> module = BuildSyntheticCro(LinkingSpec());
    const std::size_t entry = FieldOf(module, cro::ImportModuleTableOffsetField);
    PutU32(module, entry + cro::ImportModuleNameOffsetOffset, 0);

    EXPECT_FALSE(ReadStatus(module).Ok());
}

TEST(ReadCroLinkTables, RefusesAnImportSliceRunningPastItsTable) {
    std::vector<std::uint8_t> module = BuildSyntheticCro(LinkingSpec());
    const std::size_t second =
        FieldOf(module, cro::ImportModuleTableOffsetField) + cro::ImportModuleEntrySize;
    PutU32(module, second + cro::ImportModuleAnonymousCountOffset, 2);

    EXPECT_FALSE(ReadStatus(module).Ok());
}

TEST(Describe, NamesEveryRelocationType) {
    EXPECT_EQ(Describe(CroRelocationType::Nothing), "nothing");
    EXPECT_EQ(Describe(CroRelocationType::AbsoluteAddress), "absolute");
    EXPECT_EQ(Describe(CroRelocationType::RelativeAddress), "relative");
    EXPECT_EQ(Describe(CroRelocationType::ThumbBranch), "thumb-branch");
    EXPECT_EQ(Describe(CroRelocationType::ArmBranch), "arm-branch");
    EXPECT_EQ(Describe(CroRelocationType::ModifyArmBranch), "modify-arm-branch");
    EXPECT_EQ(Describe(CroRelocationType::AbsoluteAddress2), "absolute-2");
    EXPECT_EQ(Describe(CroRelocationType::AlignedRelativeAddress), "aligned-relative");
}

}  // namespace
}  // namespace hexlith::domain::ctr
