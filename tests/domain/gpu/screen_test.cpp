#include "domain/gpu/screen.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::gpu {
namespace {

// The bottom screen's framebuffer: 320 rows of 240 RGB8 pixels.
constexpr std::uint32_t Framebuffer = 0x1F000000;
constexpr std::uint32_t Stride = 240 * 3;
constexpr std::uint32_t Rgb8 = 1;

RecordedFramebuffer Describe(std::uint32_t index, std::uint32_t address, std::uint32_t shown) {
    return RecordedFramebuffer{.frame = 1,
                               .screen = ScreenSetup::Bottom,
                               .words = {index, address, 0, Stride, Rgb8, shown, 0}};
}

TEST(ScreenSetup, ShowsARowOfTheFramebufferAsAColumnFromTheBottom) {
    GpuMemory memory;
    // Row 10, pixel 0: blue, green, red in memory.
    const std::uint32_t physical = GpuMemory::VramBase + (10 * Stride);
    ASSERT_TRUE(memory.Write(physical, std::vector<std::uint8_t>{0x30, 0x20, 0x10}));
    ScreenSetup setup;
    setup.Apply(Describe(0, Framebuffer, 0));
    const Picture picture = setup.Compose(memory, ScreenSetup::Bottom);
    ASSERT_EQ(picture.width, 320U);
    ASSERT_EQ(picture.height, 240U);
    EXPECT_EQ(picture.pixels.at((239 * 320) + 10),
              (Color{.red = 0x10, .green = 0x20, .blue = 0x30, .alpha = 0xFF}));
}

TEST(ScreenSetup, ShowsTheFramebufferItIsToldTo) {
    GpuMemory memory;
    constexpr std::uint32_t Second = Framebuffer + 0x40000;
    ASSERT_TRUE(memory.Write(GpuMemory::VramBase + 0x40000, std::vector<std::uint8_t>{0, 0, 0xFF}));
    ScreenSetup setup;
    setup.Apply(Describe(0, Framebuffer, 0));
    setup.Apply(Describe(1, Second, 0));
    EXPECT_EQ(setup.Compose(memory, ScreenSetup::Bottom).pixels.at(std::size_t{239} * 320).red, 0)
        << "the first is shown";
    setup.Apply(Describe(0, Framebuffer, 1));
    EXPECT_EQ(setup.Compose(memory, ScreenSetup::Bottom).pixels.at(std::size_t{239} * 320).red,
              0xFF)
        << "now the second";
}

TEST(ScreenSetup, IsBlackBeforeItIsSetUp) {
    const GpuMemory memory;
    const Picture picture = ScreenSetup{}.Compose(memory, ScreenSetup::Top);
    EXPECT_EQ(picture.width, 400U);
    EXPECT_EQ(picture.pixels.front(), (Color{.red = 0, .green = 0, .blue = 0, .alpha = 0xFF}));
}

}  // namespace
}  // namespace hexlith::domain::gpu
