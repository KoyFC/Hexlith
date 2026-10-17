#include "domain/services/human_interface.h"

#include "service_harness.h"

#include <bit>
#include <cstdint>
#include <gtest/gtest.h>

namespace hexlith::domain::services {
namespace {

using testing::ServiceHarness;
namespace ipc = kernel::ipc;

// hid:USER command headers. Source: 3dbrew, HID Services.
constexpr std::uint32_t GetIpcHandles = 0x000A0000;
constexpr std::uint32_t GetGyroscopeCoefficient = 0x00150000;
constexpr std::uint32_t GetSoundVolume = 0x00170000;

// The reply to GetIPCHandles: the result, a descriptor for six handles, the
// shared page's handle and then the five events'.
constexpr std::uint32_t HandleCount = 6;
constexpr std::size_t FirstEvent = 4;
constexpr std::size_t EventCount = 5;

TEST(HumanInterface, HandsOutTheSharedPage) {
    ServiceHarness harness;
    HumanInterface hid(harness.Kernel());
    ASSERT_TRUE(harness.Send(hid, {GetIpcHandles}));
    EXPECT_EQ(harness.Reply(0), ipc::MakeHeader(0x000A, 1, 1 + HandleCount));
    EXPECT_EQ(harness.Reply(2), ipc::CopyHandlesDescriptor(HandleCount));
    EXPECT_EQ(harness.ReplyObject<kernel::SharedMemory>(3, kernel::ObjectType::SharedMemory),
              hid.SharedPage());
    EXPECT_EQ(hid.SharedPage()->size, HumanInterface::SharedPageSize);
}

TEST(HumanInterface, HandsOutFiveEvents) {
    ServiceHarness harness;
    HumanInterface hid(harness.Kernel());
    ASSERT_TRUE(harness.Send(hid, {GetIpcHandles}));
    for (std::size_t index = FirstEvent; index < FirstEvent + EventCount; ++index) {
        EXPECT_NE(harness.ReplyObject<kernel::Event>(index, kernel::ObjectType::Event), nullptr);
    }
}

TEST(HumanInterface, AnswersTheGyroscopeCoefficient) {
    constexpr float DegreesPerUnit = 14.375F;
    ServiceHarness harness;
    HumanInterface hid(harness.Kernel());
    ASSERT_TRUE(harness.Send(hid, {GetGyroscopeCoefficient}));
    EXPECT_EQ(std::bit_cast<float>(harness.Reply(2)), DegreesPerUnit);
}

TEST(HumanInterface, AnswersTheVolumeAtItsTop) {
    constexpr std::uint32_t FullVolume = 0x3F;
    ServiceHarness harness;
    HumanInterface hid(harness.Kernel());
    ASSERT_TRUE(harness.Send(hid, {GetSoundVolume}));
    EXPECT_EQ(harness.Reply(2), FullVolume);
}

}  // namespace
}  // namespace hexlith::domain::services
