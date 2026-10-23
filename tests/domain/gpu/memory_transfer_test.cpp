#include "domain/gpu/gx_command.h"
#include "domain/gpu/memory_transfer.h"
#include "domain/gpu/pixel_formats.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::gpu {
namespace {

constexpr std::uint32_t Base = GpuMemory::VramBase;
constexpr std::uint32_t Output = GpuMemory::VramBase + 0x10000;

std::vector<std::uint8_t> Read(const GpuMemory& memory, std::uint32_t address, std::uint32_t size) {
    const std::span<const std::uint8_t> bytes = memory.Bytes(address, size);
    return {bytes.begin(), bytes.end()};
}

// GPU_MEMFILL_CNT: start, and the fill width in bits 8 and 9.
constexpr std::uint32_t Fill16 = 0x001;
constexpr std::uint32_t Fill24 = 0x101;
constexpr std::uint32_t Fill32 = 0x201;

TEST(MemoryFill, WritesThirtyTwoBitValuesAndStopsShortOfTheEnd) {
    GpuMemory memory;
    ASSERT_TRUE(RunMemoryFill(
        memory, {.start = Base, .end = Base + 10, .value = 0x11223344, .control = Fill32}));
    EXPECT_EQ(
        Read(memory, Base, 12),
        (std::vector<std::uint8_t>{0x44, 0x33, 0x22, 0x11, 0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0}));
}

TEST(MemoryFill, WritesTheLastTwentyFourBitValueWhole) {
    GpuMemory memory;
    ASSERT_TRUE(RunMemoryFill(
        memory, {.start = Base, .end = Base + 4, .value = 0x00CCBBAA, .control = Fill24}));
    EXPECT_EQ(Read(memory, Base, 7),
              (std::vector<std::uint8_t>{0xAA, 0xBB, 0xCC, 0xAA, 0xBB, 0xCC, 0}));
}

TEST(MemoryFill, WritesSixteenBitValues) {
    GpuMemory memory;
    ASSERT_TRUE(RunMemoryFill(
        memory, {.start = Base, .end = Base + 4, .value = 0xFFFF1234, .control = Fill16}));
    EXPECT_EQ(Read(memory, Base, 5), (std::vector<std::uint8_t>{0x34, 0x12, 0x34, 0x12, 0}));
}

TEST(MemoryFill, RefusesARangeOutsideGpuMemoryOrEmpty) {
    GpuMemory memory;
    EXPECT_FALSE(RunMemoryFill(
        memory, {.start = 0x10000000, .end = 0x10000010, .value = 1, .control = Fill32}));
    EXPECT_FALSE(
        RunMemoryFill(memory, {.start = Base + 16, .end = Base, .value = 1, .control = Fill32}));
}

// GPU_MEMCOPY_FLAGS: the input format in bits 8 to 10, the output's in 12
// to 14; 0 RGBA8, 1 RGB8.
constexpr std::uint32_t Rgba8ToRgb8 = 1U << 12;
constexpr std::uint32_t FlipVertically = 1U << 0;
constexpr std::uint32_t InputLinear = 1U << 1;
constexpr std::uint32_t TextureCopy = 1U << 3;
constexpr std::uint32_t ScaleBoth = 2U << 24;

constexpr std::uint32_t Size(std::uint32_t width, std::uint32_t height) {
    return width | (height << 16);
}

// A 16x8 tiled RGBA8 image whose pixel (x, y) is red x, green y.
GpuMemory TiledImage() {
    GpuMemory memory;
    std::vector<std::uint8_t> bytes(std::size_t{16} * 8 * 4);
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            EncodePixel(PixelFormat::Rgba8,
                        Color{.red = static_cast<std::uint8_t>(x),
                              .green = static_cast<std::uint8_t>(y),
                              .blue = 0,
                              .alpha = 0xFF},
                        std::span<std::uint8_t>(bytes).subspan(TiledOffset(x, y, 16, 4), 4));
        }
    }
    EXPECT_TRUE(memory.Write(Base, bytes));
    return memory;
}

Color LinearPixel(const GpuMemory& memory, std::uint32_t x, std::uint32_t y, std::uint32_t width) {
    return DecodePixel(PixelFormat::Rgb8, memory.Bytes(Output + (((y * width) + x) * 3), 3));
}

TEST(DisplayTransfer, UntilesConvertsAndCropsTheInputLines) {
    GpuMemory memory = TiledImage();
    const TransferConfig config{.input = Base,
                                .output = Output,
                                .input_size = Size(16, 8),
                                .output_size = Size(8, 8),
                                .flags = Rgba8ToRgb8};
    ASSERT_EQ(RunTransfer(memory, config), TransferOutcome::Done);
    EXPECT_EQ(LinearPixel(memory, 5, 3, 8),
              (Color{.red = 5, .green = 3, .blue = 0, .alpha = 0xFF}));
    EXPECT_EQ(LinearPixel(memory, 7, 7, 8),
              (Color{.red = 7, .green = 7, .blue = 0, .alpha = 0xFF}));
}

TEST(DisplayTransfer, FlipsTheRows) {
    GpuMemory memory = TiledImage();
    const TransferConfig config{.input = Base,
                                .output = Output,
                                .input_size = Size(16, 8),
                                .output_size = Size(16, 8),
                                .flags = Rgba8ToRgb8 | FlipVertically};
    ASSERT_EQ(RunTransfer(memory, config), TransferOutcome::Done);
    EXPECT_EQ(LinearPixel(memory, 2, 0, 16).green, 7);
}

TEST(DisplayTransfer, AveragesFourPixelsWhenItHalvesBothSides) {
    GpuMemory memory = TiledImage();
    const TransferConfig config{.input = Base,
                                .output = Output,
                                .input_size = Size(16, 8),
                                .output_size = Size(16, 8),
                                .flags = Rgba8ToRgb8 | ScaleBoth};
    ASSERT_EQ(RunTransfer(memory, config), TransferOutcome::Done);
    // Output (1, 1) averages input (2, 2), (3, 2), (2, 3) and (3, 3).
    EXPECT_EQ(LinearPixel(memory, 1, 1, 8),
              (Color{.red = 2, .green = 2, .blue = 0, .alpha = 0xFF}));
}

TEST(DisplayTransfer, TilesLinearInput) {
    GpuMemory memory;
    const std::vector<std::uint8_t> linear = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_TRUE(memory.Write(Base + (8 * 4), linear));  // row 1, pixels 0 and 1
    const TransferConfig config{.input = Base,
                                .output = Output,
                                .input_size = Size(8, 8),
                                .output_size = Size(8, 8),
                                .flags = InputLinear};
    ASSERT_EQ(RunTransfer(memory, config), TransferOutcome::Done);
    EXPECT_EQ(Read(memory, Output + static_cast<std::uint32_t>(TiledOffset(1, 1, 8, 4)), 4),
              (std::vector<std::uint8_t>{5, 6, 7, 8}));
}

TEST(DisplayTransfer, RefusesWhatAzaharDoesNotImplement) {
    GpuMemory memory;
    const TransferConfig config{.input = Base,
                                .output = Output,
                                .input_size = Size(8, 8),
                                .output_size = Size(8, 8),
                                .flags = InputLinear | ScaleBoth};
    EXPECT_EQ(RunTransfer(memory, config), TransferOutcome::Unsupported);
    EXPECT_EQ(RunTransfer(memory, {.input = Base, .output = Output, .flags = 0}),
              TransferOutcome::Refused);
}

TEST(TextureCopy, CopiesLinesAndSkipsGaps) {
    GpuMemory memory;
    std::vector<std::uint8_t> input(64);
    for (std::size_t at = 0; at < input.size(); ++at) {
        input[at] = static_cast<std::uint8_t>(at);
    }
    ASSERT_TRUE(memory.Write(Base, input));
    // 32 bytes, read 16 then skip 16, written whole.
    const TransferConfig config{.input = Base,
                                .output = Output,
                                .flags = TextureCopy,
                                .texture_size = 32,
                                .texture_input = 1 | (1U << 16),
                                .texture_output = 0};
    ASSERT_EQ(RunTransfer(memory, config), TransferOutcome::Done);
    const std::vector<std::uint8_t> output = Read(memory, Output, 32);
    EXPECT_EQ(output[15], 15);
    EXPECT_EQ(output[16], 32) << "the second line starts after the gap";
}

TEST(GxCommand, StartsAFillUnitForEachBufferThatHasAStart) {
    const GxCommandWords words = {0x02, 0x1F000010, 0xAB, 0x1F000100, 0, 0, 0, 0x02010201};
    const std::vector<MemoryFillConfig> fills = MemoryFills(words);
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills[0].start, 0x18000010U);
    EXPECT_EQ(fills[0].end, 0x18000100U);
    EXPECT_EQ(fills[0].control, 0x0201U);
}

TEST(GxCommand, KeepsAddressesInSixteenByteUnits) {
    const GxCommandWords words = {0x02, 0x1F00001C, 0, 0x1F000108, 0, 0, 0, 0x201};
    EXPECT_EQ(MemoryFills(words).at(0).start, 0x18000010U);
    EXPECT_EQ(MemoryFills(words).at(0).end, 0x18000100U);
}

TEST(GxCommand, TakesTheEndOfVramAsTheEndOfAFill) {
    const GxCommandWords words = {0x02, 0x1F300000, 0, 0x1F600000, 0, 0, 0, 0x201};
    EXPECT_EQ(MemoryFills(words).at(0).end, GpuMemory::VramBase + GpuMemory::VramSize);
}

TEST(GxCommand, ReadsADisplayTransferAndATextureCopy) {
    const GxCommandWords transfer = {
        0x03, 0x1F08C800, 0x14000000, 0x02000100, 0x019000F0, 0x1004, 0, 0};
    const std::optional<TransferConfig> display = Transfer(transfer);
    ASSERT_TRUE(display.has_value());
    EXPECT_EQ(display.value_or(TransferConfig{}).output, GpuMemory::FcramBase);
    EXPECT_EQ(display.value_or(TransferConfig{}).output_size, 0x019000F0U);
    const GxCommandWords copy = {0x04, 0x1F000000, 0x1F000100, 0x40, 0x10001, 0x20002, 0x8, 0};
    const std::optional<TransferConfig> texture = Transfer(copy);
    ASSERT_TRUE(texture.has_value());
    const TransferConfig copied = texture.value_or(TransferConfig{});
    EXPECT_EQ(copied.texture_size, 0x40U);
    EXPECT_EQ(copied.texture_output, 0x20002U);
    EXPECT_EQ(copied.flags, 0x8U);
    EXPECT_FALSE(Transfer(GxCommandWords{0x01}).has_value());
}

}  // namespace
}  // namespace hexlith::domain::gpu
