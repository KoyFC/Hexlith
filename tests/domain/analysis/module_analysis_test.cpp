#include "domain/analysis/module_analysis.h"

#include "support/synthetic_cro.h"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::analysis {
namespace {

namespace arm = domain::arm;
namespace ctr = domain::ctr;
namespace linker = domain::linker;

using test::AnonymousImport;
using test::BuildSyntheticCro;
using test::ImportsFrom;
using test::SyntheticCroExport;
using test::SyntheticCroRelocation;
using test::SyntheticCroSegment;
using test::SyntheticCroSpec;

constexpr std::uint32_t ArmPush = 0xE92D4010;            // push {r4, lr}
constexpr std::uint32_t ArmPop = 0xE8BD8010;             // pop {r4, pc}
constexpr std::uint32_t ArmMove = 0xE1A01002;            // mov r1, r2
constexpr std::uint32_t ArmReturn = 0xE12FFF1E;          // bx lr
constexpr std::uint32_t ArmCallThreeAhead = 0xEB000001;  // bl to the instruction three on

// Where things sit in the module's code segment, as distances into it.
//
//   0x00  push {r4, lr}     exported
//   0x04  bl 0x10           a call, which the walk follows
//   0x08  pop {r4, pc}
//   0x0C  .word             a literal the loader relocates to point at 0x18
//   0x10  bx lr             reached only by the call
//   0x14  bx lr             reached only because another module imports it
//   0x18  push {r4, lr}     reached only through the relocated word at 0x0C
//   0x1C  pop {r4, pc}
//   0x20  mov r1, r2        reached by nothing
//   0x24  bx lr
constexpr std::uint32_t Exported = 0x00;
constexpr std::uint32_t Called = 0x10;
constexpr std::uint32_t Imported = 0x14;
constexpr std::uint32_t RelocatedWord = 0x0C;
constexpr std::uint32_t PointedAt = 0x18;
constexpr std::uint32_t Unreached = 0x20;
constexpr std::uint32_t CodeSize = 0x28;

constexpr std::uint32_t CodeSegment = 0;
constexpr std::uint32_t DataSize = 0x10;
constexpr std::uint32_t BssSize = 0x20;

// The main executable's segment address, which the static module repeats.
constexpr std::uint32_t StaticText = 0x00100000;
constexpr std::uint32_t StaticTextSize = 0x1000;

// A word in the module's data that receives an address in the main executable.
constexpr std::uint32_t StaticFunction = 0x100;

std::vector<std::uint8_t> Code() {
    const std::vector<std::uint32_t> words = {ArmPush,
                                              ArmCallThreeAhead,
                                              ArmPop,
                                              0,
                                              ArmReturn,
                                              ArmReturn,
                                              ArmPush,
                                              ArmPop,
                                              ArmMove,
                                              ArmReturn};
    std::vector<std::uint8_t> bytes;
    for (const std::uint32_t word : words) {
        for (unsigned index = 0; index < 4; ++index) {
            bytes.push_back(static_cast<std::uint8_t>(word >> (8U * index)));
        }
    }
    return bytes;
}

SyntheticCroSpec StaticSpec() {
    SyntheticCroSpec spec;
    spec.static_module = true;
    spec.segments = {SyntheticCroSegment{
        .type = ctr::CroSegmentType::Code, .size = StaticTextSize, .virtual_address = StaticText}};
    return spec;
}

SyntheticCroSpec ModuleSpec() {
    SyntheticCroSpec spec;
    spec.name = "DllTest";
    spec.segments = {
        SyntheticCroSegment{.type = ctr::CroSegmentType::Code, .size = CodeSize},
        SyntheticCroSegment{.type = ctr::CroSegmentType::Data, .size = DataSize},
        SyntheticCroSegment{.type = ctr::CroSegmentType::Bss, .size = BssSize},
    };
    spec.segment_contents[CodeSegment] = Code();
    spec.named_exports = {
        SyntheticCroExport{.name = "DllTestMain", .segment_index = 0, .offset_in_segment = 0}};
    spec.internal_relocations = {
        SyntheticCroRelocation{
            .segment_index = CodeSegment,
            .offset_in_segment = RelocatedWord,
            .type = static_cast<std::uint8_t>(ctr::CroRelocationType::AbsoluteAddress),
            .addend = PointedAt,
            .symbol_segment = CodeSegment},
    };
    spec.import_modules = {ImportsFrom(
        "|static|",
        {AnonymousImport(0,
                         StaticFunction,
                         {SyntheticCroRelocation{.segment_index = 1, .offset_in_segment = 0}})})};
    return spec;
}

struct Fixture {
    std::vector<std::uint8_t> static_file = BuildSyntheticCro(StaticSpec());
    std::vector<std::uint8_t> module_file = BuildSyntheticCro(ModuleSpec());
    linker::CroModule static_module = Read(static_file);
    linker::CroModule module = Read(module_file);

    static linker::CroModule Read(const std::vector<std::uint8_t>& file) {
        const core::Result<linker::CroModule> read = linker::ReadCroModule(core::ByteSpan{file});
        EXPECT_TRUE(read.Ok()) << read.Failure().Message();
        return read.Ok() ? read.Value() : linker::CroModule{};
    }

    [[nodiscard]] std::uint32_t CodeAt(std::uint32_t offset) const {
        return static_cast<std::uint32_t>(module.header.segments.at(CodeSegment).offset) + offset;
    }

    [[nodiscard]] ModuleAnalysis Analyze(std::span<const ctr::CroSegmentTag> imported) const {
        const core::Result<ModuleAnalysis> analysed = AnalyzeModule(
            static_module,
            ModuleAnalysisRequest{
                .module = &module, .file = module_file, .imported_by_others = imported});
        EXPECT_TRUE(analysed.Ok()) << analysed.Failure().Message();
        return analysed.Ok() ? analysed.Value() : ModuleAnalysis{};
    }
};

const FunctionRange* FunctionAt(const ModuleAnalysis& analysis, std::uint32_t address) {
    const auto found =
        std::ranges::find_if(analysis.functions, [address](const FunctionRange& function) {
            return function.address == address;
        });
    return found == analysis.functions.end() ? nullptr : &*found;
}

std::vector<ctr::CroSegmentTag> ImportedByAnother() {
    return {ctr::CroSegmentTag{.raw = Imported << ctr::cro::SegmentTagOffsetShift}};
}

TEST(AnalyzeModule, AnalysesTheModuleAtItsFileOffsets) {
    const Fixture fixture;
    const ModuleAnalysis analysis = fixture.Analyze(ImportedByAnother());

    ASSERT_EQ(analysis.symbols.size(), 1U);
    EXPECT_EQ(analysis.symbols[0].address, fixture.CodeAt(Exported));
    EXPECT_EQ(analysis.symbols[0].name, "DllTestMain");
}

TEST(AnalyzeModule, FindsAFunctionAtEachKindOfSeed) {
    const Fixture fixture;
    const ModuleAnalysis analysis = fixture.Analyze(ImportedByAnother());

    const FunctionRange* exported = FunctionAt(analysis, fixture.CodeAt(Exported));
    const FunctionRange* called = FunctionAt(analysis, fixture.CodeAt(Called));
    const FunctionRange* imported = FunctionAt(analysis, fixture.CodeAt(Imported));
    const FunctionRange* pointed = FunctionAt(analysis, fixture.CodeAt(PointedAt));
    ASSERT_NE(exported, nullptr);
    ASSERT_NE(called, nullptr);
    ASSERT_NE(imported, nullptr);
    ASSERT_NE(pointed, nullptr);

    EXPECT_EQ(exported->source, Provenance::CrsExport);
    EXPECT_EQ(imported->source, Provenance::Relocation);
    EXPECT_EQ(pointed->source, Provenance::Relocation);
    EXPECT_EQ(pointed->instruction_set, arm::InstructionSet::Arm);
    EXPECT_EQ(FunctionAt(analysis, fixture.CodeAt(Unreached)), nullptr);
}

TEST(AnalyzeModule, WithoutTheImportNothingReachesTheImportedFunction) {
    const Fixture fixture;
    const ModuleAnalysis analysis = fixture.Analyze({});

    EXPECT_EQ(FunctionAt(analysis, fixture.CodeAt(Imported)), nullptr);
    EXPECT_EQ(analysis.report.imported_positions, 0U);
}

TEST(AnalyzeModule, RecordsARelocatedWordAsACodePointer) {
    const Fixture fixture;
    const ModuleAnalysis analysis = fixture.Analyze(ImportedByAnother());

    ASSERT_EQ(analysis.pointers.size(), 1U);
    EXPECT_EQ(analysis.pointers[0].address, fixture.CodeAt(RelocatedWord));
    EXPECT_EQ(analysis.pointers[0].target, fixture.CodeAt(PointedAt));
    EXPECT_EQ(analysis.pointers[0].source, Provenance::Relocation);
    EXPECT_EQ(analysis.report.relocated_code_pointers, 1U);
    EXPECT_EQ(analysis.report.imported_words, 1U);
}

TEST(AnalyzeModule, ReachesNeitherTheUnreachedCodeNorPastTheSegment) {
    const Fixture fixture;
    const ModuleAnalysis analysis = fixture.Analyze(ImportedByAnother());

    EXPECT_EQ(analysis.report.code_bytes, CodeSize);
    // Everything before the unreached code is reached except the relocated word,
    // which is data sitting in the code.
    EXPECT_EQ(analysis.report.code_reached, Unreached - sizeof(std::uint32_t));
    for (const Region& region : analysis.regions) {
        EXPECT_LE(region.address + region.size, fixture.CodeAt(CodeSize));
    }
}

TEST(AnalysisPlacement, KeepsDataInPlaceAndPutsTheBssPastTheFile) {
    const Fixture fixture;
    const linker::Placement placement = AnalysisPlacement(fixture.module.header);
    const ctr::CroHeader& header = fixture.module.header;

    EXPECT_EQ(placement.module_address, 0U);
    EXPECT_EQ(placement.data_address, header.segments.at(1).offset);
    EXPECT_GE(placement.bss_address, header.declared_file_size);
    EXPECT_EQ(placement.bss_address % 0x1000, 0U);
}

}  // namespace
}  // namespace hexlith::domain::analysis
