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

}  // namespace
}  // namespace hexlith::application
