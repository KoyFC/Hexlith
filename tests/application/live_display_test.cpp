#include "application/live_display.h"

#include <array>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::application {
namespace {

namespace gpu = domain::gpu;

constexpr std::uint32_t VramVirtual = 0x1F000000;
constexpr std::size_t PageSize = 0x1000;
constexpr std::uint32_t RowBytes = 240 * 4;
constexpr std::uint8_t White = 0xFF;

// What the sink was handed and what the backend was asked to draw.
class Watcher final : public ports::ScreenSink, public ports::GpuBackend {
public:
    core::Status
    Present(std::uint64_t frame, const gpu::Picture& top, const gpu::Picture& /*bottom*/) override {
        frames.push_back(frame);
        first_pixels.push_back(top.pixels.front());
        return {};
    }

    [[nodiscard]] bool StopRequested() const override {
        return false;
    }

    core::Status RunCommandList(std::span<const std::uint8_t> list) override {
        list_sizes.push_back(list.size());
        return {};
    }

    [[nodiscard]] ports::GpuBackendStatistics Statistics() const override {
        return {};
    }

    [[nodiscard]] std::string Describe() const override {
        return "a watcher";
    }

    std::vector<std::uint64_t> frames;
    std::vector<gpu::Color> first_pixels;
    std::vector<std::size_t> list_sizes;
};

// A game's description of a framebuffer in VRAM, shown as soon as it is told.
constexpr std::array<std::uint32_t, ports::GpuRecorder::FramebufferWords> TopFramebuffer = {
    0, VramVirtual, VramVirtual, RowBytes, 0, 0, 0};

TEST(LiveDisplay, HandsOnAFrameOnceItsFramebufferLinesAreOver) {
    gpu::GpuMemory memory;
    Watcher watcher;
    LiveDisplay display{memory, watcher, watcher};

    ASSERT_TRUE(display.Framebuffer(1, 0, TopFramebuffer));
    EXPECT_TRUE(watcher.frames.empty());
    ASSERT_TRUE(display.List(1, 0, std::vector<std::uint8_t>(16)));
    EXPECT_EQ(watcher.frames, std::vector<std::uint64_t>{1});
}

TEST(LiveDisplay, ShowsWhatThePagesHoldAndPassesListsToTheBackend) {
    gpu::GpuMemory memory;
    Watcher watcher;
    LiveDisplay display{memory, watcher, watcher};

    ASSERT_TRUE(display.Page(1, VramVirtual, std::vector<std::uint8_t>(PageSize, White)));
    ASSERT_TRUE(display.Framebuffer(1, 0, TopFramebuffer));
    ASSERT_TRUE(display.List(1, 0, std::vector<std::uint8_t>(24)));
    ASSERT_TRUE(display.Finish());

    EXPECT_EQ(watcher.list_sizes, std::vector<std::size_t>{24});
    ASSERT_FALSE(watcher.first_pixels.empty());
    EXPECT_EQ(watcher.first_pixels.front().red, White);
}

TEST(LiveDisplay, ShowsWhatTheLastFrameLeftWhenItIsFinished) {
    gpu::GpuMemory memory;
    Watcher watcher;
    LiveDisplay display{memory, watcher, watcher};

    ASSERT_TRUE(display.List(3, 0, std::vector<std::uint8_t>(8)));
    ASSERT_TRUE(display.List(4, 0, std::vector<std::uint8_t>(8)));
    ASSERT_TRUE(display.Finish());

    EXPECT_EQ(watcher.frames, (std::vector<std::uint64_t>{3, 4, 5}));
    EXPECT_EQ(display.Presented(), 3U);
}

TEST(LiveDisplay, RefusesAPageOutsideGpuMemory) {
    gpu::GpuMemory memory;
    Watcher watcher;
    LiveDisplay display{memory, watcher, watcher};

    EXPECT_FALSE(display.Page(1, 0x00100000, std::vector<std::uint8_t>(PageSize)));
}

}  // namespace
}  // namespace hexlith::application
