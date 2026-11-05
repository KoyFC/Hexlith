#include "application/live_display.h"
#include "domain/gpu/gx_command.h"

#include <algorithm>
#include <array>
#include <format>
#include <gtest/gtest.h>
#include <string>
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
        calls.emplace_back("list");
        return {};
    }

    core::Status BeforeRead(std::uint32_t physical, std::uint32_t size) override {
        calls.push_back(std::format("read {:08X} {:X}", physical, size));
        return {};
    }

    core::Status BeforeWrite(std::uint32_t physical, std::uint32_t size) override {
        calls.push_back(std::format("write {:08X} {:X}", physical, size));
        return {};
    }

    core::Result<bool> RunDisplayTransfer(const gpu::TransferConfig& transfer) override {
        offered.push_back(transfer.output);
        return carries_out_transfers;
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
    std::vector<std::string> calls;
    // The outputs of the transfers the backend was offered, and whether it
    // takes them.
    std::vector<std::uint32_t> offered;
    bool carries_out_transfers = false;
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

// The backend may hold what it drew for a while, so it is told before anything
// else reads or writes the memory behind it (ADR 0030).
TEST(LiveDisplay, TellsTheBackendWhatAFillWritesBeforeItIsFilled) {
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
    EXPECT_EQ(watcher.calls, (std::vector<std::string>{"write 18000000 1000"}));
}

TEST(LiveDisplay, TellsTheBackendWhatATransferReadsAndWrites) {
    gpu::GpuMemory memory;
    Watcher watcher;
    LiveDisplay display{memory, watcher, watcher};
    const std::array<std::uint32_t, 8> transfer = {
        static_cast<std::uint32_t>(gpu::GxCommandId::DisplayTransfer),
        VramVirtual,
        VramVirtual + 0x10000,
        (4U << 16) | 4U,
        (4U << 16) | 4U,
        0,
        0,
        0};

    ASSERT_TRUE(display.Command(1, transfer));
    EXPECT_EQ(watcher.calls, (std::vector<std::string>{"read 18000000 40", "write 18010000 40"}));
}

// A 4 by 4 copy of RGBA8 from one page of VRAM to another, which memory can
// carry out and a backend may offer to.
constexpr std::array<std::uint32_t, 8> SmallTransfer = {
    static_cast<std::uint32_t>(gpu::GxCommandId::DisplayTransfer),
    VramVirtual,
    VramVirtual + 0x10000,
    (4U << 16) | 4U,
    (4U << 16) | 4U,
    0,
    0,
    0};

// ADR 0033: a backend that takes a transfer owes memory its output when it is
// read, so neither the announcements nor the copy on memory are made for it.
TEST(LiveDisplay, LeavesATransferToTheBackendThatTakesIt) {
    gpu::GpuMemory memory;
    std::ranges::fill(memory.Bytes(gpu::GpuMemory::VramBase, 0x40), White);
    Watcher watcher;
    watcher.carries_out_transfers = true;
    LiveDisplay display{memory, watcher, watcher};

    ASSERT_TRUE(display.Command(1, SmallTransfer));

    EXPECT_EQ(watcher.offered, std::vector<std::uint32_t>{gpu::GpuMemory::VramBase + 0x10000});
    EXPECT_TRUE(watcher.calls.empty()) << "nothing is announced for a transfer it carries out";
    EXPECT_EQ(memory.Bytes(gpu::GpuMemory::VramBase + 0x10000, 1).front(), 0);
    EXPECT_EQ(display.Work().transfers, 1U);
}

TEST(LiveDisplay, DoesTheTransferOnMemoryWhenTheBackendDeclinesIt) {
    gpu::GpuMemory memory;
    std::ranges::fill(memory.Bytes(gpu::GpuMemory::VramBase, 0x40), White);
    Watcher watcher;
    LiveDisplay display{memory, watcher, watcher};

    ASSERT_TRUE(display.Command(1, SmallTransfer));

    EXPECT_EQ(watcher.offered.size(), 1U);
    EXPECT_EQ(watcher.calls, (std::vector<std::string>{"read 18000000 40", "write 18010000 40"}));
    EXPECT_EQ(memory.Bytes(gpu::GpuMemory::VramBase + 0x10000, 1).front(), White);
}

TEST(LiveDisplay, TellsTheBackendWhatAFrameShowsBeforeTakingItsPicture) {
    gpu::GpuMemory memory;
    Watcher watcher;
    LiveDisplay display{memory, watcher, watcher};

    ASSERT_TRUE(display.Framebuffer(1, 0, TopFramebuffer));
    ASSERT_TRUE(display.Command(1, SubmitList(16)));

    // The top screen's 400 columns of 960 bytes, then the list.
    EXPECT_EQ(watcher.calls,
              (std::vector<std::string>{"read 18000000 5DC00", "read 20000000 10", "list"}));
}

TEST(LiveDisplay, MakesMemoryTrueWhenItIsFinished) {
    gpu::GpuMemory memory;
    Watcher watcher;
    LiveDisplay display{memory, watcher, watcher};

    ASSERT_TRUE(display.Finish());
    ASSERT_GE(watcher.calls.size(), 2U);
    EXPECT_EQ(watcher.calls.at(watcher.calls.size() - 2), "read 18000000 600000");
    EXPECT_EQ(watcher.calls.back(), "read 20000000 8000000");
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
