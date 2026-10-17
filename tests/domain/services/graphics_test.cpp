#include "domain/services/graphics.h"

#include "service_harness.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::services {
namespace {

using testing::ServiceHarness;
namespace ipc = kernel::ipc;

// gsp::Gpu command headers. Source: 3dbrew, GSP Services.
constexpr std::uint32_t WriteHwRegs = 0x00010082;
constexpr std::uint32_t ReadHwRegs = 0x00040080;
constexpr std::uint32_t TriggerCmdReqQueue = 0x000C0000;
constexpr std::uint32_t RegisterInterruptRelayQueue = 0x00130042;
constexpr std::uint32_t AcquireRight = 0x00160042;

// The first client's interrupt queue at 0, its command queue at 0x800, the
// commands from 0x20 in it. Source: Azahar's gsp_gpu.cpp and gsp_command.h.
constexpr std::uint32_t QueueCount = 1;
constexpr std::uint32_t QueueSlots = 0xC;
constexpr std::uint32_t CommandQueue = 0x800;
constexpr std::uint32_t FirstCommand = CommandQueue + 0x20;
constexpr std::uint8_t MemoryFill = 2;

constexpr std::uint8_t Pdc0 = 2;
constexpr std::uint8_t Pdc1 = 3;
constexpr std::uint8_t Psc0 = 0;

struct Harness {
    Harness() : service(harness.Kernel()) {}

    std::shared_ptr<kernel::Event> Register() {
        auto event = std::make_shared<kernel::Event>(kernel::ResetType::OneShot);
        const kernel::Handle handle = harness.Kernel().Handles().Create(event).value_or(0);
        EXPECT_TRUE(harness.Send(
            service, {RegisterInterruptRelayQueue, 1, ipc::CopyHandlesDescriptor(1), handle}));
        return event;
    }

    [[nodiscard]] std::span<std::uint8_t> Shared() const {
        return service.SharedBlock()->pages.front().backing->Bytes();
    }

    void PutWord(std::uint32_t offset, std::uint32_t value) const {
        for (unsigned byte = 0; byte < 4; ++byte) {
            Shared()[offset + byte] = static_cast<std::uint8_t>(value >> (8U * byte));
        }
    }

    ServiceHarness harness;
    Graphics service;
};

TEST(Graphics, TellsTheFirstClientToInitializeTheGpu) {
    Harness gsp;
    gsp.Register();
    EXPECT_EQ(gsp.harness.Reply(1), Graphics::FirstInitialization.raw);
    EXPECT_EQ(gsp.harness.Reply(2), 0U) << "the first client's queue index";
    EXPECT_EQ(gsp.harness.ReplyObject<kernel::SharedMemory>(4, kernel::ObjectType::SharedMemory),
              gsp.service.SharedBlock());
    gsp.Register();
    EXPECT_EQ(gsp.harness.Reply(1), kernel::result::Success.raw);
    EXPECT_EQ(gsp.harness.Reply(2), 1U);
}

TEST(Graphics, QueuesBothVerticalBlanksEachFrame) {
    Harness gsp;
    const auto event = gsp.Register();
    gsp.harness.Kernel().Threads().Advance(Graphics::FrameTicks - 1);
    EXPECT_FALSE(event->signaled);
    gsp.harness.Kernel().Threads().Advance(1);
    EXPECT_TRUE(event->signaled);
    EXPECT_EQ(gsp.Shared()[QueueCount], 2);
    EXPECT_EQ(gsp.Shared()[QueueSlots], Pdc0);
    EXPECT_EQ(gsp.Shared()[QueueSlots + 1], Pdc1);
}

// The time every request costs, 300 microseconds, in ticks. Source:
// sync_delay_nanoseconds in Azahar's gsp_gpu.cpp.
constexpr std::uint64_t SyncTicks = 300'000ULL * kernel::Arm11TicksPerSecond / 1'000'000'000ULL;

constexpr std::uint8_t TextureCopy = 4;
constexpr std::uint8_t Ppf = 4;
constexpr std::uint32_t Area = ServiceHarness::BufferAddress;
constexpr std::uint32_t Fill32Bit = 1U << 9U;

// Takes the right, queues commands from the given words, eight each, and
// triggers the queue.
void RunCommands(Harness& gsp, const std::vector<std::array<std::uint32_t, 8>>& commands) {
    ASSERT_TRUE(gsp.harness.Send(gsp.service, {AcquireRight, 0, ipc::CopyHandlesDescriptor(1), 0}));
    for (std::size_t index = 0; index < commands.size(); ++index) {
        for (std::size_t word = 0; word < 8; ++word) {
            gsp.PutWord(FirstCommand + static_cast<std::uint32_t>((index * 0x20) + (word * 4)),
                        commands.at(index).at(word));
        }
    }
    gsp.PutWord(CommandQueue, static_cast<std::uint32_t>(commands.size()) << 8U);
    ASSERT_TRUE(gsp.harness.Send(gsp.service, {TriggerCmdReqQueue}));
}

TEST(Graphics, FillsMemoryAndRaisesTheInterruptOnceItsTimeHasPassed) {
    constexpr std::uint32_t Value = 0xAABBCCDD;
    Harness gsp;
    const auto event = gsp.Register();
    RunCommands(gsp, {{MemoryFill, Area, Value, Area + 16, 0, 0, 0, Fill32Bit}});
    EXPECT_EQ(gsp.harness.Read32(Area + 12), Value);
    EXPECT_EQ(gsp.Shared()[CommandQueue + 1], 0) << "the queue is empty";
    EXPECT_EQ(gsp.Shared()[QueueCount], 0) << "not raised yet";

    gsp.harness.Kernel().Threads().Advance(SyncTicks + 1);
    EXPECT_TRUE(event->signaled);
    EXPECT_EQ(gsp.Shared()[QueueCount], 1);
    EXPECT_EQ(gsp.Shared()[QueueSlots], Psc0);
}

TEST(Graphics, RaisesOnlyPsc0ForAFillOfTwoBuffers) {
    Harness gsp;
    gsp.Register();
    constexpr std::uint32_t Second = Area + 16;
    constexpr std::uint32_t Length = 8;
    RunCommands(gsp, {{MemoryFill, Area, 1, Area + Length, Second, 2, Second + Length, 0}});
    gsp.harness.Kernel().Threads().Advance((2 * SyncTicks) + 2);
    EXPECT_EQ(gsp.Shared()[QueueCount], 1);
    EXPECT_EQ(gsp.Shared()[QueueSlots], Psc0);
    EXPECT_EQ(gsp.harness.Read32(Area + 16), 0x00020002U)
        << "the second buffer is filled all the same, in halfwords";
}

TEST(Graphics, CopiesTextureRowsAcrossTheirGaps) {
    Harness gsp;
    gsp.Register();
    std::vector<std::uint8_t> rows(64);
    for (std::size_t byte = 0; byte < rows.size(); ++byte) {
        rows.at(byte) = static_cast<std::uint8_t>(byte);
    }
    gsp.harness.WriteBytes(Area, rows);
    // 32 bytes, read as rows of 16 with a gap of 16, written contiguously.
    constexpr std::uint32_t Output = Area + 0x200;
    RunCommands(gsp, {{TextureCopy, Area, Output, 32, 1U | (1U << 16U), 0, 0, 0}});
    EXPECT_EQ(gsp.harness.ReadBytes(Output + 16, 1), std::vector<std::uint8_t>{32})
        << "the second row comes from past the gap";
}

TEST(Graphics, FinishesOperationsInTheOrderTheyStarted) {
    Harness gsp;
    gsp.Register();
    // A long fill, then a short copy that would otherwise finish first.
    constexpr std::uint32_t LongFill = 0x800;
    constexpr std::uint32_t CopyTarget = Area + 0x900;
    RunCommands(gsp,
                {{MemoryFill, Area, 0, Area + LongFill, 0, 0, 0, 0},
                 {TextureCopy, Area, CopyTarget, 16, 0, 0, 0, 0}});
    gsp.harness.Kernel().Threads().Advance(4 * SyncTicks);
    EXPECT_EQ(gsp.Shared()[QueueCount], 2);
    EXPECT_EQ(gsp.Shared()[QueueSlots], Psc0);
    EXPECT_EQ(gsp.Shared()[QueueSlots + 1], Ppf);
}

TEST(Graphics, AnswersBackTheRegistersWritten) {
    constexpr std::uint32_t Register = 0x00400468;
    constexpr std::uint32_t Data = ServiceHarness::BufferAddress;
    constexpr std::uint32_t Received = ServiceHarness::BufferAddress + 0x100;
    Harness gsp;
    const std::vector<std::uint8_t> value = {0x78, 0x56, 0x34, 0x12};
    gsp.harness.WriteBytes(Data, value);
    ASSERT_TRUE(gsp.harness.Send(
        gsp.service, {WriteHwRegs, Register, 4, ipc::StaticBufferDescriptorFor(4, 0), Data}));
    gsp.harness.ListReceiveBuffer(0, Received, 8);
    ASSERT_TRUE(gsp.harness.Send(gsp.service, {ReadHwRegs, Register, 8}));
    EXPECT_EQ(gsp.harness.Read32(Received), 0x12345678U);
    EXPECT_EQ(gsp.harness.Read32(Received + 4), 0U) << "an unwritten register reads zero";
}

}  // namespace
}  // namespace hexlith::domain::services
