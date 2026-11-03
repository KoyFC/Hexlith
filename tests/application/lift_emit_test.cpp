#include "application/lift_emit.h"
#include "core/sha256.h"
#include "domain/ctr/cro.h"

#include "support/memory_workspace.h"
#include "support/synthetic_cro.h"

#include <cstdint>
#include <format>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <vector>

namespace hexlith::application {
namespace {

using test::BuildSyntheticCro;
using test::SyntheticCroRelocation;
using test::SyntheticCroSegment;
using test::SyntheticCroSpec;

constexpr std::uint32_t Base = 0x00100000;

constexpr std::uint32_t ArmPush = 0xE92D4010;  // push {r4, lr}
constexpr std::uint32_t ArmMove = 0xE1A01002;  // mov r1, r2
constexpr std::uint32_t ArmPop = 0xE8BD8010;   // pop {r4, pc}

std::vector<std::uint8_t> Code(std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint8_t> bytes;
    for (const std::uint32_t word : words) {
        for (unsigned index = 0; index < 4; ++index) {
            bytes.push_back(static_cast<std::uint8_t>(word >> (8U * index)));
        }
    }
    return bytes;
}

constexpr std::string_view Functions = "format version=1\n"
                                       "0x00100000 size=0x0000000C isa=arm source=heuristic\n"
                                       "0x0010000C size=0x00000004 isa=arm source=heuristic\n";

constexpr std::string_view Regions =
    "format version=1\n"
    "0x00100000 size=0x00000010 kind=code isa=arm source=heuristic\n";

TEST(EmitLiftedCode, WritesTheProjectThatBuildsTheLibrary) {
    const std::vector<std::uint8_t> code = Code({ArmPush, ArmMove, ArmPop, ArmPop});
    test::MemoryBlobSink sink;
    const core::Result<LiftEmitReport> report =
        EmitLiftedCode(LiftEmitRequest{.code = code,
                                       .base = Base,
                                       .functions = Functions,
                                       .regions = Regions,
                                       .instructions_per_shard = 4000},
                       sink);
    ASSERT_TRUE(report.Ok()) << report.Failure().Message();

    EXPECT_EQ(report.Value().functions, 2U);
    EXPECT_EQ(report.Value().shards, 1U);
    EXPECT_EQ(report.Value().code_hash.size(), 64U);

    const std::string header = sink.Text("lifted.h").value_or("");
    EXPECT_NE(header.find("fn_00100000"), std::string::npos);
    EXPECT_NE(header.find("fn_0010000C"), std::string::npos);

    const std::string shard = sink.Text("shard_0000.cpp").value_or("");
    EXPECT_NE(shard.find("bb_00100000:"), std::string::npos);
    EXPECT_NE(shard.find("table.Add(0x00100000U, false, &fn_00100000);"), std::string::npos);

    const std::string registry = sink.Text("registry.cpp").value_or("");
    EXPECT_NE(registry.find("hexlith_lifted_manifest"), std::string::npos);
    EXPECT_NE(registry.find(report.Value().code_hash), std::string::npos);
    EXPECT_NE(registry.find("hexlith_lifted_register"), std::string::npos);

    const std::string cmake = sink.Text("CMakeLists.txt").value_or("");
    EXPECT_NE(cmake.find("shard_0000.cpp"), std::string::npos);
    EXPECT_NE(cmake.find("-ffp-contract=off"), std::string::npos);
}

TEST(EmitLiftedCode, StartsANewShardWhenOneHoldsEnough) {
    const std::vector<std::uint8_t> code = Code({ArmPush, ArmMove, ArmPop, ArmPop});
    test::MemoryBlobSink sink;
    const core::Result<LiftEmitReport> report =
        EmitLiftedCode(LiftEmitRequest{.code = code,
                                       .base = Base,
                                       .functions = Functions,
                                       .regions = Regions,
                                       .instructions_per_shard = 2},
                       sink);
    ASSERT_TRUE(report.Ok());
    EXPECT_EQ(report.Value().shards, 2U);
    EXPECT_TRUE(sink.Text("shard_0001.cpp").has_value());
    EXPECT_NE(sink.Text("registry.cpp").value_or("").find("register_shard_0001"),
              std::string::npos);
}

TEST(EmitLiftedCode, SaysWhichCodeItWasLiftedFrom) {
    const std::vector<std::uint8_t> one = Code({ArmPush, ArmPop});
    const std::vector<std::uint8_t> other = Code({ArmPush, ArmMove});
    test::MemoryBlobSink first_sink;
    test::MemoryBlobSink second_sink;
    const LiftEmitRequest request{.code = one,
                                  .base = Base,
                                  .functions =
                                      "format version=1\n"
                                      "0x00100000 size=0x00000008 isa=arm source=heuristic\n",
                                  .regions = Regions};
    LiftEmitRequest changed = request;
    changed.code = other;
    const auto first = EmitLiftedCode(request, first_sink);
    const auto second = EmitLiftedCode(changed, second_sink);
    ASSERT_TRUE(first.Ok());
    ASSERT_TRUE(second.Ok());
    EXPECT_NE(first.Value().code_hash, second.Value().code_hash);
}

TEST(EmitLiftedCode, RefusesTablesItCannotRead) {
    const std::vector<std::uint8_t> code = Code({ArmPop});
    test::MemoryBlobSink sink;
    EXPECT_FALSE(
        EmitLiftedCode(
            LiftEmitRequest{.code = code, .base = Base, .functions = "x", .regions = Regions}, sink)
            .Ok());
}

// --- Modules -----------------------------------------------------------------

// A module of four instructions in a code segment, with a relocation on the word
// at `relocated_word` when there is one. The functions it declares are at file
// offsets, as the analysis writes a module's.
struct BuiltModule {
    std::vector<std::uint8_t> file;
    std::uint32_t code_offset = 0;
    std::string functions;
    std::string regions;
};

constexpr std::size_t NoRelocation = 99;

BuiltModule BuildModule(std::string name,
                        std::initializer_list<std::uint32_t> words,
                        std::size_t relocated_word = NoRelocation) {
    constexpr std::uint32_t WordSize = 4;
    const std::vector<std::uint8_t> code = Code(words);
    SyntheticCroSpec spec;
    spec.name = std::move(name);
    spec.segments = {
        SyntheticCroSegment{.type = domain::ctr::CroSegmentType::Code, .size = code.size()}};
    spec.segment_contents[0] = code;
    if (relocated_word != NoRelocation) {
        spec.internal_relocations = {SyntheticCroRelocation{
            .segment_index = 0,
            .offset_in_segment = static_cast<std::uint32_t>(relocated_word) * WordSize,
            .addend = 0,
            .symbol_segment = 0}};
    }
    BuiltModule built;
    built.file = BuildSyntheticCro(spec);
    const auto header = domain::ctr::ParseCroHeader(built.file);
    built.code_offset = static_cast<std::uint32_t>(header.Value().segments.at(0).offset);
    built.functions = std::format("format version=1\n0x{:08X} size=0x{:08X} isa=arm "
                                  "source=heuristic\n",
                                  built.code_offset,
                                  code.size());
    built.regions = std::format("format version=1\n0x{:08X} size=0x{:08X} kind=code isa=arm "
                                "source=heuristic\n",
                                built.code_offset,
                                code.size());
    return built;
}

ModuleSource SourceOf(const std::string& name, const BuiltModule& built) {
    return ModuleSource{
        .name = name, .file = built.file, .functions = built.functions, .regions = built.regions};
}

std::string CodeDigestOf(const BuiltModule& built, std::size_t size) {
    return core::Sha256::Of(
               std::span<const std::uint8_t>{built.file}.subspan(built.code_offset, size))
        .ToHex();
}

TEST(EmitLiftedModules, WritesEachModuleInANamespaceOfItsOwnAndSaysWhatItWasLiftedFrom) {
    const BuiltModule first = BuildModule("DllA", {ArmPush, ArmMove, ArmPop, ArmPop});
    const BuiltModule second = BuildModule("DllB", {ArmPush, ArmPop});
    const std::vector<ModuleSource> modules = {SourceOf("DllA", first), SourceOf("DllB", second)};
    test::MemoryBlobSink sink;
    const core::Result<LiftModulesReport> report = EmitLiftedModules(
        LiftModulesRequest{.modules = modules, .instructions_per_shard = 4000}, sink);
    ASSERT_TRUE(report.Ok()) << report.Failure().Message();
    EXPECT_EQ(report.Value().totals.functions, 2U);
    ASSERT_EQ(report.Value().modules.size(), 2U);
    EXPECT_EQ(report.Value().modules.at(0).code_hash, CodeDigestOf(first, 16));
    EXPECT_EQ(report.Value().modules.at(1).code_hash, CodeDigestOf(second, 8));

    // The same offset in two modules is the same function name, so each is in a
    // namespace of its own.
    const std::string header = sink.Text("modules.h").value_or("");
    EXPECT_NE(header.find("namespace DllA {"), std::string::npos) << header;
    EXPECT_NE(header.find("namespace DllB {"), std::string::npos);
    const std::string shard = sink.Text("DllA_0000.cpp").value_or("");
    EXPECT_NE(shard.find("namespace lifted_modules::DllA {"), std::string::npos) << shard;
    EXPECT_NE(shard.find(std::format("table.Add(0x{:08X}U, false, &fn_{:08X});",
                                     first.code_offset,
                                     first.code_offset)),
              std::string::npos);
    EXPECT_TRUE(sink.Text("DllB_0000.cpp").has_value());

    const std::string registry = sink.Text("registry.cpp").value_or("");
    EXPECT_NE(registry.find("hexlith_modules_manifest"), std::string::npos);
    EXPECT_NE(registry.find("hexlith_modules_register"), std::string::npos);
    EXPECT_NE(registry.find(CodeDigestOf(first, 16)), std::string::npos);
    EXPECT_NE(registry.find(CodeDigestOf(second, 8)), std::string::npos);
    EXPECT_NE(registry.find("\"DllA\""), std::string::npos);

    const std::string manifest = sink.Text("manifest.txt").value_or("");
    EXPECT_NE(manifest.find(std::format("lifter={}", LifterVersion)), std::string::npos);
    EXPECT_NE(manifest.find(std::format("DllA code={}", CodeDigestOf(first, 16))),
              std::string::npos)
        << manifest;
    EXPECT_NE(manifest.find("DllB code="), std::string::npos);

    const std::string cmake = sink.Text("CMakeLists.txt").value_or("");
    EXPECT_NE(cmake.find("hexlith_modules"), std::string::npos);
    EXPECT_NE(cmake.find("DllB_0000.cpp"), std::string::npos);
}

TEST(EmitLiftedModules, LiftsAtOffsetsAndLeavesARelocatedWordToTheInterpreter) {
    const BuiltModule built = BuildModule("DllA", {ArmPush, ArmMove, ArmPop, ArmPop}, 1);
    const std::vector<ModuleSource> modules = {SourceOf("DllA", built)};
    test::MemoryBlobSink sink;
    const auto report = EmitLiftedModules(
        LiftModulesRequest{.modules = modules, .instructions_per_shard = 4000}, sink);
    ASSERT_TRUE(report.Ok()) << report.Failure().Message();
    EXPECT_EQ(report.Value().modules.at(0).relocated_words, 1U);

    const std::string shard = sink.Text("DllA_0000.cpp").value_or("");
    EXPECT_NE(shard.find("switch (pc - c.base)"), std::string::npos) << shard;
    EXPECT_NE(shard.find(std::format("return runtime::Bail(c, (c.base + 0x{:08X}U), 2);",
                                     built.code_offset + 4)),
              std::string::npos)
        << shard;
}

TEST(EmitLiftedModules, StartsANewShardInAModuleWhenOneHoldsEnough) {
    const BuiltModule built = BuildModule("DllA", {ArmPush, ArmMove, ArmPop, ArmPop});
    const std::string two_functions =
        std::format("format version=1\n0x{:08X} size=0x0000000C isa=arm source=heuristic\n"
                    "0x{:08X} size=0x00000004 isa=arm source=heuristic\n",
                    built.code_offset,
                    built.code_offset + 12);
    BuiltModule split = built;
    split.functions = two_functions;
    const std::vector<ModuleSource> modules = {SourceOf("DllA", split)};
    test::MemoryBlobSink sink;
    const auto report = EmitLiftedModules(
        LiftModulesRequest{.modules = modules, .instructions_per_shard = 2}, sink);
    ASSERT_TRUE(report.Ok()) << report.Failure().Message();
    EXPECT_EQ(report.Value().totals.shards, 2U);
    EXPECT_TRUE(sink.Text("DllA_0001.cpp").has_value());
    EXPECT_NE(sink.Text("registry.cpp").value_or("").find("register_DllA_0001"), std::string::npos);
}

TEST(EmitLiftedModules, RefusesAFileThatIsNotAModuleAndANameThatIsNotAnIdentifier) {
    const BuiltModule built = BuildModule("DllA", {ArmPop});
    test::MemoryBlobSink sink;

    const std::vector<std::uint8_t> junk(64, 0x41);
    const std::vector<ModuleSource> bad_file = {ModuleSource{
        .name = "DllA", .file = junk, .functions = built.functions, .regions = built.regions}};
    EXPECT_FALSE(EmitLiftedModules(
                     LiftModulesRequest{.modules = bad_file, .instructions_per_shard = 4000}, sink)
                     .Ok());

    const std::vector<ModuleSource> bad_name = {SourceOf("Dll-A", built)};
    EXPECT_FALSE(EmitLiftedModules(
                     LiftModulesRequest{.modules = bad_name, .instructions_per_shard = 4000}, sink)
                     .Ok());
}

}  // namespace
}  // namespace hexlith::application
