#include "domain/kernel/address_space.h"

#include <cstdint>
#include <gtest/gtest.h>

namespace hexlith::domain::kernel {
namespace {

using memory::Access;

constexpr std::uint32_t Page = layout::PageSize;
constexpr std::uint32_t RegionSize = 64 * Page;

// The reference title's extended header declares kernel 2.35.
constexpr std::uint16_t ReferenceKernelVersion = 0x0223;

// Where the reference title's first module is mapped, and the heap buffer it
// was read into, from the Azahar trace in docs/ldr-ro-trace.md.
constexpr std::uint32_t ModuleAddress = 0x006A5000;
constexpr std::uint32_t ModuleBuffer = 0x08044000;

AddressSpace MakeSpace() {
    return {RegionSize, layout::LinearHeapBaseFor(ReferenceKernelVersion)};
}

TEST(AddressSpace, PutsTheLinearHeapWhereTheKernelVersionSays) {
    EXPECT_EQ(layout::LinearHeapBaseFor(ReferenceKernelVersion), 0x14000000U);
    EXPECT_EQ(layout::LinearHeapBaseFor(0x022C), 0x30000000U);
}

TEST(AddressSpace, CommitsZeroFilledHeapInsideTheHeapRangeOnly) {
    AddressSpace space = MakeSpace();
    EXPECT_EQ(space.CommitHeap(layout::HeapBase, 2 * Page, Access::ReadWrite), result::Success);
    EXPECT_EQ(space.Memory().Read32(layout::HeapBase + Page), 0U);
    EXPECT_TRUE(space.Memory().Write32(layout::HeapBase, 1));
    EXPECT_EQ(space.StateAt(layout::HeapBase), MemoryState::Private);

    EXPECT_EQ(space.CommitHeap(layout::HeapBase - Page, Page, Access::ReadWrite),
              result::InvalidAddress);
    EXPECT_EQ(space.CommitHeap(layout::HeapBase, Page, Access::ReadWrite),
              result::InvalidAddressState)
        << "already committed";
    EXPECT_EQ(space.CommitHeap(layout::HeapBase + (4 * Page), RegionSize, Access::ReadWrite),
              result::OutOfHeapMemory);
}

TEST(AddressSpace, GivesTheFirstLinearAllocationTheBottomOfTheRegion) {
    AddressSpace space = MakeSpace();
    // Heap comes from the top, so it does not move where linear memory starts.
    ASSERT_EQ(space.CommitHeap(layout::HeapBase, 4 * Page, Access::ReadWrite), result::Success);

    const AddressResult first = space.CommitLinear(0, 2 * Page, Access::ReadWrite);
    EXPECT_EQ(first.result, result::Success);
    EXPECT_EQ(first.address, 0x14000000U);
    EXPECT_EQ(space.StateAt(first.address), MemoryState::Continuous);

    const AddressResult second = space.CommitLinear(0, Page, Access::ReadWrite);
    EXPECT_EQ(second.address, 0x14000000U + (2 * Page));

    EXPECT_EQ(space.FreeLinear(first.address, 2 * Page), result::Success);
    EXPECT_EQ(space.CommitLinear(0, Page, Access::ReadWrite).address, 0x14000000U)
        << "freed memory is the lowest piece again";
}

TEST(AddressSpace, CommitsLinearMemoryAtAnAddressTheCallerChose) {
    AddressSpace space = MakeSpace();
    const std::uint32_t wanted = space.LinearHeapBase() + (8 * Page);
    EXPECT_EQ(space.CommitLinear(wanted, Page, Access::ReadWrite).address, wanted);
    EXPECT_EQ(space.CommitLinear(wanted, Page, Access::ReadWrite).result,
              result::InvalidAddressState);
    EXPECT_EQ(space.CommitLinear(space.LinearHeapEnd(), Page, Access::ReadWrite).result,
              result::InvalidAddress);
}

TEST(AddressSpace, FreeingHeapReturnsItToTheRegion) {
    AddressSpace space = MakeSpace();
    ASSERT_EQ(space.CommitHeap(layout::HeapBase, 4 * Page, Access::ReadWrite), result::Success);
    EXPECT_EQ(space.RegionUsed(), 4 * Page);

    EXPECT_EQ(space.FreeHeap(layout::HeapBase + Page, 2 * Page), result::Success);
    EXPECT_EQ(space.RegionUsed(), 2 * Page);
    EXPECT_FALSE(space.Memory().IsMapped(layout::HeapBase + Page));
    EXPECT_TRUE(space.Memory().IsMapped(layout::HeapBase));
    EXPECT_EQ(space.FreeHeap(layout::HeapBase + Page, Page), result::InvalidAddressState);
}

TEST(AddressSpace, APrivilegedAliasIsWhatLdrRoMakesOfAModule) {
    AddressSpace space = MakeSpace();
    ASSERT_EQ(space.CommitHeap(ModuleBuffer, 4 * Page, Access::ReadWrite), result::Success);
    ASSERT_TRUE(space.Memory().Write32(ModuleBuffer + Page, 0xE12FFF1E));

    ASSERT_EQ(space.MapAlias(ModuleAddress, ModuleBuffer, 4 * Page, Access::Read, true),
              result::Success);
    EXPECT_EQ(space.Memory().Read32(ModuleAddress + Page), 0xE12FFF1EU);
    EXPECT_EQ(space.StateAt(ModuleAddress), MemoryState::AliasCode);
    EXPECT_EQ(space.StateAt(ModuleBuffer), MemoryState::Locked);
    EXPECT_FALSE(space.Memory().Read32(ModuleBuffer).has_value())
        << "the buffer is out of the game's reach while the module is loaded";

    ASSERT_EQ(space.UnmapAlias(ModuleAddress, ModuleBuffer, 4 * Page, Access::ReadWrite, true),
              result::Success);
    EXPECT_FALSE(space.Memory().IsMapped(ModuleAddress));
    EXPECT_EQ(space.StateAt(ModuleBuffer), MemoryState::Private);
    EXPECT_EQ(space.Memory().Read32(ModuleBuffer + Page), 0xE12FFF1EU);
}

TEST(AddressSpace, AnOrdinaryAliasNeedsHeapAsItsSource) {
    AddressSpace space = MakeSpace();
    ASSERT_EQ(space.CommitHeap(layout::HeapBase, Page, Access::ReadWrite), result::Success);

    EXPECT_EQ(space.MapAlias(ModuleAddress, 0x00100000, Page, Access::ReadWrite, false),
              result::InvalidAddress);
    EXPECT_EQ(space.MapAlias(layout::HeapBase, layout::HeapBase, Page, Access::Read, false),
              result::InvalidAddressState)
        << "overlapping ranges";
    ASSERT_EQ(space.MapAlias(ModuleAddress, layout::HeapBase, Page, Access::ReadWrite, false),
              result::Success);
    EXPECT_EQ(space.StateAt(layout::HeapBase), MemoryState::Aliased);
    EXPECT_TRUE(space.Memory().Write32(layout::HeapBase, 3)) << "both stay usable";
    EXPECT_EQ(space.Memory().Read32(ModuleAddress), 3U);
}

TEST(AddressSpace, APrivilegedMapOntoItselfOnlyChangesStateAndAccess) {
    AddressSpace space = MakeSpace();
    ASSERT_EQ(space.CommitHeap(layout::HeapBase, Page, Access::ReadWrite), result::Success);
    ASSERT_EQ(space.MapAlias(layout::HeapBase, layout::HeapBase, Page, Access::ReadExecute, true),
              result::Success);
    EXPECT_EQ(space.StateAt(layout::HeapBase), MemoryState::AliasCode);
    EXPECT_EQ(space.Memory().AccessAt(layout::HeapBase), Access::ReadExecute);
}

TEST(AddressSpace, AnswersQueryMemoryWithTheWholeRun) {
    AddressSpace space = MakeSpace();
    ASSERT_EQ(space.CommitHeap(layout::HeapBase, 3 * Page, Access::ReadWrite), result::Success);
    ASSERT_EQ(space.Protect(layout::HeapBase + (2 * Page), Page, Access::Read), result::Success);

    const MemoryInfo info = space.Query(layout::HeapBase + Page + 0x10);
    EXPECT_EQ(info.base, layout::HeapBase);
    EXPECT_EQ(info.size, 2 * Page) << "the third page differs in access";
    EXPECT_EQ(info.state, MemoryState::Private);
    EXPECT_EQ(info.access, Access::ReadWrite);
}

TEST(AddressSpace, HandsOutThreadLocalStorageEightToAPage) {
    AddressSpace space = MakeSpace();
    for (std::uint32_t slot = 0; slot < 8; ++slot) {
        EXPECT_EQ(space.AllocateThreadLocalStorage(),
                  layout::ThreadLocalStorageBase + (slot * layout::ThreadLocalStorageSize));
    }
    EXPECT_EQ(space.AllocateThreadLocalStorage(), layout::ThreadLocalStorageBase + Page);
    EXPECT_EQ(space.StateAt(layout::ThreadLocalStorageBase), MemoryState::Locked);

    space.FreeThreadLocalStorage(layout::ThreadLocalStorageBase + layout::ThreadLocalStorageSize);
    EXPECT_EQ(space.AllocateThreadLocalStorage(),
              layout::ThreadLocalStorageBase + layout::ThreadLocalStorageSize)
        << "a freed slot is reused before a new page is mapped";
}

TEST(AddressSpace, MapsAForeignBackingAtAFreeRange) {
    AddressSpace space = MakeSpace();
    const auto block = memory::Backing::Allocate(Page);
    ASSERT_EQ(space.MapBacking(
                  layout::SharedMemoryBase, Page, block, 0, Access::Read, MemoryState::Shared),
              result::Success);
    constexpr std::uint8_t Marker = 0x42;
    block->Bytes()[0] = Marker;
    EXPECT_EQ(space.Memory().Read8(layout::SharedMemoryBase), Marker);
    EXPECT_EQ(space.MapBacking(
                  layout::SharedMemoryBase, Page, block, 0, Access::Read, MemoryState::Shared),
              result::InvalidAddressState);
    EXPECT_EQ(space.UnmapBacking(layout::SharedMemoryBase, Page), result::Success);
    EXPECT_EQ(space.StateAt(layout::SharedMemoryBase), MemoryState::Free);
}

}  // namespace
}  // namespace hexlith::domain::kernel
