#include "domain/kernel/region_allocator.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::kernel {
namespace {

constexpr std::uint32_t Page = 0x1000;
constexpr std::uint32_t RegionSize = 16 * Page;

TEST(RegionAllocator, TakesHeapFromTheTopAndLinearFromTheBottom) {
    RegionAllocator region(RegionSize);

    const auto heap = region.TakeFromTop(2 * Page);
    ASSERT_TRUE(heap.has_value());
    const std::vector<Extent> top = {Extent{.offset = RegionSize - (2 * Page), .size = 2 * Page}};
    EXPECT_EQ(heap.value_or(std::vector<Extent>{}), top);

    EXPECT_EQ(region.TakeLowest(Page), 0U);
    EXPECT_EQ(region.TakeLowest(Page), Page);
    EXPECT_EQ(region.Used(), 4 * Page);
}

TEST(RegionAllocator, SplitsAHeapRequestAcrossFreePiecesHighestFirst) {
    RegionAllocator region(4 * Page);
    ASSERT_TRUE(region.TakeAt(Extent{.offset = Page, .size = Page}));
    ASSERT_TRUE(region.TakeAt(Extent{.offset = 3 * Page, .size = Page}));

    // Free: [0, 1) and [2, 3) in pages. Three pages cannot be had.
    EXPECT_FALSE(region.TakeFromTop(3 * Page).has_value());
    EXPECT_EQ(region.Used(), 2 * Page) << "a refused request takes nothing";

    const auto taken = region.TakeFromTop(2 * Page);
    ASSERT_TRUE(taken.has_value());
    const std::vector<Extent> expected = {Extent{.offset = 2 * Page, .size = Page},
                                          Extent{.offset = 0, .size = Page}};
    EXPECT_EQ(taken.value_or(std::vector<Extent>{}), expected);
}

TEST(RegionAllocator, TakesTheLowestPieceThatFits) {
    RegionAllocator region(8 * Page);
    ASSERT_TRUE(region.TakeAt(Extent{.offset = Page, .size = Page}));

    // Page 0 is free but too small for two.
    EXPECT_EQ(region.TakeLowest(2 * Page), 2 * Page);
    EXPECT_EQ(region.TakeLowest(Page), 0U);
}

TEST(RegionAllocator, RefusesToTakeWhatIsTaken) {
    RegionAllocator region(4 * Page);
    ASSERT_TRUE(region.TakeAt(Extent{.offset = Page, .size = 2 * Page}));
    EXPECT_FALSE(region.TakeAt(Extent{.offset = 0, .size = 2 * Page}));
    EXPECT_FALSE(region.TakeAt(Extent{.offset = 2 * Page, .size = Page}));
    EXPECT_FALSE(region.TakeAt(Extent{.offset = 3 * Page, .size = 2 * Page}))
        << "past the end of the region";
}

TEST(RegionAllocator, GivingBackMergesNeighbours) {
    RegionAllocator region(4 * Page);
    ASSERT_TRUE(region.TakeAt(Extent{.offset = 0, .size = 4 * Page}));

    region.Give(Extent{.offset = Page, .size = Page});
    region.Give(Extent{.offset = 3 * Page, .size = Page});
    region.Give(Extent{.offset = 2 * Page, .size = Page});
    EXPECT_EQ(region.Used(), Page);

    // Pages 1 to 3 are one piece again, so three of them fit contiguously.
    EXPECT_EQ(region.TakeLowest(3 * Page), Page);
}

TEST(RegionAllocator, RefusesToGiveBackWhatIsFree) {
    RegionAllocator region(4 * Page);
    EXPECT_THROW(region.Give(Extent{.offset = 0, .size = Page}), std::logic_error);
}

}  // namespace
}  // namespace hexlith::domain::kernel
