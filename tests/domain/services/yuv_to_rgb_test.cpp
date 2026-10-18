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
constexpr std::uint32_t SetSendingY = 0x00100102;
constexpr std::uint32_t SetSendingU = 0x00110102;
constexpr std::uint32_t SetSendingV = 0x00120102;
constexpr std::uint32_t SetReceiving = 0x00180102;
constexpr std::uint32_t SetCoefficient = 0x001E0100;

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

// Names a buffer of the harness's page: its offset, its size, and its unit
// and gap.
void SetTransfer(ServiceHarness& harness,
                 YuvToRgb& y2r,
                 std::uint32_t header,
                 std::uint32_t offset,
                 std::uint32_t size,
                 std::uint32_t unit,
                 std::uint32_t gap) {
    ASSERT_TRUE(harness.Send(y2r,
                             {header,
                              ServiceHarness::BufferAddress + offset,
                              size,
                              unit,
                              gap,
                              kernel::ipc::CopyHandlesDescriptor(1),
                              0}));
}

// An 8x2 image whose red is its Y, from planes of 16, 8 and 8 bytes, to RGB8
// sent in units of one line, 24 bytes, with 8 bytes skipped after each.
constexpr std::uint32_t YOffset = 0x000;
constexpr std::uint32_t UOffset = 0x100;
constexpr std::uint32_t VOffset = 0x200;
constexpr std::uint32_t OutOffset = 0x300;
constexpr std::uint32_t Line = 24;
constexpr std::uint32_t Gap = 8;
constexpr std::uint8_t Untouched = 0xEE;
constexpr std::uint8_t FirstLuma = 0x40;

void SetUpLumaToRed(ServiceHarness& harness, YuvToRgb& y2r) {
    std::vector<std::uint8_t> luma(16);
    for (std::size_t x = 0; x < luma.size(); ++x) {
        luma[x] = static_cast<std::uint8_t>(FirstLuma + x);
    }
    harness.WriteBytes(ServiceHarness::BufferAddress + YOffset, luma);
    harness.WriteBytes(ServiceHarness::BufferAddress + OutOffset,
                       std::vector<std::uint8_t>(std::size_t{2} * (Line + Gap), Untouched));
    ASSERT_TRUE(harness.Send(y2r, {SetOutputFormat, 1}));
    ASSERT_TRUE(harness.Send(y2r, {SetInputLineWidth, 8}));
    ASSERT_TRUE(harness.Send(y2r, {SetInputLines, 2}));
    // Multipliers 0x100, 0, 0, 0, 0 and offsets of -0x18 for every channel.
    ASSERT_TRUE(harness.Send(y2r, {SetCoefficient, 0x100, 0, 0xFFE80000, 0xFFE8FFE8}));
    SetTransfer(harness, y2r, SetSendingY, YOffset, 16, 16, 0);
    SetTransfer(harness, y2r, SetSendingU, UOffset, 8, 8, 0);
    SetTransfer(harness, y2r, SetSendingV, VOffset, 8, 8, 0);
    SetTransfer(harness, y2r, SetReceiving, OutOffset, 2 * Line, Line, Gap);
}

TEST(YuvToRgb, ConvertsBetweenTheBuffersInUnitsWithGaps) {
    ServiceHarness harness;
    YuvToRgb y2r(harness.Kernel());
    SetUpLumaToRed(harness, y2r);
    ASSERT_TRUE(harness.Send(y2r, {StartConversion}));

    const std::vector<std::uint8_t> out =
        harness.ReadBytes(ServiceHarness::BufferAddress + OutOffset, 2 * (Line + Gap));
    EXPECT_EQ(out.at(2), FirstLuma) << "blue, green, red";
    EXPECT_EQ(out.at(Line - 1), FirstLuma + 7);
    EXPECT_EQ(out.at(Line), Untouched) << "the gap is skipped";
    EXPECT_EQ(out.at(Line + Gap + 2), FirstLuma + 8) << "the second line after it";
}

}  // namespace
}  // namespace hexlith::domain::services
