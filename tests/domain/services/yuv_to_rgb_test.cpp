#include "domain/services/yuv_to_rgb.h"

#include "service_harness.h"

#include <cstdint>
#include <gtest/gtest.h>

namespace hexlith::domain::services {
namespace {

using testing::ServiceHarness;

// y2r:u command headers. Source: 3dbrew, Y2R Services.
constexpr std::uint32_t SetOutputFormat = 0x00030040;
constexpr std::uint32_t GetOutputFormat = 0x00040000;
constexpr std::uint32_t GetTransferEndEvent = 0x000F0000;
constexpr std::uint32_t SetInputLineWidth = 0x001A0040;
constexpr std::uint32_t GetInputLineWidth = 0x001B0000;
constexpr std::uint32_t SetInputLines = 0x001C0040;
constexpr std::uint32_t GetInputLines = 0x001D0000;
constexpr std::uint32_t SetStandardCoefficient = 0x00200040;
constexpr std::uint32_t GetCoefficient = 0x001F0000;
constexpr std::uint32_t StartConversion = 0x00260000;
constexpr std::uint32_t IsBusyConversion = 0x00280000;
constexpr std::uint32_t SetPackageParameter = 0x002901C0;

TEST(YuvToRgb, AnswersBackWhatWasSet) {
    constexpr std::uint32_t Rgb565 = 3;
    ServiceHarness harness;
    YuvToRgb y2r(harness.Kernel());
    ASSERT_TRUE(harness.Send(y2r, {SetOutputFormat, Rgb565}));
    ASSERT_TRUE(harness.Send(y2r, {GetOutputFormat}));
    EXPECT_EQ(harness.Reply(2), Rgb565);
}

TEST(YuvToRgb, RefusesALineWidthTheHardwareCannotTake) {
    constexpr std::uint32_t Good = 400;
    constexpr std::uint32_t NotAMultipleOfEight = 401;
    ServiceHarness harness;
    YuvToRgb y2r(harness.Kernel());
    ASSERT_TRUE(harness.Send(y2r, {SetInputLineWidth, Good}));
    ASSERT_TRUE(harness.Send(y2r, {SetInputLineWidth, NotAMultipleOfEight}));
    EXPECT_EQ(harness.Reply(1), YuvToRgb::OutOfRange.raw);
    ASSERT_TRUE(harness.Send(y2r, {GetInputLineWidth}));
    EXPECT_EQ(harness.Reply(2), Good);
}

TEST(YuvToRgb, KeepsTheLastCountWhenAskedForAThousandAndTwentyFourLines) {
    constexpr std::uint32_t Lines = 240;
    constexpr std::uint32_t Maximum = 1024;
    ServiceHarness harness;
    YuvToRgb y2r(harness.Kernel());
    ASSERT_TRUE(harness.Send(y2r, {SetInputLines, Lines}));
    ASSERT_TRUE(harness.Send(y2r, {SetInputLines, Maximum}));
    EXPECT_EQ(harness.Reply(1), kernel::result::Success.raw);
    ASSERT_TRUE(harness.Send(y2r, {GetInputLines}));
    EXPECT_EQ(harness.Reply(2), Lines) << "the console's driver never writes 1024";
}

TEST(YuvToRgb, LoadsAStandardSetOfCoefficients) {
    ServiceHarness harness;
    YuvToRgb y2r(harness.Kernel());
    ASSERT_TRUE(harness.Send(y2r, {SetStandardCoefficient, 0}));
    ASSERT_TRUE(harness.Send(y2r, {GetCoefficient}));
    EXPECT_EQ(harness.Reply(2), 0x01660100U) << "ITU Rec. 601, the first two halfwords";
    ASSERT_TRUE(harness.Send(y2r, {SetStandardCoefficient, 4}));
    EXPECT_EQ(harness.Reply(1), YuvToRgb::InvalidEnumValue.raw);
}

TEST(YuvToRgb, TakesAPackageOfParameters) {
    constexpr std::uint32_t Width = 320;
    constexpr std::uint32_t Lines = 240;
    constexpr std::uint32_t Alpha = 0xFF;
    ServiceHarness harness;
    YuvToRgb y2r(harness.Kernel());
    ASSERT_TRUE(
        harness.Send(y2r, {SetPackageParameter, 0x00000300, Width | (Lines << 16U), Alpha << 16U}));
    EXPECT_EQ(harness.Reply(1), kernel::result::Success.raw);
    ASSERT_TRUE(harness.Send(y2r, {GetOutputFormat}));
    EXPECT_EQ(harness.Reply(2), 3U);
    ASSERT_TRUE(harness.Send(y2r, {GetInputLines}));
    EXPECT_EQ(harness.Reply(2), Lines);
}

TEST(YuvToRgb, EndsAConversionAfterItsDelay) {
    ServiceHarness harness;
    YuvToRgb y2r(harness.Kernel());
    ASSERT_TRUE(harness.Send(y2r, {GetTransferEndEvent}));
    const auto event = harness.ReplyObject<kernel::Event>(3, kernel::ObjectType::Event);
    ASSERT_NE(event, nullptr);

    ASSERT_TRUE(harness.Send(y2r, {StartConversion}));
    ASSERT_TRUE(harness.Send(y2r, {IsBusyConversion}));
    EXPECT_EQ(harness.Reply(2), 1U);
    harness.Kernel().Threads().Advance(YuvToRgb::ConversionTicks);
    EXPECT_FALSE(y2r.Busy());
    EXPECT_TRUE(event->signaled) << "the end of the transfer is signalled";
}

}  // namespace
}  // namespace hexlith::domain::services
