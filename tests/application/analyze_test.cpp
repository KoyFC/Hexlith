#include "application/analyze.h"

#include "support/memory_workspace.h"
#include "support/synthetic_cro.h"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::application {
namespace {

namespace analysis = domain::analysis;
namespace ctr = domain::ctr;
namespace profile = domain::profile;

using test::BuildSyntheticCro;
using test::MemoryBlobSink;
using test::SyntheticCroExport;
using test::SyntheticCroSegment;
using test::SyntheticCroSpec;

// A profile shaped like the real one and small enough to assemble by hand. The
// three segments sit contiguously, as they do in the decompressed code image.
// The program identity does not matter here, only that it is one.
constexpr std::uint64_t TestProgramId = 0x0004000000055D00;

constexpr std::uint32_t TextAddress = 0x00100000;
constexpr std::uint32_t TextSize = 0x40;
constexpr std::uint32_t ReadOnlyAddress = TextAddress + TextSize;
constexpr std::uint32_t ReadOnlySize = 0x10;
constexpr std::uint32_t DataAddress = ReadOnlyAddress + ReadOnlySize;
constexpr std::uint32_t DataSize = 0x10;

constexpr std::uint32_t ArmPush = 0xE92D4010;    // push {r4, lr}
constexpr std::uint32_t ArmMove = 0xE1A01002;    // mov r1, r2
constexpr std::uint32_t ArmPop = 0xE8BD8010;     // pop {r4, pc}
constexpr std::uint32_t ArmReturn = 0xE12FFF1E;  // bx lr

profile::GameProfile TestProfile() {
    profile::GameProfile chosen;
    chosen.name = "Test";
    chosen.program_id = TestProgramId;
    chosen.text = profile::SegmentLayout{.address = TextAddress, .size = TextSize};
    chosen.read_only = profile::SegmentLayout{.address = ReadOnlyAddress, .size = ReadOnlySize};
    chosen.data = profile::SegmentLayout{.address = DataAddress, .size = DataSize};
    return chosen;
}

// The code image: a function at the start of the text segment, a second one after
// it, and a word in read-only data pointing at the second so that the sweep has
// something to find.
std::vector<std::uint8_t> CodeImage() {
    std::vector<std::uint8_t> bytes;
    const auto word = [&bytes](std::uint32_t value) {
        for (unsigned index = 0; index < 4; ++index) {
            bytes.push_back(static_cast<std::uint8_t>(value >> (8U * index)));
        }
    };

    word(ArmPush);  // 0x00100000, the entry point
    word(ArmMove);
    word(ArmPop);
    word(ArmReturn);  // 0x0010000C, unreachable
    word(ArmPush);    // 0x00100010, only the sweep finds this one
    word(ArmPop);

    bytes.resize(TextSize, 0);
    word(TextAddress + 0x10);  // in read-only data, pointing at the second function
    bytes.resize(TextSize + ReadOnlySize + DataSize, 0);
    return bytes;
}

// The static module, whose segment offsets are the virtual addresses of the
// segments above, exporting one Thumb function and one word of data.
std::vector<std::uint8_t> StaticModule() {
    SyntheticCroSpec spec;
    spec.static_module = true;
    spec.segments = {
        SyntheticCroSegment{
            .type = ctr::CroSegmentType::Code, .size = TextSize, .virtual_address = TextAddress},
        SyntheticCroSegment{.type = ctr::CroSegmentType::ReadOnlyData,
                            .size = ReadOnlySize,
                            .virtual_address = ReadOnlyAddress},
    };
    spec.named_exports = {
        SyntheticCroExport{.name = "TheEntryPoint", .segment_index = 0, .offset_in_segment = 0},
        SyntheticCroExport{.name = "SomeTable", .segment_index = 1, .offset_in_segment = 0},
    };
    spec.counts.named_exports = spec.named_exports.size();
    return BuildSyntheticCro(spec);
}

AnalyzeRequest Request(const std::vector<std::uint8_t>& code,
                       const std::vector<std::uint8_t>& module,
                       const profile::GameProfile& chosen) {
    return AnalyzeRequest{.code = code,
                          .static_module = module,
                          .modules = {},
                          .profile = &chosen,
                          .sweep_data_for_entry_points = true};
}

TEST(Analyze, WritesTheFourTables) {
    const std::vector<std::uint8_t> code = CodeImage();
    const std::vector<std::uint8_t> module = StaticModule();
    const profile::GameProfile chosen = TestProfile();

    MemoryBlobSink sink;
    ASSERT_TRUE(Analyze(Request(code, module, chosen), ExistingTables{}, sink).Ok());

    EXPECT_TRUE(sink.Holds(analysis::SymbolsFileName));
    EXPECT_TRUE(sink.Holds(analysis::FunctionsFileName));
    EXPECT_TRUE(sink.Holds(analysis::RegionsFileName));
    EXPECT_TRUE(sink.Holds(analysis::PointersFileName));
}

TEST(Analyze, CountsWhatTheExportTableGaveIt) {
    const std::vector<std::uint8_t> code = CodeImage();
    const std::vector<std::uint8_t> module = StaticModule();
    const profile::GameProfile chosen = TestProfile();

    MemoryBlobSink sink;
    const core::Result<AnalyzeReport> report =
        Analyze(Request(code, module, chosen), ExistingTables{}, sink);
    ASSERT_TRUE(report.Ok()) << report.Failure().Message();

    EXPECT_EQ(report.Value().exported_symbols, 2U);
    EXPECT_EQ(report.Value().exported_functions, 1U) << "only the one in the code segment";
    EXPECT_GT(report.Value().code_bytes, 0U);
    EXPECT_EQ(report.Value().text_bytes, TextSize);
}

// The entry point is exported and nothing in the image calls it, so only being a
// seed makes it a function. Before the seeds were measured too, an exported
// function nobody called had no record at all.
TEST(Analyze, RecordsAFunctionAtEverySeedEvenIfNothingCallsIt) {
    const std::vector<std::uint8_t> code = CodeImage();
    const std::vector<std::uint8_t> module = StaticModule();
    const profile::GameProfile chosen = TestProfile();

    MemoryBlobSink sink;
    ASSERT_TRUE(Analyze(Request(code, module, chosen), ExistingTables{}, sink).Ok());

    const core::Result<std::vector<analysis::FunctionRange>> functions =
        analysis::ParseFunctions(sink.Text(analysis::FunctionsFileName).value_or(""));
    ASSERT_TRUE(functions.Ok()) << functions.Failure().Message();
    ASSERT_FALSE(functions.Value().empty());

    const analysis::FunctionRange& first = functions.Value().front();
    EXPECT_EQ(first.address, TextAddress);
    EXPECT_GT(first.size, 0U);
    EXPECT_EQ(first.source, analysis::Provenance::ExHeader);
}

// The word at 0x0010000C is a return nothing in the image reaches. A module
// that imports that position makes it an entry point.
TEST(Analyze, SeedsTheMainExecutableWithWhatTheModulesImportFromIt) {
    constexpr std::uint32_t ImportedReturn = 0xC;
    constexpr std::size_t ModuleCodeSize = 0x10;

    const std::vector<std::uint8_t> code = CodeImage();
    const std::vector<std::uint8_t> static_module = StaticModule();
    const profile::GameProfile chosen = TestProfile();

    SyntheticCroSpec spec;
    spec.name = "DllImporter";
    spec.segments = {
        SyntheticCroSegment{.type = ctr::CroSegmentType::Code, .size = ModuleCodeSize}};
    spec.import_modules = {test::ImportsFrom(
        "|static|",
        {test::AnonymousImport(
            0, ImportedReturn, {test::SyntheticCroRelocation{.segment_index = 0}})})};
    const std::vector<std::uint8_t> importer = BuildSyntheticCro(spec);
    const std::vector<core::ByteSpan> modules = {core::ByteSpan{importer}};

    AnalyzeRequest request = Request(code, static_module, chosen);
    request.modules = modules;

    MemoryBlobSink sink;
    const core::Result<AnalyzeReport> report = Analyze(request, ExistingTables{}, sink);
    ASSERT_TRUE(report.Ok()) << report.Failure().Message();
    EXPECT_EQ(report.Value().imported_functions, 1U);

    const core::Result<std::vector<analysis::FunctionRange>> functions =
        analysis::ParseFunctions(sink.Text(analysis::FunctionsFileName).value_or(""));
    ASSERT_TRUE(functions.Ok()) << functions.Failure().Message();
    const bool found = std::ranges::any_of(functions.Value(), [](const auto& function) {
        return function.address == TextAddress + ImportedReturn &&
               function.source == analysis::Provenance::Relocation;
    });
    EXPECT_TRUE(found);
}

TEST(Analyze, ReachesWhatOnlyTheSweepCanFind) {
    const std::vector<std::uint8_t> code = CodeImage();
    const std::vector<std::uint8_t> module = StaticModule();
    const profile::GameProfile chosen = TestProfile();

    MemoryBlobSink with_sweep;
    const core::Result<AnalyzeReport> swept =
        Analyze(Request(code, module, chosen), ExistingTables{}, with_sweep);
    ASSERT_TRUE(swept.Ok()) << swept.Failure().Message();

    AnalyzeRequest without = Request(code, module, chosen);
    without.sweep_data_for_entry_points = false;
    MemoryBlobSink no_sweep;
    const core::Result<AnalyzeReport> unswept = Analyze(without, ExistingTables{}, no_sweep);
    ASSERT_TRUE(unswept.Ok()) << unswept.Failure().Message();

    EXPECT_GT(swept.Value().code_bytes, unswept.Value().code_bytes);
    EXPECT_GT(swept.Value().tables.functions, unswept.Value().tables.functions);
    EXPECT_EQ(unswept.Value().tables.pointers, 0U);
}

// A run fed its own output, which is what a second run looks like.
ExistingTables OutputOf(const MemoryBlobSink& sink) {
    return ExistingTables{
        .symbols = sink.Text(analysis::SymbolsFileName).value_or(""),
        .functions = sink.Text(analysis::FunctionsFileName).value_or(""),
        .regions = sink.Text(analysis::RegionsFileName).value_or(""),
        .pointers = sink.Text(analysis::PointersFileName).value_or(""),
    };
}

TEST(Analyze, WritesTheSameBytesTwice) {
    const std::vector<std::uint8_t> code = CodeImage();
    const std::vector<std::uint8_t> module = StaticModule();
    const profile::GameProfile chosen = TestProfile();

    MemoryBlobSink first;
    ASSERT_TRUE(Analyze(Request(code, module, chosen), ExistingTables{}, first).Ok());

    MemoryBlobSink second;
    ASSERT_TRUE(Analyze(Request(code, module, chosen), OutputOf(first), second).Ok());

    EXPECT_EQ(second.Text(analysis::SymbolsFileName), first.Text(analysis::SymbolsFileName));
    EXPECT_EQ(second.Text(analysis::FunctionsFileName), first.Text(analysis::FunctionsFileName));
    EXPECT_EQ(second.Text(analysis::RegionsFileName), first.Text(analysis::RegionsFileName));
    EXPECT_EQ(second.Text(analysis::PointersFileName), first.Text(analysis::PointersFileName));
}

TEST(Analyze, KeepsANameAPersonGave) {
    const std::vector<std::uint8_t> code = CodeImage();
    const std::vector<std::uint8_t> module = StaticModule();
    const profile::GameProfile chosen = TestProfile();

    const ExistingTables existing{
        .symbols =
            analysis::RenderSymbols({analysis::Symbol{.address = TextAddress,
                                                      .size = analysis::UnknownSize,
                                                      .kind = analysis::SymbolKind::Function,
                                                      .name = "SomethingAPersonWorkedOut",
                                                      .source = analysis::Provenance::Manual}}),
        .functions = {},
        .regions = {},
        .pointers = {},
    };

    MemoryBlobSink sink;
    const core::Result<AnalyzeReport> report =
        Analyze(Request(code, module, chosen), existing, sink);
    ASSERT_TRUE(report.Ok()) << report.Failure().Message();

    EXPECT_EQ(report.Value().tables.merged_symbols.preserved, 1U);
    EXPECT_EQ(report.Value().tables.merged_symbols.suppressed, 1U)
        << "the exported name for that address gives way to the one a person chose";

    const std::string written = sink.Text(analysis::SymbolsFileName).value_or("");
    EXPECT_NE(written.find("SomethingAPersonWorkedOut"), std::string::npos);
    EXPECT_EQ(written.find("TheEntryPoint"), std::string::npos);
}

TEST(Analyze, RefusesToOverwriteATableItCannotRead) {
    const std::vector<std::uint8_t> code = CodeImage();
    const std::vector<std::uint8_t> module = StaticModule();
    const profile::GameProfile chosen = TestProfile();

    const ExistingTables existing{
        .symbols = "format version=1\n0x00100000 this is not a record\n",
        .functions = {},
        .regions = {},
        .pointers = {},
    };

    MemoryBlobSink sink;
    const core::Result<AnalyzeReport> report =
        Analyze(Request(code, module, chosen), existing, sink);

    ASSERT_FALSE(report.Ok());
    EXPECT_NE(report.Failure().Message().find("symbols.txt"), std::string::npos);
    EXPECT_FALSE(sink.Text(analysis::SymbolsFileName).has_value())
        << "the work in a file that does not parse is the only copy of it";
}

TEST(Analyze, RefusesWhatItCannotWorkWith) {
    const std::vector<std::uint8_t> code = CodeImage();
    const std::vector<std::uint8_t> module = StaticModule();
    const profile::GameProfile chosen = TestProfile();

    MemoryBlobSink sink;

    AnalyzeRequest without_a_profile = Request(code, module, chosen);
    without_a_profile.profile = nullptr;
    EXPECT_FALSE(Analyze(without_a_profile, ExistingTables{}, sink).Ok());

    // A loadable module in place of the static one, whose segment offsets would be
    // file offsets rather than addresses.
    const std::vector<std::uint8_t> loadable = BuildSyntheticCro(test::DefaultCroSpec());
    const AnalyzeRequest wrong_module = Request(code, loadable, chosen);
    const core::Result<AnalyzeReport> report = Analyze(wrong_module, ExistingTables{}, sink);
    ASSERT_FALSE(report.Ok());
    EXPECT_NE(report.Failure().Message().find("not the static one"), std::string::npos);

    // An image shorter than the segment the profile describes.
    const std::vector<std::uint8_t> truncated(TextSize / 2, 0);
    const AnalyzeRequest short_image = Request(truncated, module, chosen);
    EXPECT_FALSE(Analyze(short_image, ExistingTables{}, sink).Ok());
}

TEST(RenderAnalyzeSummary, NamesTheNumbersWorthWatching) {
    // The figures the real run produced, so that the summary is checked against
    // something recognisable rather than against round numbers.
    constexpr std::size_t ExportedSymbols = 461;
    constexpr std::uint32_t TextBytes = 100;
    constexpr std::uint32_t CodeBytes = 59;
    constexpr std::size_t Functions = 11824;

    AnalyzeReport report;
    report.exported_symbols = ExportedSymbols;
    report.text_bytes = TextBytes;
    report.code_bytes = CodeBytes;
    report.bounds.functions = Functions;

    const std::string summary = RenderAnalyzeSummary(report);
    EXPECT_NE(summary.find("461"), std::string::npos);
    EXPECT_NE(summary.find("59.0%"), std::string::npos);
    EXPECT_NE(summary.find("11824"), std::string::npos);
}

}  // namespace
}  // namespace hexlith::application
