#include "domain/gpu/gx_command.h"

#include <gtest/gtest.h>

namespace hexlith::domain::gpu {
namespace {

constexpr std::uint32_t Vram = GpuMemory::VramVirtualBase;
constexpr std::uint32_t Heap = GpuMemory::OldLinearHeapBase;

GxCommandWords Command(GxCommandId id) {
    GxCommandWords words{};
    words[0] = static_cast<std::uint32_t>(id);
    return words;
}

TEST(MemoryUsed, ReadsAndWritesNothingForACacheFlush) {
    const GxMemoryUse use = MemoryUsed(Command(GxCommandId::FlushCacheRegions));
    EXPECT_TRUE(use.reads.empty());
    EXPECT_TRUE(use.writes.empty());
}

TEST(MemoryUsed, WritesTheRangeOfEachMemoryFill) {
    GxCommandWords words = Command(GxCommandId::MemoryFill);
    words[1] = Vram;
    words[3] = Vram + 0x1000;
    words[4] = Vram + 0x8000;
    words[6] = Vram + 0x9000;
    const GxMemoryUse use = MemoryUsed(words);
    EXPECT_TRUE(use.reads.empty());
    ASSERT_EQ(use.writes.size(), 2U);
    EXPECT_EQ(use.writes[0], (MemoryRange{GpuMemory::VramBase, 0x1000}));
    EXPECT_EQ(use.writes[1], (MemoryRange{GpuMemory::VramBase + 0x8000, 0x1000}));
}

// The buffers' sizes are in pixels, at most four bytes each, so the ranges
// cover whatever the formats turn out to be.
TEST(MemoryUsed, ReadsTheInputAndWritesTheOutputOfADisplayTransfer) {
    GxCommandWords words = Command(GxCommandId::DisplayTransfer);
    words[1] = Vram;
    words[2] = Vram + 0x100000;
    words[3] = (240U << 16) | 16U;
    words[4] = (240U << 16) | 8U;
    const GxMemoryUse use = MemoryUsed(words);
    ASSERT_EQ(use.reads.size(), 1U);
    ASSERT_EQ(use.writes.size(), 1U);
    EXPECT_EQ(use.reads[0], (MemoryRange{GpuMemory::VramBase, 16 * 240 * 4}));
    EXPECT_EQ(use.writes[0], (MemoryRange{GpuMemory::VramBase + 0x100000, 8 * 240 * 4}));
}

TEST(MemoryUsed, SpansTheGapsOfATextureCopy) {
    GxCommandWords words = Command(GxCommandId::TextureCopy);
    words[1] = Vram;
    words[2] = Heap;
    words[3] = 0x400;
    // Rows of 0x100 bytes with 0x100 between them, on the source side only.
    words[4] = (0x10U << 16) | 0x10U;
    words[5] = 0;
    const GxMemoryUse use = MemoryUsed(words);
    ASSERT_EQ(use.reads.size(), 1U);
    ASSERT_EQ(use.writes.size(), 1U);
    EXPECT_EQ(use.reads[0], (MemoryRange{GpuMemory::VramBase, 4 * 0x200}));
    EXPECT_EQ(use.writes[0], (MemoryRange{GpuMemory::FcramBase, 0x400}));
}

TEST(MemoryUsed, ReadsTheSourceAndWritesTheDestinationOfADma) {
    GxCommandWords words = Command(GxCommandId::RequestDma);
    words[1] = Heap + 0x2000;
    words[2] = Vram + 0x3000;
    words[3] = 0x500;
    const GxMemoryUse use = MemoryUsed(words);
    ASSERT_EQ(use.reads.size(), 1U);
    ASSERT_EQ(use.writes.size(), 1U);
    EXPECT_EQ(use.reads[0], (MemoryRange{GpuMemory::FcramBase + 0x2000, 0x500}));
    EXPECT_EQ(use.writes[0], (MemoryRange{GpuMemory::VramBase + 0x3000, 0x500}));
}

TEST(MemoryUsed, LeavesOutAnAddressOutsideGpuMemory) {
    GxCommandWords words = Command(GxCommandId::RequestDma);
    words[1] = 0x00100000;
    words[2] = Vram;
    words[3] = 0x100;
    const GxMemoryUse use = MemoryUsed(words);
    EXPECT_TRUE(use.reads.empty());
    EXPECT_EQ(use.writes.size(), 1U);
}

TEST(MemoryUsed, ReadsTheListAProcessCommandListRuns) {
    GxCommandWords words = Command(GxCommandId::ProcessCommandList);
    words[1] = Heap + 0x100;
    words[2] = 0x40;
    const GxMemoryUse use = MemoryUsed(words);
    ASSERT_EQ(use.reads.size(), 1U);
    EXPECT_EQ(use.reads[0], (MemoryRange{GpuMemory::FcramBase + 0x100, 0x40}));
    EXPECT_TRUE(use.writes.empty());
}

}  // namespace
}  // namespace hexlith::domain::gpu
