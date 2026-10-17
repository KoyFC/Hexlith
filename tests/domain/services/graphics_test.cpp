#include "domain/services/graphics.h"

#include "service_harness.h"

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

TEST(Graphics, CompletesAQueuedMemoryFillAtOnce) {
    constexpr std::uint32_t FillStart = 0x1F000000;
    Harness gsp;
    gsp.Register();
    ASSERT_TRUE(gsp.harness.Send(gsp.service, {AcquireRight, 0, ipc::CopyHandlesDescriptor(1), 0}));
    gsp.PutWord(FirstCommand, MemoryFill);
    gsp.PutWord(FirstCommand + 4, FillStart);
    gsp.PutWord(CommandQueue, 1U << 8U);  // one command queued, at index 0

    ASSERT_TRUE(gsp.harness.Send(gsp.service, {TriggerCmdReqQueue}));
    EXPECT_EQ(gsp.Shared()[QueueCount], 1);
    EXPECT_EQ(gsp.Shared()[QueueSlots], Psc0);
    EXPECT_EQ(gsp.Shared()[CommandQueue + 1], 0) << "the queue is empty";
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
