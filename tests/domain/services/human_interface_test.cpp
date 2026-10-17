#include "core/byte_reader.h"
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

// Offsets into the shared page. Source: SharedMem in Azahar's hid.h; 3dbrew,
// HID Shared Memory.
constexpr std::uint32_t PadResetTicks = 0x00;
constexpr std::uint32_t PadIndex = 0x10;
constexpr std::uint32_t PadCurrentState = 0x1C;
constexpr std::uint32_t PadEntries = 0x28;
constexpr std::uint32_t PadEntrySize = 0x10;
constexpr std::uint32_t TouchIndex = 0xB8;
constexpr std::uint32_t TouchEntries = 0xC8;

std::uint32_t PageWord(const HumanInterface& hid, std::uint32_t offset) {
    return core::ReadU32At(hid.SharedPage()->pages.front().backing->Bytes(), offset).ValueOr(0);
}

InputState Holding(std::uint32_t buttons) {
    InputState state;
    state.buttons = buttons;
    return state;
}

InputState CircleAt(std::int16_t x, std::int16_t y) {
    InputState state;
    state.circle_x = x;
    state.circle_y = y;
    return state;
}

// Runs the clock through a number of updates of the page.
void RunUpdates(ServiceHarness& harness, std::uint64_t updates) {
    harness.Kernel().Threads().Advance(updates * HumanInterface::PadUpdateTicks);
}

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

TEST(HumanInterface, FillsThePageFromItsInput) {
    ServiceHarness harness;
    HumanInterface hid(harness.Kernel());
    hid.SetInput([] { return Holding(button::A | button::Up); });

    harness.Kernel().Threads().Advance(HumanInterface::PadUpdateTicks - 1);
    EXPECT_EQ(PageWord(hid, PadCurrentState), 0U) << "nothing before the first update";
    RunUpdates(harness, 1);
    EXPECT_EQ(PageWord(hid, PadCurrentState), button::A | button::Up);
    EXPECT_EQ(PageWord(hid, PadIndex), 0U);
    EXPECT_EQ(PageWord(hid, PadEntries), button::A | button::Up);
    EXPECT_EQ(PageWord(hid, PadEntries + 4), button::A | button::Up) << "newly pressed";
    EXPECT_EQ(PageWord(hid, PadResetTicks), HumanInterface::PadUpdateTicks)
        << "stamped when the ring starts over";
}

TEST(HumanInterface, RecordsWhatChangedBetweenTwoEntries) {
    ServiceHarness harness;
    HumanInterface hid(harness.Kernel());
    std::uint32_t held = button::A;
    hid.SetInput([&held] { return Holding(held); });
    RunUpdates(harness, 1);
    held = button::B;
    RunUpdates(harness, 1);

    const std::uint32_t second = PadEntries + PadEntrySize;
    EXPECT_EQ(PageWord(hid, PadIndex), 1U);
    EXPECT_EQ(PageWord(hid, second), button::B);
    EXPECT_EQ(PageWord(hid, second + 4), button::B) << "pressed since the last entry";
    EXPECT_EQ(PageWord(hid, second + 8), button::A) << "released since the last entry";
}

TEST(HumanInterface, GoesRoundARingOfEightEntries) {
    ServiceHarness harness;
    const HumanInterface hid(harness.Kernel());
    constexpr std::uint64_t PastTheRing = 9;
    RunUpdates(harness, PastTheRing);
    EXPECT_EQ(PageWord(hid, PadIndex), 0U);
    EXPECT_EQ(PageWord(hid, TouchIndex), 0U);
    EXPECT_EQ(PageWord(hid, PadResetTicks), PastTheRing * HumanInterface::PadUpdateTicks)
        << "the second time round";
}

TEST(HumanInterface, TurnsTheCirclePadIntoDirections) {
    constexpr std::int16_t Rim = 0x9A;
    constexpr std::int16_t Inside = 20;
    ServiceHarness harness;
    HumanInterface hid(harness.Kernel());
    InputState state = CircleAt(Rim, 0);
    hid.SetInput([&state] { return state; });
    RunUpdates(harness, 1);
    EXPECT_EQ(PageWord(hid, PadCurrentState), button::CircleRight);
    EXPECT_EQ(PageWord(hid, PadEntries + 0xC), static_cast<std::uint32_t>(Rim));

    state = CircleAt(Rim, Rim);
    RunUpdates(harness, 1);
    EXPECT_EQ(PageWord(hid, PadCurrentState), button::CircleRight | button::CircleUp)
        << "a diagonal is two directions";

    state = CircleAt(Inside, -Inside);
    RunUpdates(harness, 1);
    EXPECT_EQ(PageWord(hid, PadCurrentState), 0U) << "too close to the centre";
}

TEST(HumanInterface, WritesWhereTheStylusTouches) {
    constexpr std::uint16_t X = 160;
    constexpr std::uint16_t Y = 120;
    ServiceHarness harness;
    HumanInterface hid(harness.Kernel());
    hid.SetInput([] {
        InputState state;
        state.touch = TouchPoint{.x = X, .y = Y};
        return state;
    });
    RunUpdates(harness, 1);
    EXPECT_EQ(PageWord(hid, TouchEntries), static_cast<std::uint32_t>(X) | (Y << 16U));
    EXPECT_EQ(PageWord(hid, TouchEntries + 4), 1U);

    hid.SetInput([] { return InputState{}; });
    RunUpdates(harness, 1);
    EXPECT_EQ(PageWord(hid, TouchEntries + 8 + 4), 0U) << "the stylus is up";
}

}  // namespace
}  // namespace hexlith::domain::services
