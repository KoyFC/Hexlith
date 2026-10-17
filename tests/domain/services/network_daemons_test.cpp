#include "domain/services/network_daemons.h"

#include "service_harness.h"

#include <cstdint>
#include <gtest/gtest.h>

namespace hexlith::domain::services {
namespace {

using testing::ServiceHarness;
namespace ipc = kernel::ipc;

// ndm:u command headers. Source: 3dbrew, NDM Services.
constexpr std::uint32_t EnterExclusiveState = 0x00010042;
constexpr std::uint32_t SuspendDaemons = 0x00060040;
constexpr std::uint32_t ResumeDaemons = 0x00070040;
constexpr std::uint32_t OverrideDefaultDaemons = 0x00140040;

// The BOSS daemon's bit, and the friends daemon's.
constexpr std::uint32_t Boss = 1U << 1U;
constexpr std::uint32_t Friends = 1U << 2U;

TEST(NetworkDaemonManager, CountsSuspensionsPerDaemon) {
    ServiceHarness harness;
    NetworkDaemonManager ndm;
    ASSERT_TRUE(harness.Send(ndm, {SuspendDaemons, Boss | Friends}));
    ASSERT_TRUE(harness.Send(ndm, {SuspendDaemons, Boss}));
    EXPECT_EQ(ndm.SuspendCount(1), 2U);
    EXPECT_EQ(ndm.SuspendCount(2), 1U);
    EXPECT_EQ(ndm.SuspendCount(0), 0U);

    ASSERT_TRUE(harness.Send(ndm, {ResumeDaemons, Boss | Friends}));
    EXPECT_EQ(harness.Reply(1), kernel::result::Success.raw);
    EXPECT_EQ(ndm.SuspendCount(1), 1U);
    EXPECT_EQ(ndm.SuspendCount(2), 0U);
}

TEST(NetworkDaemonManager, RefusesToResumeADaemonThatIsNotSuspended) {
    ServiceHarness harness;
    NetworkDaemonManager ndm;
    ASSERT_TRUE(harness.Send(ndm, {SuspendDaemons, Boss}));
    ASSERT_TRUE(harness.Send(ndm, {ResumeDaemons, Boss | Friends}));
    EXPECT_EQ(harness.Reply(1), NetworkDaemonManager::NotSuspended.raw);
    EXPECT_EQ(ndm.SuspendCount(1), 1U) << "nothing is resumed";
}

TEST(NetworkDaemonManager, RemembersTheDefaultDaemons) {
    ServiceHarness harness;
    NetworkDaemonManager ndm;
    ASSERT_TRUE(harness.Send(ndm, {OverrideDefaultDaemons, Friends | 0xF0U}));
    EXPECT_EQ(ndm.DefaultDaemons(), Friends);
}

TEST(NetworkDaemonManager, EntersAnExclusiveState) {
    constexpr std::uint32_t LocalCommunication = 2;
    ServiceHarness harness;
    NetworkDaemonManager ndm;
    ASSERT_TRUE(harness.Send(
        ndm, {EnterExclusiveState, LocalCommunication, ipc::CallingProcessIdDescriptor, 0}));
    EXPECT_EQ(harness.Reply(1), kernel::result::Success.raw);
}

}  // namespace
}  // namespace hexlith::domain::services
