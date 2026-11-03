#include "application/executed_table.h"
#include "domain/analysis/record_text.h"

#include <cstdint>
#include <gtest/gtest.h>

namespace hexlith::application {
namespace {

namespace analysis = domain::analysis;

constexpr std::uint32_t TextStart = 0x00100000;
constexpr std::uint32_t TextEnd = 0x00200000;

TEST(RenderExecutedTable, WritesOneRecordPerExecutedAddress) {
    domain::arm::ExecutedAddresses executed;
    executed.BeforeInstruction(TextStart, false);
    executed.BeforeInstruction(TextStart + 4, false);
    executed.BeforeInstruction(TextStart + 4, false);
    executed.BeforeInstruction(TextStart + 8, true);

    const core::Result<analysis::RecordFile> parsed =
        analysis::ParseRecordText(RenderExecutedTable(executed, TextStart, TextEnd));
    ASSERT_TRUE(parsed.Ok()) << parsed.Failure().Message();
    ASSERT_EQ(parsed.Value().records.size(), 3U);

    const analysis::Record& second = parsed.Value().records.at(1);
    EXPECT_EQ(second.address, TextStart + 4);
    ASSERT_NE(second.Find("isa"), nullptr);
    EXPECT_EQ(*second.Find("isa"), "arm");
    ASSERT_NE(second.Find("count"), nullptr);
    EXPECT_EQ(*second.Find("count"), "2");
    EXPECT_EQ(*parsed.Value().records.at(2).Find("isa"), "thumb");
}

TEST(RenderExecutedTable, LeavesOutWhatIsOutsideTheRange) {
    domain::arm::ExecutedAddresses executed;
    executed.BeforeInstruction(TextStart - 4, false);
    executed.BeforeInstruction(TextStart, false);
    executed.BeforeInstruction(TextEnd, false);

    const core::Result<analysis::RecordFile> parsed =
        analysis::ParseRecordText(RenderExecutedTable(executed, TextStart, TextEnd));
    ASSERT_TRUE(parsed.Ok());
    ASSERT_EQ(parsed.Value().records.size(), 1U);
    EXPECT_EQ(parsed.Value().records.front().address, TextStart);
}

TEST(RenderExecutedTable, IsTheSameTextForTheSameRun) {
    domain::arm::ExecutedAddresses first;
    domain::arm::ExecutedAddresses second;
    for (const std::uint32_t address : {TextStart + 8, TextStart, TextStart + 4}) {
        first.BeforeInstruction(address, false);
    }
    for (const std::uint32_t address : {TextStart, TextStart + 4, TextStart + 8}) {
        second.BeforeInstruction(address, false);
    }
    EXPECT_EQ(RenderExecutedTable(first, TextStart, TextEnd),
              RenderExecutedTable(second, TextStart, TextEnd));
}

// --- By module ---------------------------------------------------------------

constexpr std::uint32_t FirstBase = 0x006A5000;
constexpr std::uint32_t SecondBase = 0x007E5000;
constexpr std::uint32_t CodeOffset = 0x180;
constexpr std::uint32_t CodeSize = 0x1000;
constexpr std::uint32_t ThirdWord = 12;

TEST(ExecutionRecorder, RecordsAModulesInstructionsByOffsetFromItsStart) {
    ExecutionRecorder recorder;
    recorder.ModuleLoaded("DllA", FirstBase, CodeOffset, CodeSize);
    recorder.BeforeInstruction(FirstBase + CodeOffset + 8, false);
    recorder.BeforeInstruction(FirstBase + CodeOffset + 8, false);
    recorder.BeforeInstruction(TextStart, false);

    EXPECT_EQ(recorder.Main().CountOf(TextStart, false), 1U);
    EXPECT_EQ(recorder.Main().Total(), 1U) << "the module's instructions are not the executable's";
    ASSERT_EQ(recorder.Modules().size(), 1U);
    const ExecutionRecorder::ModuleRecord& record = recorder.Modules().at("DllA");
    EXPECT_EQ(record.executed.CountOf(CodeOffset + 8, false), 2U);
    EXPECT_EQ(record.code_offset, CodeOffset);
    EXPECT_EQ(record.code_size, CodeSize);
}

TEST(ExecutionRecorder, AModuleLoadedAtTwoAddressesIsOneRecord) {
    ExecutionRecorder recorder;
    recorder.ModuleLoaded("DllA", FirstBase, CodeOffset, CodeSize);
    recorder.BeforeInstruction(FirstBase + CodeOffset + 4, false);
    recorder.ModuleUnloaded("DllA", FirstBase);

    recorder.ModuleLoaded("DllA", SecondBase, CodeOffset, CodeSize);
    recorder.BeforeInstruction(SecondBase + CodeOffset + 4, false);
    recorder.BeforeInstruction(SecondBase + CodeOffset + ThirdWord, false);

    ASSERT_EQ(recorder.Modules().size(), 1U);
    const auto& executed = recorder.Modules().at("DllA").executed;
    EXPECT_EQ(executed.CountOf(CodeOffset + 4, false), 2U) << "the same offset, from both loads";
    EXPECT_EQ(executed.CountOf(CodeOffset + ThirdWord, false), 1U);
    EXPECT_EQ(executed.Total(), 3U);
}

TEST(ExecutionRecorder, WhatRanWhereNoModuleIsLoadedBelongsToTheExecutable) {
    ExecutionRecorder recorder;
    recorder.ModuleLoaded("DllA", FirstBase, CodeOffset, CodeSize);
    recorder.ModuleUnloaded("DllA", FirstBase);
    recorder.BeforeInstruction(FirstBase + CodeOffset, false);
    EXPECT_EQ(recorder.Main().CountOf(FirstBase + CodeOffset, false), 1U);
    EXPECT_EQ(recorder.Modules().at("DllA").executed.Total(), 0U);
}

TEST(ExecutionRecorder, OnlyTheCodeSegmentOfAModuleIsItsCode) {
    ExecutionRecorder recorder;
    recorder.ModuleLoaded("DllA", FirstBase, CodeOffset, CodeSize);
    // Inside the module's pages but past its code, as a stack or a buffer a
    // module is loaded next to would be.
    recorder.BeforeInstruction(FirstBase + CodeOffset + CodeSize, false);
    recorder.BeforeInstruction(FirstBase + CodeOffset - 4, false);
    EXPECT_EQ(recorder.Modules().at("DllA").executed.Total(), 0U);
    EXPECT_EQ(recorder.Main().Total(), 2U);
}

TEST(RenderExecutedTable, ForAModuleIsInOffsetsAndInItsCodeSegmentOnly) {
    ExecutionRecorder recorder;
    recorder.ModuleLoaded("DllA", FirstBase, CodeOffset, CodeSize);
    recorder.BeforeInstruction(FirstBase + CodeOffset, false);
    recorder.BeforeInstruction(FirstBase + CodeOffset + 4, true);

    const ExecutionRecorder::ModuleRecord& record = recorder.Modules().at("DllA");
    const core::Result<analysis::RecordFile> parsed = analysis::ParseRecordText(RenderExecutedTable(
        record.executed, record.code_offset, record.code_offset + record.code_size));
    ASSERT_TRUE(parsed.Ok()) << parsed.Failure().Message();
    ASSERT_EQ(parsed.Value().records.size(), 2U);
    EXPECT_EQ(parsed.Value().records.at(0).address, CodeOffset);
    EXPECT_EQ(parsed.Value().records.at(1).address, CodeOffset + 4);
}

}  // namespace
}  // namespace hexlith::application
