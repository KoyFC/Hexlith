#include "domain/arm/interpreter/interpreter.h"
#include "domain/memory/process_memory.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::memory {
namespace {

constexpr std::uint32_t Base = 0x00100000;
constexpr std::uint32_t Page = ProcessMemory::PageSize;
constexpr std::uint32_t Elsewhere = 0x006A5000;

TEST(ProcessMemory, ReadsBackWhatWasWrittenLittleEndian) {
    ProcessMemory memory;
    ASSERT_TRUE(memory.Map(Base, Page, Backing::Allocate(Page), 0, Access::ReadWrite));

    ASSERT_TRUE(memory.Write32(Base, 0x11223344));
    EXPECT_EQ(memory.Read8(Base), 0x44);
    EXPECT_EQ(memory.Read16(Base + 2), 0x1122);
    ASSERT_TRUE(memory.Write64(Base + 8, 0x0102030405060708ULL));
    EXPECT_EQ(memory.Read32(Base + 8), 0x05060708U) << "the low word comes first";
    EXPECT_EQ(memory.Read64(Base + 8), 0x0102030405060708ULL);
}

TEST(ProcessMemory, AnAliasSeesTheSameBytes) {
    // What ldr:ro does to a module: the buffer it was read into and the address
    // it runs at are two names for one memory.
    ProcessMemory memory;
    const auto backing = Backing::Allocate(2 * Page);
    ASSERT_TRUE(memory.Map(Base, 2 * Page, backing, 0, Access::ReadWrite));
    ASSERT_TRUE(memory.Map(Elsewhere, Page, backing, Page, Access::ReadWrite));

    ASSERT_TRUE(memory.Write32(Base + Page + 4, 0xCAFEF00D));
    EXPECT_EQ(memory.Read32(Elsewhere + 4), 0xCAFEF00DU);
    ASSERT_TRUE(memory.Write8(Elsewhere, 0x5A));
    EXPECT_EQ(memory.Read8(Base + Page), 0x5A);
}

TEST(ProcessMemory, AnAccessStraddlingTwoPagesReachesBothBackings) {
    ProcessMemory memory;
    ASSERT_TRUE(memory.Map(Base, Page, Backing::Allocate(Page), 0, Access::ReadWrite));
    ASSERT_TRUE(memory.Map(Base + Page, Page, Backing::Allocate(Page), 0, Access::ReadWrite));

    ASSERT_TRUE(memory.Write32(Base + Page - 2, 0xAABBCCDD));
    EXPECT_EQ(memory.Read16(Base + Page - 2), 0xCCDD);
    EXPECT_EQ(memory.Read16(Base + Page), 0xAABB);
    EXPECT_EQ(memory.Read32(Base + Page - 2), 0xAABBCCDDU);
}

TEST(ProcessMemory, ChecksWhatEachPageAllows) {
    ProcessMemory memory;
    ASSERT_TRUE(memory.Map(Base, Page, Backing::Allocate(Page), 0, Access::ReadExecute));
    ASSERT_TRUE(memory.Map(Base + Page, Page, Backing::Allocate(Page), 0, Access::None));

    EXPECT_TRUE(memory.Read32(Base).has_value());
    EXPECT_FALSE(memory.Write32(Base, 1)) << "code is read-only";
    EXPECT_FALSE(memory.Read8(Base + Page).has_value()) << "a locked page answers nothing";

    ASSERT_TRUE(memory.Protect(Base, Page, Access::ReadWrite));
    EXPECT_TRUE(memory.Write32(Base, 1));
}

TEST(ProcessMemory, AFailedStraddlingWriteChangesNothing) {
    ProcessMemory memory;
    ASSERT_TRUE(memory.Map(Base, Page, Backing::Allocate(Page), 0, Access::ReadWrite));
    ASSERT_TRUE(memory.Map(Base + Page, Page, Backing::Allocate(Page), 0, Access::Read));

    EXPECT_FALSE(memory.Write32(Base + Page - 2, 0xFFFFFFFF));
    EXPECT_EQ(memory.Read16(Base + Page - 2), 0) << "the writable half was left alone";
}

TEST(ProcessMemory, LoadWritesWhatAccessForbids) {
    // The kernel copies the executable into pages the game cannot write.
    ProcessMemory memory;
    ASSERT_TRUE(memory.Map(Base, Page, Backing::Allocate(Page), 0, Access::ReadExecute));
    const std::array<std::uint8_t, 4> word = {0x1E, 0xFF, 0x2F, 0xE1};
    ASSERT_TRUE(memory.Load(Base, word));
    EXPECT_EQ(memory.Read32(Base), 0xE12FFF1EU);

    std::array<std::uint8_t, 4> copied{};
    ASSERT_TRUE(memory.Copy(Base, copied));
    EXPECT_EQ(copied, word);
}

TEST(ProcessMemory, RefusesAMisalignedOrOverlappingMap) {
    ProcessMemory memory;
    const auto backing = Backing::Allocate(2 * Page);
    EXPECT_FALSE(memory.Map(Base + 4, Page, backing, 0, Access::Read));
    EXPECT_FALSE(memory.Map(Base, Page, backing, 4, Access::Read));
    EXPECT_FALSE(memory.Map(Base, 3 * Page, backing, 0, Access::Read))
        << "the backing does not hold three pages";
    ASSERT_TRUE(memory.Map(Base, Page, backing, 0, Access::Read));
    EXPECT_FALSE(memory.Map(Base, Page, backing, Page, Access::Read));
}

TEST(ProcessMemory, AnUnmappedAliasLeavesTheOtherMappingWorking) {
    ProcessMemory memory;
    {
        const auto backing = Backing::Allocate(Page);
        ASSERT_TRUE(memory.Map(Base, Page, backing, 0, Access::ReadWrite));
        ASSERT_TRUE(memory.Map(Elsewhere, Page, backing, 0, Access::ReadWrite));
    }
    ASSERT_TRUE(memory.Write32(Base, 7));
    ASSERT_TRUE(memory.Unmap(Base, Page));

    EXPECT_FALSE(memory.IsMapped(Base));
    EXPECT_EQ(memory.Read32(Elsewhere), 7U) << "the mapping keeps its backing alive";
    EXPECT_FALSE(memory.Unmap(Base, Page)) << "nothing is there to unmap";
}

TEST(ProcessMemory, ReportsWhereAPagePoints) {
    ProcessMemory memory;
    const auto backing = Backing::Allocate(2 * Page);
    ASSERT_TRUE(memory.Map(Base, Page, backing, Page, Access::Read));

    const auto target = memory.TargetOf(Base + 0x10);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(target.value_or(PageTarget{}).backing, backing);
    EXPECT_EQ(target.value_or(PageTarget{}).offset, Page + 0x10);
    EXPECT_FALSE(memory.TargetOf(Elsewhere).has_value());
}

TEST(ProcessMemory, TheInterpreterRunsOnIt) {
    // mov r0, #5
    constexpr std::uint32_t MoveFive = 0xE3A00005;
    // svc 0x32
    constexpr std::uint32_t SendSyncRequest = 0xEF000032;

    ProcessMemory memory;
    ASSERT_TRUE(memory.Map(Base, Page, Backing::Allocate(Page), 0, Access::ReadExecute));
    const std::vector<std::uint8_t> code = {0x05, 0x00, 0xA0, 0xE3, 0x32, 0x00, 0x00, 0xEF};
    ASSERT_TRUE(memory.Load(Base, code));
    ASSERT_EQ(memory.Read32(Base), MoveFive);
    ASSERT_EQ(memory.Read32(Base + 4), SendSyncRequest);

    arm::CpuState state;
    state.registers.at(arm::ProgramCounter) = Base;
    const arm::Stop stop = arm::Run(state, memory, 10);
    EXPECT_EQ(stop.reason, arm::StopReason::SupervisorCall);
    EXPECT_EQ(stop.detail, 0x32U);
    EXPECT_EQ(state.registers.at(0), 5U);
}

}  // namespace
}  // namespace hexlith::domain::memory
