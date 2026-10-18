#include "domain/gpu/gpu_memory.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::gpu {
namespace {

TEST(GpuMemory, FindsThePhysicalAddressBehindAGamesView) {
    EXPECT_EQ(GpuMemory::PhysicalFromVirtual(0x1F000010), 0x18000010U) << "VRAM";
    EXPECT_EQ(GpuMemory::PhysicalFromVirtual(0x14668B80), 0x20668B80U) << "the old linear heap";
    EXPECT_EQ(GpuMemory::PhysicalFromVirtual(0x30001000), 0x20001000U) << "the new linear heap";
    EXPECT_FALSE(GpuMemory::PhysicalFromVirtual(0x1F600000).has_value()) << "past VRAM";
    EXPECT_FALSE(GpuMemory::PhysicalFromVirtual(0x08000000).has_value()) << "the heap";
}

TEST(GpuMemory, ReadsBackWhatWasWritten) {
    GpuMemory memory;
    const std::vector<std::uint8_t> bytes = {1, 2, 3, 4};
    ASSERT_TRUE(memory.Write(0x18100000, bytes));
    const auto read = memory.Bytes(0x18100000, 4);
    EXPECT_EQ(std::vector<std::uint8_t>(read.begin(), read.end()), bytes);
    EXPECT_EQ(memory.Bytes(0x20000000, 1).front(), 0) << "memory starts zeroed";
}

TEST(GpuMemory, RefusesAnythingThatLeavesItsMemory) {
    GpuMemory memory;
    EXPECT_TRUE(memory.Bytes(GpuMemory::VramBase + GpuMemory::VramSize - 2, 4).empty());
    EXPECT_TRUE(memory.Bytes(0x1C000000, 4).empty()) << "between VRAM and FCRAM";
    EXPECT_FALSE(memory.Write(GpuMemory::FcramBase + GpuMemory::FcramSize - 1,
                              std::vector<std::uint8_t>{1, 2}));
}

}  // namespace
}  // namespace hexlith::domain::gpu
