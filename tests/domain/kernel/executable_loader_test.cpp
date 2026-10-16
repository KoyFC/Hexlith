#include "domain/kernel/executable_loader.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::kernel {
namespace {

using memory::Access;

constexpr std::uint32_t Page = layout::PageSize;
constexpr std::uint32_t RegionSize = 64 * Page;

constexpr std::uint32_t TextAddress = 0x00100000;
constexpr std::uint32_t ReadOnlyAddress = TextAddress + (2 * Page);
constexpr std::uint32_t DataAddress = ReadOnlyAddress + Page;
constexpr std::uint32_t BssSize = Page + 0x800;
constexpr std::uint32_t StackSize = 4 * Page;
constexpr std::uint8_t Priority = 0x30;

// A word placed at the start of each segment, so a test can see which bytes
// went where.
constexpr std::uint32_t TextMarker = 0xE12FFF1E;
constexpr std::uint32_t ReadOnlyMarker = 0x52524F44;
constexpr std::uint32_t DataMarker = 0x44415441;

// Segments that do not fill their last page, as real ones rarely do.
constexpr std::uint32_t TextSize = Page + 0x10;
constexpr std::uint32_t ReadOnlySize = 0x20;
constexpr std::uint32_t DataSize = 0x30;

// The same page size, as a host size for vectors and offsets.
constexpr std::size_t PageBytes = Page;

ctr::ExHeader Header() {
    ctr::ExHeader header;
    header.text = {.address = TextAddress, .page_count = 2, .size = TextSize};
    header.read_only = {.address = ReadOnlyAddress, .page_count = 1, .size = ReadOnlySize};
    header.data = {.address = DataAddress, .page_count = 1, .size = DataSize};
    header.bss_size = BssSize;
    header.stack_size = StackSize;
    header.main_thread_priority = Priority;
    return header;
}

void PutWord(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
        bytes.at(offset + byte) = static_cast<std::uint8_t>(value >> (8 * byte));
    }
}

std::vector<std::uint8_t> Code() {
    std::vector<std::uint8_t> code(4 * PageBytes, 0);
    PutWord(code, 0, TextMarker);
    PutWord(code, 2 * PageBytes, ReadOnlyMarker);
    PutWord(code, 3 * PageBytes, DataMarker);
    return code;
}

TEST(LoadExecutable, PutsEachSegmentWhereTheHeaderSaysWithItsAccess) {
    AddressSpace space(RegionSize, layout::OldLinearHeapBase);
    const auto code = Code();
    const core::Result<MainThreadStart> start =
        LoadExecutable(space, Header(), code, MakeSystemPages(0, ProductionMemoryRegions));
    ASSERT_TRUE(start.Ok()) << start.Failure().Message();

    const memory::ProcessMemory& memory = space.Memory();
    EXPECT_EQ(memory.Read32(TextAddress), TextMarker);
    EXPECT_EQ(memory.Read32(ReadOnlyAddress), ReadOnlyMarker);
    EXPECT_EQ(memory.Read32(DataAddress), DataMarker);

    EXPECT_EQ(memory.AccessAt(TextAddress), Access::ReadExecute);
    EXPECT_EQ(memory.AccessAt(ReadOnlyAddress), Access::Read);
    EXPECT_EQ(memory.AccessAt(DataAddress), Access::ReadWrite);
    EXPECT_EQ(space.StateAt(TextAddress), MemoryState::Code);
    EXPECT_EQ(space.StateAt(ReadOnlyAddress), MemoryState::Code);
    EXPECT_EQ(space.StateAt(DataAddress), MemoryState::Private);
}

TEST(LoadExecutable, PutsTheBssAfterTheDataRoundedToPages) {
    AddressSpace space(RegionSize, layout::OldLinearHeapBase);
    const auto code = Code();
    ASSERT_TRUE(
        LoadExecutable(space, Header(), code, MakeSystemPages(0, ProductionMemoryRegions)).Ok());

    // One page of data, then 0x1800 bytes of BSS in two pages.
    const memory::ProcessMemory& memory = space.Memory();
    EXPECT_EQ(memory.Read32(DataAddress + Page), 0U);
    EXPECT_EQ(memory.Read32(DataAddress + (3 * Page) - 4), 0U);
    EXPECT_FALSE(memory.IsMapped(DataAddress + (3 * Page)));
}

TEST(LoadExecutable, PutsTheStackAtTheTopOfTheHeapRange) {
    AddressSpace space(RegionSize, layout::OldLinearHeapBase);
    const auto code = Code();
    const core::Result<MainThreadStart> start =
        LoadExecutable(space, Header(), code, MakeSystemPages(0, ProductionMemoryRegions));
    ASSERT_TRUE(start.Ok());

    EXPECT_EQ(start.Value().stack_top, 0x10000000U);
    EXPECT_EQ(space.StateAt(start.Value().stack_top - StackSize), MemoryState::Locked);
    EXPECT_EQ(space.Memory().AccessAt(start.Value().stack_top - 4), Access::ReadWrite);
    EXPECT_FALSE(space.Memory().IsMapped(start.Value().stack_top - StackSize - Page));

    EXPECT_EQ(start.Value().entry_point, TextAddress);
    EXPECT_EQ(start.Value().priority, Priority);
    EXPECT_EQ(start.Value().fpscr, MainThreadFpscr);
}

TEST(LoadExecutable, MapsTheSystemPagesReadOnly) {
    AddressSpace space(RegionSize, layout::OldLinearHeapBase);
    const auto code = Code();
    ASSERT_TRUE(
        LoadExecutable(space, Header(), code, MakeSystemPages(0, ProductionMemoryRegions)).Ok());

    const memory::ProcessMemory& memory = space.Memory();
    EXPECT_EQ(memory.Read8(layout::ConfigMemory + config_memory::KernelVersionMajor), 2);
    EXPECT_EQ(memory.Read32(layout::ConfigMemory + config_memory::ApplicationMemoryAllocated),
              ProductionMemoryRegions.application);
    EXPECT_EQ(memory.Read8(layout::SharedPage + shared_page::RunningHardware), 1);
    EXPECT_FALSE(space.Memory().Write8(layout::ConfigMemory, 0));
    EXPECT_FALSE(space.Memory().Write8(layout::SharedPage, 0));
}

TEST(LoadExecutable, RefusesCodeOfTheWrongSize) {
    AddressSpace space(RegionSize, layout::OldLinearHeapBase);
    const std::vector<std::uint8_t> short_code(3 * PageBytes, 0);
    EXPECT_FALSE(
        LoadExecutable(space, Header(), short_code, MakeSystemPages(0, ProductionMemoryRegions))
            .Ok());
}

TEST(LoadExecutable, RefusesARegionTooSmallToHoldIt) {
    AddressSpace space(4 * Page, layout::OldLinearHeapBase);
    const auto code = Code();
    const core::Result<MainThreadStart> start =
        LoadExecutable(space, Header(), code, MakeSystemPages(0, ProductionMemoryRegions));
    ASSERT_FALSE(start.Ok());
    EXPECT_NE(start.Failure().Message().find("0xC860180A"), std::string::npos)
        << start.Failure().Message();
}

}  // namespace
}  // namespace hexlith::domain::kernel
