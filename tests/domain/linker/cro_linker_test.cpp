#include "domain/linker/cro_linker.h"

#include "support/synthetic_cro.h"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

namespace hexlith::domain::linker {
namespace {

using ctr::CroSegmentType;
using test::AnonymousImport;
using test::BuildSyntheticCro;
using test::ImportsFrom;
using test::NamedImport;
using test::SyntheticCroExport;
using test::SyntheticCroRelocation;
using test::SyntheticCroSegment;
using test::SyntheticCroSpec;

constexpr std::uint32_t Code = 0;
constexpr std::uint32_t ReadOnly = 1;
constexpr std::uint32_t Data = 2;
constexpr std::uint32_t Bss = 3;

// The main executable's segment addresses, which the static module repeats.
constexpr std::uint32_t StaticText = 0x00100000;
constexpr std::uint32_t StaticReadOnly = 0x0053C000;
constexpr std::uint32_t StaticData = 0x005A6000;
constexpr std::size_t StaticSegmentSize = 0x1000;

// Places in the static module's text: the function the title registers static
// destructors with, a function exported under a name that a module exports
// too, the handler unresolved imports are pointed at, a Thumb function a module
// imports by position, and a word the static module patches with an address in
// a module.
constexpr std::uint32_t AtexitOffset = 0x100;
constexpr std::uint32_t SharedOffset = 0x180;
constexpr std::uint32_t UnresolvedOffset = 0x800;
constexpr std::uint32_t ThumbFunctionOffset = 0x101;
constexpr std::uint32_t StaticPatchedWord = 0x200;

// Where the game puts the two modules, in the region above the static image
// where the trace saw modules land, with their data and BSS in the heap.
constexpr Placement PlacementA{
    .module_address = 0x006A5000, .data_address = 0x08066000, .bss_address = 0x08067000};
constexpr Placement PlacementB{
    .module_address = 0x006B0000, .data_address = 0x08070000, .bss_address = 0x08071000};

// Words in module A's code that its relocations patch, and what they point at.
constexpr std::uint32_t StaticImportWord = 0x10;
constexpr std::uint32_t StaticImportSecondWord = 0x14;
constexpr std::uint32_t AtexitWord = 0x20;
constexpr std::uint32_t SharedWord = 0x24;
constexpr std::uint32_t CodePointerWord = 0x30;
constexpr std::uint32_t CodePointerTarget = 0x40;
constexpr std::uint32_t BssPointerTarget = 8;
constexpr std::uint32_t DataPointerWord = 4;
constexpr std::uint32_t DataPointerTarget = 16;

// What module B takes from A, and what the static module takes from B.
constexpr std::uint32_t ObjectInA = 8;
constexpr std::uint32_t FunctionInB = 0x50;
constexpr std::uint32_t ImportFromAWord = 0;
constexpr std::uint32_t ImportFromCWord = 4;

SyntheticCroRelocation Patch(std::uint32_t segment,
                             std::uint32_t offset,
                             std::uint32_t addend = 0,
                             std::uint8_t symbol_segment = 0) {
    return SyntheticCroRelocation{
        .segment_index = segment,
        .offset_in_segment = offset,
        .type = static_cast<std::uint8_t>(ctr::CroRelocationType::AbsoluteAddress),
        .addend = addend,
        .symbol_segment = symbol_segment};
}

std::uint32_t Tag(std::uint32_t segment, std::uint32_t offset) {
    return (offset << ctr::cro::SegmentTagOffsetShift) | segment;
}

SyntheticCroSpec StaticSpec() {
    SyntheticCroSpec spec;
    spec.static_module = true;
    spec.segments = {
        SyntheticCroSegment{
            .type = CroSegmentType::Code, .size = StaticSegmentSize, .virtual_address = StaticText},
        SyntheticCroSegment{.type = CroSegmentType::ReadOnlyData,
                            .size = StaticSegmentSize,
                            .virtual_address = StaticReadOnly},
        SyntheticCroSegment{
            .type = CroSegmentType::Data, .size = StaticSegmentSize, .virtual_address = StaticData},
    };
    spec.named_exports = {
        SyntheticCroExport{
            .name = "nnroAeabiAtexit_", .segment_index = Code, .offset_in_segment = AtexitOffset},
        SyntheticCroExport{
            .name = "SharedName", .segment_index = Code, .offset_in_segment = SharedOffset},
    };
    spec.counts.export_tree_nodes = 2;
    spec.import_modules = {
        ImportsFrom("DllB", {AnonymousImport(Code, FunctionInB, {Patch(Code, StaticPatchedWord)})}),
    };
    spec.on_unresolved_hook = Tag(Code, UnresolvedOffset);
    return spec;
}

// The four segments of a small module. The position past the end of the
// read-only data is one no import may name.
constexpr std::size_t ModuleCodeSize = 0x100;
constexpr std::size_t ModuleReadOnlySize = 0x40;
constexpr std::size_t ModuleDataSize = 0x20;
constexpr std::size_t ModuleBssSize = 0x40;
constexpr std::uint32_t PastTheReadOnlyData = ModuleReadOnlySize;

std::vector<SyntheticCroSegment> FourSegments() {
    return {
        SyntheticCroSegment{.type = CroSegmentType::Code, .size = ModuleCodeSize},
        SyntheticCroSegment{.type = CroSegmentType::ReadOnlyData, .size = ModuleReadOnlySize},
        SyntheticCroSegment{.type = CroSegmentType::Data, .size = ModuleDataSize},
        SyntheticCroSegment{.type = CroSegmentType::Bss, .size = ModuleBssSize},
    };
}

SyntheticCroSpec ModuleASpec() {
    SyntheticCroSpec spec;
    spec.name = "DllA";
    spec.segments = FourSegments();
    spec.named_exports = {
        SyntheticCroExport{.name = "SharedName", .segment_index = Code, .offset_in_segment = 0}};
    spec.counts.export_tree_nodes = 2;
    spec.named_imports = {
        NamedImport("__aeabi_atexit", {Patch(Code, AtexitWord)}),
        NamedImport("SharedName", {Patch(Code, SharedWord)}),
    };
    spec.import_modules = {
        ImportsFrom("|static|",
                    {AnonymousImport(
                        Code,
                        ThumbFunctionOffset,
                        {Patch(Code, StaticImportWord), Patch(Code, StaticImportSecondWord, 4)})}),
    };
    spec.internal_relocations = {
        Patch(Code, CodePointerWord, CodePointerTarget, Code),
        Patch(ReadOnly, 0, BssPointerTarget, Bss),
        Patch(Data, DataPointerWord, DataPointerTarget, Data),
    };
    return spec;
}

SyntheticCroSpec ModuleBSpec() {
    SyntheticCroSpec spec;
    spec.name = "DllB";
    spec.segments = FourSegments();
    spec.import_modules = {
        ImportsFrom("DllA", {AnonymousImport(ReadOnly, ObjectInA, {Patch(Code, ImportFromAWord)})}),
        ImportsFrom("DllC", {AnonymousImport(Code, 0, {Patch(Code, ImportFromCWord)})}),
    };
    return spec;
}

// The modules a test links, kept alive for as long as the linker borrows them.
struct Fixture {
    std::vector<std::uint8_t> static_file = BuildSyntheticCro(StaticSpec());
    std::vector<std::uint8_t> a_file = BuildSyntheticCro(ModuleASpec());
    std::vector<std::uint8_t> b_file = BuildSyntheticCro(ModuleBSpec());

    CroModule static_module = Read(static_file);
    CroModule a = Read(a_file);
    CroModule b = Read(b_file);

    static CroModule Read(const std::vector<std::uint8_t>& file) {
        const core::Result<CroModule> module = ReadCroModule(core::ByteSpan{file});
        EXPECT_TRUE(module.Ok()) << module.Failure().Message();
        return module.Ok() ? module.Value() : CroModule{};
    }

    static std::uint32_t FileOffset(const CroModule& module, std::uint32_t segment) {
        return static_cast<std::uint32_t>(module.header.segments.at(segment).offset);
    }
};

Linker StartOrFail(const CroModule& static_module) {
    core::Result<Linker> linker = Linker::FromStaticModule(static_module);
    EXPECT_TRUE(linker.Ok()) << linker.Failure().Message();
    return std::move(linker.Value());
}

std::size_t LoadOrFail(Linker& linker, const CroModule& module, const Placement& placement) {
    const core::Result<std::size_t> loaded = linker.Load(module, placement);
    EXPECT_TRUE(loaded.Ok()) << loaded.Failure().Message();
    return loaded.ValueOr(0);
}

// The word an external relocation of a module patches at a position in code.
const LinkedWord& ExternalAt(const LoadedModule& loaded, std::uint32_t offset) {
    for (const LinkedWord& word : loaded.external) {
        if (word.target.SegmentIndex() == Code && word.target.OffsetInSegment() == offset) {
            return word;
        }
    }
    ADD_FAILURE() << "no external relocation at code offset " << offset;
    static const LinkedWord None;
    return None;
}

TEST(Linker, PlacesCodeWithTheModuleAndDataAndBssInTheirBuffers) {
    const Fixture fixture;
    Linker linker = StartOrFail(fixture.static_module);
    const LoadedModule& a = linker.Modules().at(LoadOrFail(linker, fixture.a, PlacementA));

    EXPECT_EQ(a.segment_addresses.at(Code),
              PlacementA.module_address + Fixture::FileOffset(fixture.a, Code));
    EXPECT_EQ(a.segment_addresses.at(ReadOnly),
              PlacementA.module_address + Fixture::FileOffset(fixture.a, ReadOnly));
    EXPECT_EQ(a.segment_addresses.at(Data), PlacementA.data_address);
    EXPECT_EQ(a.segment_addresses.at(Bss), PlacementA.bss_address);
}

TEST(Linker, PointsAnInternalRelocationAtItsSegmentPlusTheAddend) {
    const Fixture fixture;
    Linker linker = StartOrFail(fixture.static_module);
    const LoadedModule& a = linker.Modules().at(LoadOrFail(linker, fixture.a, PlacementA));

    ASSERT_EQ(a.internal.size(), 3U);
    EXPECT_EQ(a.internal[0].value, a.segment_addresses.at(Code) + CodePointerTarget);
    EXPECT_EQ(a.internal[0].address, a.segment_addresses.at(Code) + CodePointerWord);
    EXPECT_EQ(a.internal[1].value, PlacementA.bss_address + BssPointerTarget);
    EXPECT_EQ(a.internal[2].value, PlacementA.data_address + DataPointerTarget);

    // A word in data is where the game copies data to, not where the file has it.
    EXPECT_EQ(a.internal[2].address, PlacementA.data_address + DataPointerWord);
}

TEST(Linker, ResolvesAnAnonymousImportFromTheStaticModule) {
    const Fixture fixture;
    Linker linker = StartOrFail(fixture.static_module);
    const LoadedModule& a = linker.Modules().at(LoadOrFail(linker, fixture.a, PlacementA));

    // The low bit of a Thumb position is carried through, as it has to be for
    // a BX through the word to land in Thumb.
    EXPECT_EQ(ExternalAt(a, StaticImportWord).value, StaticText + ThumbFunctionOffset);
    EXPECT_EQ(ExternalAt(a, StaticImportSecondWord).value, StaticText + ThumbFunctionOffset + 4);

    const Resolution& resolution = a.resolutions.at(2);
    EXPECT_TRUE(resolution.resolved);
    EXPECT_EQ(resolution.exporter, 0U);
}

TEST(Linker, ResolvesAtexitAgainstTheExportTheLoaderSubstitutes) {
    const Fixture fixture;
    Linker linker = StartOrFail(fixture.static_module);
    const LoadedModule& a = linker.Modules().at(LoadOrFail(linker, fixture.a, PlacementA));

    EXPECT_EQ(ExternalAt(a, AtexitWord).value, StaticText + AtexitOffset);
}

TEST(Linker, TakesANamedImportFromTheFirstModuleThatExportsIt) {
    const Fixture fixture;
    Linker linker = StartOrFail(fixture.static_module);
    LoadOrFail(linker, fixture.a, PlacementA);

    // A exports the name too, but the static module is searched first.
    const LoadedModule& a = linker.Modules().at(1);
    EXPECT_EQ(ExternalAt(a, SharedWord).value, StaticText + SharedOffset);
}

TEST(Linker, FindsNoNamedExportInAModuleWithoutAnExportTree) {
    const Fixture fixture;
    SyntheticCroSpec spec = StaticSpec();
    spec.counts.export_tree_nodes = 0;
    const std::vector<std::uint8_t> file = BuildSyntheticCro(spec);
    const CroModule treeless = Fixture::Read(file);

    Linker linker = StartOrFail(treeless);
    const LoadedModule& a = linker.Modules().at(LoadOrFail(linker, fixture.a, PlacementA));

    EXPECT_EQ(ExternalAt(a, SharedWord).value, 0U);
    EXPECT_FALSE(a.resolutions.at(1).resolved);
}

TEST(Linker, PointsAnImportFromAnAbsentModuleAtTheUnresolvedHook) {
    const Fixture fixture;
    Linker linker = StartOrFail(fixture.static_module);
    const LoadedModule& b = linker.Modules().at(LoadOrFail(linker, fixture.b, PlacementB));

    // B has no hook, so an import nobody supplies holds its addend alone.
    EXPECT_EQ(ExternalAt(b, ImportFromCWord).value, 0U);
    EXPECT_FALSE(b.resolutions.at(1).resolved);
}

TEST(Linker, PointsTheStaticModulesOwnImportsAtItsHookUntilTheModuleLoads) {
    const Fixture fixture;
    Linker linker = StartOrFail(fixture.static_module);

    const LoadedModule& before = linker.Modules().at(0);
    ASSERT_EQ(before.external.size(), 1U);
    EXPECT_EQ(before.external[0].value, StaticText + UnresolvedOffset);
    EXPECT_EQ(before.external[0].address, StaticText + StaticPatchedWord);

    const std::size_t b = LoadOrFail(linker, fixture.b, PlacementB);
    const LoadedModule& after = linker.Modules().at(0);
    EXPECT_EQ(after.external[0].value,
              linker.Modules().at(b).segment_addresses.at(Code) + FunctionInB);
    EXPECT_EQ(after.resolutions[0].exporter, b);
}

TEST(Linker, ResolvesAnImportWhicheverOfTheTwoModulesLoadsFirst) {
    const Fixture fixture;

    Linker a_first = StartOrFail(fixture.static_module);
    LoadOrFail(a_first, fixture.a, PlacementA);
    const LoadedModule& b_after = a_first.Modules().at(LoadOrFail(a_first, fixture.b, PlacementB));

    Linker b_first = StartOrFail(fixture.static_module);
    LoadOrFail(b_first, fixture.b, PlacementB);
    LoadOrFail(b_first, fixture.a, PlacementA);
    const LoadedModule& b_before = b_first.Modules().at(1);

    const std::uint32_t expected =
        PlacementA.module_address + Fixture::FileOffset(fixture.a, ReadOnly) + ObjectInA;
    EXPECT_EQ(ExternalAt(b_after, ImportFromAWord).value, expected);
    EXPECT_EQ(ExternalAt(b_before, ImportFromAWord).value, expected);
    EXPECT_EQ(b_before.resolutions.at(0).exporter, 2U);
}

TEST(Linker, WritesTheLinkedWordsIntoACopyOfTheFile) {
    const Fixture fixture;
    Linker linker = StartOrFail(fixture.static_module);
    const LoadedModule& a = linker.Modules().at(LoadOrFail(linker, fixture.a, PlacementA));

    const core::Result<std::vector<std::uint8_t>> image =
        LinkedImage(a, core::ByteSpan{fixture.a_file});
    ASSERT_TRUE(image.Ok()) << image.Failure().Message();
    ASSERT_EQ(image.Value().size(), fixture.a_file.size());

    const std::size_t at = Fixture::FileOffset(fixture.a, Code) + CodePointerWord;
    ASSERT_TRUE(a.internal[0].value.has_value());
    EXPECT_EQ(core::ReadU32At(core::ByteSpan{image.Value()}, at).Value(),
              a.internal[0].value.value_or(0));

    // A byte no relocation touches is the file's.
    const std::size_t untouched = Fixture::FileOffset(fixture.a, Code) + CodePointerWord + 4;
    EXPECT_EQ(image.Value().at(untouched), fixture.a_file.at(untouched));
}

TEST(Linker, RefusesAnImageOfTheStaticModule) {
    const Fixture fixture;
    const Linker linker = StartOrFail(fixture.static_module);
    EXPECT_FALSE(LinkedImage(linker.Modules().at(0), core::ByteSpan{fixture.static_file}).Ok());
}

TEST(Linker, RefusesAnImportOfAPositionTheExporterDoesNotHave) {
    const Fixture fixture;
    SyntheticCroSpec spec = ModuleBSpec();
    spec.import_modules[0].anonymous[0].offset_in_segment = PastTheReadOnlyData;
    const std::vector<std::uint8_t> file = BuildSyntheticCro(spec);
    const CroModule b = Fixture::Read(file);

    Linker linker = StartOrFail(fixture.static_module);
    LoadOrFail(linker, fixture.a, PlacementA);
    EXPECT_FALSE(linker.Load(b, PlacementB).Ok());
}

TEST(Linker, RefusesABranchRelocation) {
    const Fixture fixture;
    SyntheticCroSpec spec = ModuleASpec();
    spec.internal_relocations[0].type =
        static_cast<std::uint8_t>(ctr::CroRelocationType::ArmBranch);
    const std::vector<std::uint8_t> file = BuildSyntheticCro(spec);
    const CroModule a = Fixture::Read(file);

    Linker linker = StartOrFail(fixture.static_module);
    EXPECT_FALSE(linker.Load(a, PlacementA).Ok());
}

TEST(Linker, RefusesAModuleThatHandsSymbolsToTheStaticModule) {
    const Fixture fixture;
    SyntheticCroSpec spec = ModuleASpec();
    spec.static_anonymous_symbols = {AnonymousImport(Code, 0, {Patch(Code, StaticPatchedWord)})};
    const std::vector<std::uint8_t> file = BuildSyntheticCro(spec);
    const CroModule a = Fixture::Read(file);

    Linker linker = StartOrFail(fixture.static_module);
    EXPECT_FALSE(linker.Load(a, PlacementA).Ok());
}

TEST(Linker, StartsOnlyFromTheStaticModule) {
    const Fixture fixture;
    EXPECT_FALSE(Linker::FromStaticModule(fixture.a).Ok());

    Linker linker = StartOrFail(fixture.static_module);
    EXPECT_FALSE(linker.Load(fixture.static_module, PlacementA).Ok());
}

TEST(RelocatedValue, WritesWhatApplyRelocationWrites) {
    using enum ctr::CroRelocationType;
    constexpr std::uint32_t Symbol = 0x006A5100;
    constexpr std::uint32_t Word = 0x006A5000;

    EXPECT_EQ(RelocatedValue(AbsoluteAddress, true, Symbol, 4, Word).Value(), Symbol + 4);
    EXPECT_EQ(RelocatedValue(AbsoluteAddress2, true, Symbol, 4, Word).Value(), Symbol + 4);
    EXPECT_EQ(RelocatedValue(RelativeAddress, true, Symbol, 4, Word).Value(), Symbol + 4 - Word);
    EXPECT_EQ(RelocatedValue(Nothing, true, Symbol, 4, Word).Value(), std::nullopt);
    EXPECT_FALSE(RelocatedValue(ThumbBranch, true, Symbol, 4, Word).Ok());
    EXPECT_FALSE(RelocatedValue(Nothing, false, Symbol, 4, Word).Ok());
}

}  // namespace
}  // namespace hexlith::domain::linker
