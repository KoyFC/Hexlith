#include "domain/kernel/handle_table.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <memory>

namespace hexlith::domain::kernel {
namespace {

constexpr std::size_t Capacity = 4;

std::shared_ptr<Event> AnEvent() {
    return std::make_shared<Event>(ResetType::OneShot);
}

TEST(HandleTable, EncodesSlotAndGenerationTheWayTheConsoleDoes) {
    HandleTable table(Capacity);
    // Slot 0 with generation 1, then slot 1 with generation 2.
    EXPECT_EQ(table.Create(AnEvent()), Handle{0x00000001});
    EXPECT_EQ(table.Create(AnEvent()), Handle{0x00008002});
}

TEST(HandleTable, ReusesASlotUnderANewGeneration) {
    HandleTable table(Capacity);
    const Handle first = table.Create(AnEvent()).value_or(0);
    ASSERT_TRUE(table.Close(first));
    EXPECT_EQ(table.Get(first), nullptr) << "a closed handle stays invalid";

    const Handle again = table.Create(AnEvent()).value_or(0);
    EXPECT_EQ(again, Handle{0x00000002}) << "slot 0, generation 2";
    EXPECT_NE(table.Get(again), nullptr);
    EXPECT_FALSE(table.Close(first));
}

TEST(HandleTable, ChecksTheTypeOfWhatAHandleRefersTo) {
    HandleTable table(Capacity);
    const auto event = AnEvent();
    const Handle handle = table.Create(event).value_or(0);
    EXPECT_EQ(table.Get<Event>(handle, ObjectType::Event), event);
    EXPECT_EQ(table.Get<Mutex>(handle, ObjectType::Mutex), nullptr);
}

TEST(HandleTable, RunsOutOfSlots) {
    HandleTable table(Capacity);
    for (std::size_t count = 0; count < Capacity; ++count) {
        ASSERT_TRUE(table.Create(AnEvent()).has_value());
    }
    EXPECT_FALSE(table.Create(AnEvent()).has_value());
    EXPECT_EQ(table.OpenCount(), Capacity);
}

TEST(HandleTable, RefusesHandlesItNeverMade) {
    const HandleTable table(Capacity);
    EXPECT_EQ(table.Get(0), nullptr);
    EXPECT_EQ(table.Get(CurrentThreadHandle), nullptr)
        << "the pseudo-handles are the kernel's to resolve, not the table's";
}

}  // namespace
}  // namespace hexlith::domain::kernel
