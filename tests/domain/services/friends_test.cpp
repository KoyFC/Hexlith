#include "domain/services/friends.h"
#include "domain/services/spot_pass.h"

#include "service_harness.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::services {
namespace {

using testing::ServiceHarness;
namespace ipc = kernel::ipc;

// frd:u and boss:U command headers. Source: 3dbrew, Friend Services and BOSS
// Services.
constexpr std::uint32_t HasLoggedIn = 0x00010000;
constexpr std::uint32_t GetMyFriendKey = 0x00050000;
constexpr std::uint32_t GetFriendKeyList = 0x00110080;
constexpr std::uint32_t InitializeSession = 0x00010082;
constexpr std::uint32_t RegisterStorageEntry = 0x002F0140;
constexpr std::uint32_t GetStorageEntryInfo = 0x00300000;

TEST(Friends, IsNeverLoggedIn) {
    ServiceHarness harness;
    Friends frd;
    ASSERT_TRUE(harness.Send(frd, {HasLoggedIn}));
    EXPECT_EQ(harness.Reply(1), kernel::result::Success.raw);
    EXPECT_EQ(harness.Reply(2), 0U);
}

TEST(Friends, HasAZeroFriendKey) {
    ServiceHarness harness;
    Friends frd;
    ASSERT_TRUE(harness.Send(frd, {GetMyFriendKey}));
    EXPECT_EQ(harness.Reply(0), ipc::MakeHeader(0x0005, 5, 0));
    // Four words after the result.
    constexpr std::size_t FirstKeyWord = 2;
    constexpr std::size_t KeyWords = 4;
    for (std::size_t index = FirstKeyWord; index < FirstKeyWord + KeyWords; ++index) {
        EXPECT_EQ(harness.Reply(index), 0U);
    }
}

TEST(Friends, HasNoFriends) {
    constexpr std::uint32_t Capacity = 0x20;
    constexpr std::uint32_t Keys = ServiceHarness::BufferAddress;
    ServiceHarness harness;
    harness.ListReceiveBuffer(0, Keys, Capacity);
    constexpr std::uint8_t Filler = 0xFF;
    harness.WriteBytes(Keys, std::vector<std::uint8_t>(Capacity, Filler));
    Friends frd;
    ASSERT_TRUE(harness.Send(frd, {GetFriendKeyList, 0, 2}));
    EXPECT_EQ(harness.Reply(2), 0U);
    EXPECT_EQ(harness.ReadBytes(Keys, Capacity), std::vector<std::uint8_t>(Capacity, 0));
}

TEST(SpotPass, OpensASessionThatNeverDownloads) {
    ServiceHarness harness;
    SpotPass boss;
    ASSERT_TRUE(harness.Send(boss, {InitializeSession, 0, 0, ipc::CallingProcessIdDescriptor, 0}));
    EXPECT_EQ(harness.Reply(1), kernel::result::Success.raw);
    EXPECT_TRUE(boss.HasSession());

    ASSERT_TRUE(harness.Send(boss, {RegisterStorageEntry, 0x55D, 0, 0x00100000, 0x14, 1}));
    EXPECT_EQ(harness.Reply(1), kernel::result::Success.raw);
    ASSERT_TRUE(harness.Send(boss, {GetStorageEntryInfo}));
    EXPECT_EQ(harness.Reply(0), ipc::MakeHeader(0x0030, 3, 0));
    EXPECT_EQ(harness.Reply(2), 0U);
}

}  // namespace
}  // namespace hexlith::domain::services
