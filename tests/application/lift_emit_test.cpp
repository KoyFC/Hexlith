#include "application/lift_emit.h"

#include "support/memory_workspace.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::application {
namespace {

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

}  // namespace
}  // namespace hexlith::application
