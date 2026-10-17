#include "domain/services/apt.h"
#include "domain/services/stand_in_font.h"

#include "service_harness.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::services {
namespace {

using testing::ServiceHarness;
namespace ipc = kernel::ipc;

// APT:U command headers. Source: 3dbrew, NS and APT Services.
constexpr std::uint32_t GetLockHandle = 0x00010040;
constexpr std::uint32_t Initialize = 0x00020080;
constexpr std::uint32_t InquireNotification = 0x000B0040;
constexpr std::uint32_t ReceiveParameter = 0x000D0080;
constexpr std::uint32_t GlanceParameter = 0x000E0080;
constexpr std::uint32_t GetSharedFont = 0x00440000;
constexpr std::uint32_t AppletUtility = 0x004B00C2;

constexpr std::uint32_t ParameterSize = 0x10;
constexpr std::uint32_t ReceiveAddress = ServiceHarness::BufferAddress + 0x100;

// Where the system region's first block sits with a 64-page application
// region: the old linear heap plus the application region.
constexpr std::uint32_t SystemRegionStart =
    kernel::layout::OldLinearHeapBase + (64 * kernel::layout::PageSize);

struct Harness {
    Harness() : service(harness.Kernel()) {
        harness.ListReceiveBuffer(0, ReceiveAddress, ParameterSize);
    }

    void Register() {
        ASSERT_TRUE(harness.Send(service, {Initialize, AppletManager::ApplicationId, 0}));
    }

    ServiceHarness harness;
    AppletManager service;
};

TEST(AppletManager, HandsOutItsLock) {
    Harness apt;
    ASSERT_TRUE(apt.harness.Send(apt.service, {GetLockHandle, 0}));
    EXPECT_EQ(apt.harness.Reply(0), ipc::MakeHeader(1, 3, 2));
    EXPECT_EQ(apt.harness.Reply(1), kernel::result::Success.raw);
    EXPECT_NE(apt.harness.ReplyObject<kernel::Mutex>(5, kernel::ObjectType::Mutex), nullptr);
}

TEST(AppletManager, WakesTheFirstAppletToRegister) {
    Harness apt;
    apt.Register();
    EXPECT_EQ(apt.harness.Reply(2), ipc::CopyHandlesDescriptor(2));
    const auto parameter_event =
        apt.harness.ReplyObject<kernel::Event>(4, kernel::ObjectType::Event);
    ASSERT_NE(parameter_event, nullptr);
    EXPECT_TRUE(parameter_event->signaled);

    ASSERT_TRUE(apt.harness.Send(apt.service,
                                 {ReceiveParameter, AppletManager::ApplicationId, ParameterSize}));
    EXPECT_EQ(apt.harness.Reply(1), kernel::result::Success.raw);
    EXPECT_EQ(apt.harness.Reply(3), AppletManager::WakeupSignal);
    EXPECT_EQ(apt.harness.Reply(4), 0U) << "the wakeup carries no data";
}

TEST(AppletManager, GlancesAtAParameterWithoutTakingIt) {
    Harness apt;
    apt.Register();
    ASSERT_TRUE(apt.harness.Send(apt.service,
                                 {GlanceParameter, AppletManager::ApplicationId, ParameterSize}));
    EXPECT_EQ(apt.harness.Reply(1), kernel::result::Success.raw);
    ASSERT_TRUE(apt.harness.Send(apt.service,
                                 {ReceiveParameter, AppletManager::ApplicationId, ParameterSize}));
    EXPECT_EQ(apt.harness.Reply(1), kernel::result::Success.raw);
    ASSERT_TRUE(apt.harness.Send(apt.service,
                                 {ReceiveParameter, AppletManager::ApplicationId, ParameterSize}));
    EXPECT_EQ(apt.harness.Reply(1), AppletManager::NoParameter.raw);
}

TEST(AppletManager, SendsNoNotification) {
    Harness apt;
    ASSERT_TRUE(apt.harness.Send(apt.service, {InquireNotification, AppletManager::ApplicationId}));
    EXPECT_EQ(apt.harness.Reply(2), 0U);
}

TEST(AppletManager, MapsTheStandInFontInTheSystemRegion) {
    Harness apt;
    ASSERT_TRUE(apt.harness.Send(apt.service, {GetSharedFont}));
    EXPECT_EQ(apt.harness.Reply(1), kernel::result::Success.raw);
    EXPECT_EQ(apt.harness.Reply(2), SystemRegionStart);
    const auto font =
        apt.harness.ReplyObject<kernel::SharedMemory>(4, kernel::ObjectType::SharedMemory);
    ASSERT_NE(font, nullptr);
    EXPECT_EQ(font->size, SharedFontBlockSize);
    EXPECT_EQ(font->pages.front().backing->Bytes()[0], 2) << "the header says it is loaded";

    ASSERT_TRUE(apt.harness.Send(apt.service, {GetSharedFont}));
    EXPECT_EQ(apt.harness.Reply(2), SystemRegionStart) << "the same block every time";
}

TEST(AppletManager, AnswersTheUtilityCommandsWithZerosAndOneTrue) {
    constexpr std::uint32_t OutputSize = 4;
    constexpr std::uint32_t UtilityWithTrue = 6;
    Harness apt;
    ASSERT_TRUE(apt.harness.Send(apt.service,
                                 {AppletUtility,
                                  UtilityWithTrue,
                                  0,
                                  OutputSize,
                                  ipc::StaticBufferDescriptorFor(0, 1),
                                  ServiceHarness::BufferAddress}));
    EXPECT_EQ(apt.harness.Reply(2), kernel::result::Success.raw);
    EXPECT_EQ(apt.harness.ReadBytes(ReceiveAddress, OutputSize),
              (std::vector<std::uint8_t>{1, 0, 0, 0}));
}

}  // namespace
}  // namespace hexlith::domain::services
