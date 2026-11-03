#include "application/live_display.h"
#include "domain/gpu/gx_command.h"

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
// The main executable's code, which the GPU cannot see.
constexpr std::uint32_t CodeVirtual = 0x00100000;

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
constexpr std::array<std::uint32_t, LiveDisplay::FramebufferWords> TopFramebuffer = {
    0, VramVirtual, VramVirtual, RowBytes, 0, 0, 0};

// A list of some bytes at the start of the linear heap, as the game submits it.
constexpr std::uint32_t LinearHeap = gpu::GpuMemory::OldLinearHeapBase;

std::array<std::uint32_t, 8> SubmitList(std::uint32_t size) {
    return {static_cast<std::uint32_t>(gpu::GxCommandId::ProcessCommandList),
            LinearHeap,
            size,
            0,
            0,
            0,
            0,
            0};
}

TEST(LiveDisplay, HandsOnAFrameOnceItsFramebufferLinesAreOver) {
    gpu::GpuMemory memory;
    Watcher watcher;
    LiveDisplay display{memory, watcher, watcher};

    ASSERT_TRUE(display.Framebuffer(1, 0, TopFramebuffer));
    EXPECT_TRUE(watcher.frames.empty());
    ASSERT_TRUE(display.Command(1, SubmitList(16)));
    EXPECT_EQ(watcher.frames, std::vector<std::uint64_t>{1});
}

TEST(LiveDisplay, ShowsWhatMemoryHoldsAndPassesListsToTheBackend) {
    gpu::GpuMemory memory;
    ASSERT_TRUE(memory.Write(gpu::GpuMemory::VramBase, std::vector<std::uint8_t>(PageSize, White)));
    Watcher watcher;
    LiveDisplay display{memory, watcher, watcher};

    ASSERT_TRUE(display.Framebuffer(1, 0, TopFramebuffer));
    ASSERT_TRUE(display.Command(1, SubmitList(24)));
    ASSERT_TRUE(display.Finish());

    EXPECT_EQ(watcher.list_sizes, std::vector<std::size_t>{24});
    ASSERT_FALSE(watcher.first_pixels.empty());
    EXPECT_EQ(watcher.first_pixels.front().red, White);
}

TEST(LiveDisplay, ShowsWhatTheLastFrameLeftWhenItIsFinished) {
    gpu::GpuMemory memory;
    Watcher watcher;
    LiveDisplay display{memory, watcher, watcher};

    ASSERT_TRUE(display.Command(3, SubmitList(8)));
    ASSERT_TRUE(display.Command(4, SubmitList(8)));
    ASSERT_TRUE(display.Finish());

    EXPECT_EQ(watcher.frames, (std::vector<std::uint64_t>{3, 4, 5}));
    EXPECT_EQ(display.Presented(), 3U);
}

// gsp::Gpu fills the game's memory itself; filling it again here would be
// work done twice.
TEST(LiveDisplay, LeavesMemoryFillsToTheGame) {
    gpu::GpuMemory memory;
    Watcher watcher;
    LiveDisplay display{memory, watcher, watcher};
    const std::array<std::uint32_t, 8> fill = {
        static_cast<std::uint32_t>(gpu::GxCommandId::MemoryFill),
        VramVirtual,
        0xFFFFFFFF,
        VramVirtual + PageSize,
        0,
        0,
        0,
        0x0002};

    ASSERT_TRUE(display.Command(1, fill));
    EXPECT_EQ(memory.Bytes(gpu::GpuMemory::VramBase, 1).front(), 0);
}

TEST(LiveDisplay, CountsTheListsAndTransfersItRuns) {
    gpu::GpuMemory memory;
    Watcher watcher;
    LiveDisplay display{memory, watcher, watcher};
    const std::array<std::uint32_t, 8> transfer = {
        static_cast<std::uint32_t>(gpu::GxCommandId::DisplayTransfer),
        VramVirtual,
        VramVirtual + 0x10000,
        0x00F00010,
        0x00F00010,
        0,
        0,
        0};

    ASSERT_TRUE(display.Command(1, SubmitList(16)));
    ASSERT_TRUE(display.Command(1, SubmitList(16)));
    ASSERT_TRUE(display.Command(1, transfer));

    EXPECT_EQ(display.Work().lists, 2U);
    EXPECT_EQ(display.Work().transfers, 1U);
}

TEST(LiveDisplay, RefusesAListOutsideGpuMemory) {
    gpu::GpuMemory memory;
    Watcher watcher;
    LiveDisplay display{memory, watcher, watcher};
    std::array<std::uint32_t, 8> outside = SubmitList(16);
    outside.at(1) = CodeVirtual;

    EXPECT_FALSE(display.Command(1, outside));
    EXPECT_TRUE(watcher.list_sizes.empty());
}

}  // namespace
}  // namespace hexlith::application
