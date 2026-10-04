#include "application/analyze_modules.h"

#include "support/memory_workspace.h"
#include "support/synthetic_cro.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::application {
namespace {

namespace analysis = domain::analysis;
namespace ctr = domain::ctr;

using test::AnonymousImport;
using test::BuildSyntheticCro;
using test::ImportsFrom;
using test::MemoryBlobSink;
using test::SyntheticCroExport;
using test::SyntheticCroRelocation;
using test::SyntheticCroSegment;
using test::SyntheticCroSpec;

constexpr std::uint32_t ArmPush = 0xE92D4010;    // push {r4, lr}
constexpr std::uint32_t ArmPop = 0xE8BD8010;     // pop {r4, pc}
constexpr std::uint32_t ArmReturn = 0xE12FFF1E;  // bx lr

// DllA exports a function at the start of its code and has a second one after
// it that nothing in DllA reaches. DllB imports that second one by position.
constexpr std::uint32_t ImportedFunction = 8;
constexpr std::uint32_t CodeSize = 0x10;

// The word in DllB that receives the address of the function it imports.
constexpr std::uint32_t ImportingWord = 0xC;
constexpr std::uint32_t StaticTextSize = 0x1000;
constexpr std::uint32_t StaticText = 0x00100000;

std::vector<std::uint8_t> CodeOfA() {
    std::vector<std::uint8_t> bytes;
    for (const std::uint32_t word : {ArmPush, ArmPop, ArmReturn, ArmReturn}) {
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

SyntheticCroSpec ModuleA() {
    SyntheticCroSpec spec;
    spec.name = "DllA";
    spec.segments = {SyntheticCroSegment{.type = ctr::CroSegmentType::Code, .size = CodeSize}};
    spec.segment_contents[0] = CodeOfA();
    spec.named_exports = {
        SyntheticCroExport{.name = "DllAMain", .segment_index = 0, .offset_in_segment = 0}};
    return spec;
}

SyntheticCroSpec ModuleB() {
    SyntheticCroSpec spec;
    spec.name = "DllB";
    spec.segments = {SyntheticCroSegment{.type = ctr::CroSegmentType::Code, .size = CodeSize}};
    spec.segment_contents[0] = CodeOfA();
    spec.import_modules = {ImportsFrom(
        "DllA",
        {AnonymousImport(
            0,
            ImportedFunction,
            {SyntheticCroRelocation{.segment_index = 0, .offset_in_segment = ImportingWord}})})};
    return spec;
}

struct Fixture {
    std::vector<std::uint8_t> static_file = BuildSyntheticCro(StaticSpec());
    std::vector<std::uint8_t> a = BuildSyntheticCro(ModuleA());
    std::vector<std::uint8_t> b = BuildSyntheticCro(ModuleB());
    std::vector<core::ByteSpan> modules{core::ByteSpan{a}, core::ByteSpan{b}};

    [[nodiscard]] AnalyzeModulesRequest Request() const {
        return AnalyzeModulesRequest{.static_module = static_file, .modules = modules};
    }

    [[nodiscard]] std::uint32_t CodeOfAAt(std::uint32_t offset) const {
        const core::Result<ctr::CroHeader> header = ctr::ParseCroHeader(core::ByteSpan{a});
        return static_cast<std::uint32_t>(header.Value().segments.at(0).offset) + offset;
    }
};

std::string TablePath(std::string_view module, std::string_view table) {
    return std::string{ModulesDirectory} + "/" + std::string{module} + "/" + std::string{table};
}

bool HoldsTheFourTables(const MemoryBlobSink& sink, std::string_view module) {
    return sink.Holds(TablePath(module, analysis::SymbolsFileName)) &&
           sink.Holds(TablePath(module, analysis::FunctionsFileName)) &&
           sink.Holds(TablePath(module, analysis::RegionsFileName)) &&
           sink.Holds(TablePath(module, analysis::PointersFileName));
}

TEST(AnalyzeModules, WritesEachModulesTablesInADirectoryOfItsOwn) {
    const Fixture fixture;
    MemoryBlobSink sink;
    const core::Result<AnalyzeModulesReport> report =
        AnalyzeModules(fixture.Request(), ExistingModuleTables{}, sink);
    ASSERT_TRUE(report.Ok()) << report.Failure().Message();

    EXPECT_TRUE(HoldsTheFourTables(sink, "DllA"));
    EXPECT_TRUE(HoldsTheFourTables(sink, "DllB"));
    ASSERT_EQ(report.Value().modules.size(), 2U);
    EXPECT_EQ(report.Value().code_bytes, 2U * CodeSize);
}

TEST(AnalyzeModules, SeedsAModuleWithWhatAnotherImportsFromIt) {
    const Fixture fixture;
    MemoryBlobSink sink;
    ASSERT_TRUE(AnalyzeModules(fixture.Request(), ExistingModuleTables{}, sink).Ok());

    const core::Result<std::vector<analysis::FunctionRange>> functions = analysis::ParseFunctions(
        sink.Text(TablePath("DllA", analysis::FunctionsFileName)).value_or(""));
    ASSERT_TRUE(functions.Ok()) << functions.Failure().Message();

    bool found = false;
    for (const analysis::FunctionRange& function : functions.Value()) {
        found = found || (function.address == fixture.CodeOfAAt(ImportedFunction) &&
                          function.source == analysis::Provenance::Relocation);
    }
    EXPECT_TRUE(found) << "the function DllB imports from DllA";
}

TEST(AnalyzeModules, KeepsARecordAPersonEnteredForAModule) {
    const Fixture fixture;
    ExistingModuleTables existing;
    existing["DllA"].symbols =
        analysis::RenderSymbols({analysis::Symbol{.address = fixture.CodeOfAAt(ImportedFunction),
                                                  .size = analysis::UnknownSize,
                                                  .kind = analysis::SymbolKind::Function,
                                                  .name = "SomethingAPersonWorkedOut",
                                                  .source = analysis::Provenance::Manual}});

    MemoryBlobSink sink;
    const core::Result<AnalyzeModulesReport> report =
        AnalyzeModules(fixture.Request(), existing, sink);
    ASSERT_TRUE(report.Ok()) << report.Failure().Message();

    EXPECT_EQ(report.Value().modules.at(0).tables.merged_symbols.preserved, 1U);
    EXPECT_NE(sink.Text(TablePath("DllA", analysis::SymbolsFileName))
                  .value_or("")
                  .find("SomethingAPersonWorkedOut"),
              std::string::npos);
}

TEST(AnalyzeModules, RegeneratesTheSameTables) {
    const Fixture fixture;
    MemoryBlobSink first;
    ASSERT_TRUE(AnalyzeModules(fixture.Request(), ExistingModuleTables{}, first).Ok());

    ExistingModuleTables existing;
    existing["DllA"] = ExistingTables{
        .symbols = first.Text(TablePath("DllA", analysis::SymbolsFileName)).value_or(""),
        .functions = first.Text(TablePath("DllA", analysis::FunctionsFileName)).value_or(""),
        .regions = first.Text(TablePath("DllA", analysis::RegionsFileName)).value_or(""),
        .pointers = first.Text(TablePath("DllA", analysis::PointersFileName)).value_or("")};

    MemoryBlobSink second;
    ASSERT_TRUE(AnalyzeModules(fixture.Request(), existing, second).Ok());
    for (const std::string_view table : {analysis::SymbolsFileName,
                                         analysis::FunctionsFileName,
                                         analysis::RegionsFileName,
                                         analysis::PointersFileName}) {
        EXPECT_EQ(second.Text(TablePath("DllA", table)), first.Text(TablePath("DllA", table)));
    }
}

TEST(RenderModulesReport, WritesALinePerModule) {
    const Fixture fixture;
    MemoryBlobSink sink;
    const core::Result<AnalyzeModulesReport> report =
        AnalyzeModules(fixture.Request(), ExistingModuleTables{}, sink);
    ASSERT_TRUE(report.Ok()) << report.Failure().Message();

    const std::string text = RenderModulesReport(report.Value());
    EXPECT_NE(text.find("\nDllA code="), std::string::npos);
    EXPECT_NE(text.find("\nDllB code="), std::string::npos);
}

}  // namespace
}  // namespace hexlith::application
