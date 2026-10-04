#include "domain/memory/paged_memory.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>

namespace hexlith::domain::memory {
namespace {

constexpr std::uint32_t Base = 0x00100000;
constexpr std::uint32_t Page = PagedMemory::PageSize;

TEST(PagedMemory, ReadsBackWhatWasWrittenLittleEndian) {
    PagedMemory memory;
    ASSERT_TRUE(memory.Map(Base, Page, true));

    ASSERT_TRUE(memory.Write32(Base, 0x11223344));
    EXPECT_EQ(memory.Read8(Base), 0x44);
    EXPECT_EQ(memory.Read16(Base + 2), 0x1122);
    EXPECT_EQ(memory.Read32(Base), 0x11223344U);

    ASSERT_TRUE(memory.Write64(Base + 8, 0x0102030405060708ULL));
    EXPECT_EQ(memory.Read32(Base + 8), 0x05060708U) << "the low word comes first";
    EXPECT_EQ(memory.Read64(Base + 8), 0x0102030405060708ULL);
}

TEST(PagedMemory, StartsZeroFilled) {
    PagedMemory memory;
    ASSERT_TRUE(memory.Map(Base, Page, false));
    EXPECT_EQ(memory.Read32(Base + 0x100), 0U);
}

TEST(PagedMemory, FailsOutsideWhatIsMapped) {
    PagedMemory memory;
    ASSERT_TRUE(memory.Map(Base, Page, true));
    EXPECT_FALSE(memory.Read8(Base - 1).has_value());
    EXPECT_FALSE(memory.Read8(Base + Page).has_value());
    EXPECT_FALSE(memory.Write8(Base + Page, 0));
}

TEST(PagedMemory, CrossesAPageBoundaryOnlyWhenBothPagesAreMapped) {
    PagedMemory memory;
    ASSERT_TRUE(memory.Map(Base, 2 * Page, true));
    ASSERT_TRUE(memory.Write32(Base + Page - 2, 0xAABBCCDD));
    EXPECT_EQ(memory.Read32(Base + Page - 2), 0xAABBCCDDU);

    PagedMemory single;
    ASSERT_TRUE(single.Map(Base, Page, true));
    EXPECT_FALSE(single.Read32(Base + Page - 2).has_value());
    EXPECT_FALSE(single.Write32(Base + Page - 2, 0xFFFFFFFF));
    EXPECT_EQ(single.Read16(Base + Page - 2), 0U) << "a failed write changes nothing";
}

TEST(PagedMemory, RefusesWritesToReadOnlyPagesButLoadsThem) {
    PagedMemory memory;
    ASSERT_TRUE(memory.Map(Base, Page, false));
    EXPECT_FALSE(memory.Write32(Base, 1));

    const std::array<std::uint8_t, 4> code = {0x1E, 0xFF, 0x2F, 0xE1};
    ASSERT_TRUE(memory.Load(Base, code));
    EXPECT_EQ(memory.Read32(Base), 0xE12FFF1EU);
}

TEST(PagedMemory, RefusesAMappingThatIsNotPageAlignedOrOverlaps) {
    PagedMemory memory;
    EXPECT_FALSE(memory.Map(Base + 1, Page, true));
    EXPECT_FALSE(memory.Map(Base, Page + 1, true));
    ASSERT_TRUE(memory.Map(Base, Page, true));
    EXPECT_FALSE(memory.Map(Base, Page, true));
}

TEST(PagedMemory, RefusesALoadThatRunsOffTheMapping) {
    PagedMemory memory;
    ASSERT_TRUE(memory.Map(Base, Page, true));
    const std::array<std::uint8_t, 4> bytes = {1, 2, 3, 4};
    EXPECT_FALSE(memory.Load(Base + Page - 2, bytes));
    EXPECT_EQ(memory.Read16(Base + Page - 2), 0U) << "nothing was written";
}

TEST(PagedMemory, MapsTheTopOfTheAddressSpace) {
    PagedMemory memory;
    ASSERT_TRUE(memory.Map(0xFFFFF000, Page, true));
    ASSERT_TRUE(memory.Write8(0xFFFFFFFF, 7));
    EXPECT_EQ(memory.Read8(0xFFFFFFFF), 7);
    EXPECT_FALSE(memory.Read16(0xFFFFFFFF).has_value())
        << "the second byte wraps to address zero, which is not mapped";
}

TEST(PagedMemory, CopiesOutAcrossPages) {
    PagedMemory memory;
    ASSERT_TRUE(memory.Map(Base, 2 * Page, true));
    ASSERT_TRUE(memory.Write32(Base + Page - 2, 0xAABBCCDD));

    std::array<std::uint8_t, 4> bytes{};
    ASSERT_TRUE(memory.Copy(Base + Page - 2, bytes));
    EXPECT_EQ(bytes, (std::array<std::uint8_t, 4>{0xDD, 0xCC, 0xBB, 0xAA}));
    EXPECT_FALSE(memory.Copy(Base + (2 * Page) - 2, bytes)) << "the last two bytes are unmapped";
}

}  // namespace
}  // namespace hexlith::domain::memory
