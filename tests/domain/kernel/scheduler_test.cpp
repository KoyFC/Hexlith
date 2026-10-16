#include "domain/kernel/scheduler.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace hexlith::domain::kernel {
namespace {

constexpr std::uint32_t High = 0x10;
constexpr std::uint32_t Normal = 0x30;
constexpr std::uint32_t Low = 0x38;

constexpr std::int64_t Forever = -1;
constexpr std::int64_t OneMillisecond = 1'000'000;
constexpr std::uint64_t TicksPerMillisecond = 268111;
constexpr std::uint64_t ASecond = 1000 * TicksPerMillisecond;

constexpr std::uint32_t ArbitratedAddress = 0x08001000;
constexpr std::uint32_t OtherAddress = 0x08001004;

// What a waiting thread's r0 holds if nothing overwrites it: the timeout a
// system call writes before it puts the thread to sleep.
void PresetTimeout(Thread& thread) {
    thread.context.registers.at(0) = result::Timeout.raw;
}

std::vector<std::shared_ptr<WaitObject>> Objects(std::shared_ptr<WaitObject> object) {
    return {std::move(object)};
}

TEST(Scheduler, ConvertsNanosecondsAtTheArm11Clock) {
    EXPECT_EQ(NanosecondsToTicks(1'000'000'000), Arm11TicksPerSecond);
    EXPECT_EQ(NanosecondsToTicks(OneMillisecond), TicksPerMillisecond);
    EXPECT_EQ(NanosecondsToTicks(-1), 0U);
}

TEST(Scheduler, RunsTheHighestPriorityThread) {
    Scheduler scheduler;
    const auto normal = scheduler.CreateThread(Normal, 0);
    const auto high = scheduler.CreateThread(High, 0);
    EXPECT_EQ(scheduler.Reschedule(), high.get());
    EXPECT_EQ(high->status, ThreadStatus::Running);
    EXPECT_EQ(normal->status, ThreadStatus::Ready);
}

TEST(Scheduler, NeverSlicesTimeBetweenEqualPriorities) {
    Scheduler scheduler;
    const auto first = scheduler.CreateThread(Normal, 0);
    scheduler.CreateThread(Normal, 0);
    ASSERT_EQ(scheduler.Reschedule(), first.get());

    scheduler.Advance(ASecond);
    EXPECT_EQ(scheduler.Reschedule(), first.get()) << "it runs until it gives way";
}

TEST(Scheduler, AYieldingThreadGoesBehindTheOthers) {
    Scheduler scheduler;
    const auto first = scheduler.CreateThread(Normal, 0);
    const auto second = scheduler.CreateThread(Normal, 0);
    ASSERT_EQ(scheduler.Reschedule(), first.get());

    scheduler.Yield();
    EXPECT_EQ(scheduler.Reschedule(), second.get());
    scheduler.Advance(1);
    EXPECT_EQ(first->status, ThreadStatus::Ready) << "awake again once the clock moves";

    scheduler.Yield();
    EXPECT_EQ(scheduler.Reschedule(), first.get());
}

TEST(Scheduler, AYieldWithNothingElseReadyCarriesOn) {
    Scheduler scheduler;
    const auto only = scheduler.CreateThread(Normal, 0);
    ASSERT_EQ(scheduler.Reschedule(), only.get());
    scheduler.Yield();
    EXPECT_EQ(only->status, ThreadStatus::Running);
}

TEST(Scheduler, ASleeperWakesWhenItsTimeComesAndPreempts) {
    Scheduler scheduler;
    const auto sleeper = scheduler.CreateThread(High, 0);
    const auto worker = scheduler.CreateThread(Normal, 0);
    ASSERT_EQ(scheduler.Reschedule(), sleeper.get());
    scheduler.Sleep(OneMillisecond);
    ASSERT_EQ(scheduler.Reschedule(), worker.get());

    scheduler.Advance(TicksPerMillisecond - 1);
    EXPECT_EQ(scheduler.Reschedule(), worker.get());
    scheduler.Advance(1);
    EXPECT_EQ(scheduler.Reschedule(), sleeper.get());
    EXPECT_EQ(worker->status, ThreadStatus::Ready);
}

TEST(Scheduler, APreemptedThreadRunsBeforeOthersOfItsPriority) {
    Scheduler scheduler;
    const auto high = scheduler.CreateThread(High, 0);
    const auto first = scheduler.CreateThread(Normal, 0);
    const auto second = scheduler.CreateThread(Normal, 0);
    const auto event = std::make_shared<Event>(ResetType::OneShot);

    ASSERT_EQ(scheduler.Reschedule(), high.get());
    scheduler.Wait(Objects(event), false, Forever, false);
    ASSERT_EQ(scheduler.Reschedule(), first.get());

    event->signaled = true;
    scheduler.Signal(*event);
    ASSERT_EQ(scheduler.Reschedule(), high.get());
    scheduler.Exit(*high);
    EXPECT_EQ(scheduler.Reschedule(), first.get()) << "back at the front, ahead of the second";
    EXPECT_EQ(second->status, ThreadStatus::Ready);
}

TEST(Scheduler, AOneShotEventWakesOneWaiterHighestPriorityFirst) {
    Scheduler scheduler;
    const auto low = scheduler.CreateThread(Low, 0);
    const auto event = std::make_shared<Event>(ResetType::OneShot);

    // The high priority thread starts waiting second, and still wakes first.
    ASSERT_EQ(scheduler.Reschedule(), low.get());
    const auto later = scheduler.CreateThread(High, 0);
    scheduler.Wait(Objects(event), false, Forever, false);
    ASSERT_EQ(scheduler.Reschedule(), later.get());
    scheduler.Wait(Objects(event), false, Forever, false);

    event->signaled = true;
    scheduler.Signal(*event);
    EXPECT_EQ(later->status, ThreadStatus::Ready);
    EXPECT_EQ(low->status, ThreadStatus::WaitSynchronizationAny);
    EXPECT_FALSE(event->signaled) << "a one-shot event is taken by the thread it wakes";
}

TEST(Scheduler, APulseWakesEveryWaiterAndLeavesTheEventClear) {
    Scheduler scheduler;
    const auto first = scheduler.CreateThread(Normal, 0);
    const auto second = scheduler.CreateThread(Normal, 0);
    const auto event = std::make_shared<Event>(ResetType::Pulse);
    ASSERT_EQ(scheduler.Reschedule(), first.get());
    scheduler.Wait(Objects(event), false, Forever, false);
    ASSERT_EQ(scheduler.Reschedule(), second.get());
    scheduler.Wait(Objects(event), false, Forever, false);

    event->signaled = true;
    scheduler.Signal(*event);
    EXPECT_EQ(first->status, ThreadStatus::Ready);
    EXPECT_EQ(second->status, ThreadStatus::Ready);
    EXPECT_FALSE(event->signaled);
}

TEST(Scheduler, AStickyEventStaysSignalled) {
    Scheduler scheduler;
    const auto waiter = scheduler.CreateThread(Normal, 0);
    const auto event = std::make_shared<Event>(ResetType::Sticky);
    ASSERT_EQ(scheduler.Reschedule(), waiter.get());
    scheduler.Wait(Objects(event), false, Forever, false);
    event->signaled = true;
    scheduler.Signal(*event);
    EXPECT_EQ(waiter->status, ThreadStatus::Ready);
    EXPECT_TRUE(event->signaled);
}

TEST(Scheduler, WaitingForAnyReportsWhichObjectWokeIt) {
    Scheduler scheduler;
    const auto waiter = scheduler.CreateThread(Normal, 0);
    const auto first = std::make_shared<Event>(ResetType::OneShot);
    const auto second = std::make_shared<Event>(ResetType::OneShot);
    ASSERT_EQ(scheduler.Reschedule(), waiter.get());
    PresetTimeout(*waiter);
    const std::vector<std::shared_ptr<WaitObject>> objects = {first, second};
    scheduler.Wait(objects, false, Forever, true);

    second->signaled = true;
    scheduler.Signal(*second);
    EXPECT_EQ(waiter->context.registers.at(0), result::Success.raw);
    EXPECT_EQ(waiter->context.registers.at(1), 1U);
    EXPECT_TRUE(first->Waiters().empty()) << "it stops waiting on every object";
}

TEST(Scheduler, WaitingForAllWakesOnlyWhenEveryObjectIsAvailable) {
    Scheduler scheduler;
    const auto waiter = scheduler.CreateThread(Normal, 0);
    const auto first = std::make_shared<Event>(ResetType::OneShot);
    const auto second = std::make_shared<Event>(ResetType::OneShot);
    ASSERT_EQ(scheduler.Reschedule(), waiter.get());
    const std::vector<std::shared_ptr<WaitObject>> objects = {first, second};
    scheduler.Wait(objects, true, Forever, false);

    first->signaled = true;
    scheduler.Signal(*first);
    EXPECT_EQ(waiter->status, ThreadStatus::WaitSynchronizationAll);
    second->signaled = true;
    scheduler.Signal(*second);
    EXPECT_EQ(waiter->status, ThreadStatus::Ready);
    EXPECT_FALSE(first->signaled);
    EXPECT_FALSE(second->signaled) << "both were taken";
}

TEST(Scheduler, AWaitThatTimesOutKeepsTheTimeoutResult) {
    Scheduler scheduler;
    const auto waiter = scheduler.CreateThread(Normal, 0);
    const auto event = std::make_shared<Event>(ResetType::OneShot);
    ASSERT_EQ(scheduler.Reschedule(), waiter.get());
    PresetTimeout(*waiter);
    scheduler.Wait(Objects(event), false, OneMillisecond, false);

    EXPECT_TRUE(scheduler.AdvanceToNextEvent());
    EXPECT_EQ(waiter->status, ThreadStatus::Ready);
    EXPECT_EQ(waiter->context.registers.at(0), result::Timeout.raw);
    EXPECT_TRUE(event->Waiters().empty());
    EXPECT_EQ(scheduler.Ticks(), TicksPerMillisecond);

    // A signal after the timeout does nothing to it.
    event->signaled = true;
    scheduler.Signal(*event);
    EXPECT_TRUE(event->signaled);
}

TEST(Scheduler, ASignalCancelsThePendingTimeout) {
    Scheduler scheduler;
    const auto waiter = scheduler.CreateThread(Normal, 0);
    const auto event = std::make_shared<Event>(ResetType::OneShot);
    ASSERT_EQ(scheduler.Reschedule(), waiter.get());
    scheduler.Wait(Objects(event), false, OneMillisecond, false);
    event->signaled = true;
    scheduler.Signal(*event);
    ASSERT_EQ(scheduler.Reschedule(), waiter.get());

    // The thread waits again; the first wait's timeout must not end this one.
    scheduler.Wait(Objects(event), false, Forever, false);
    scheduler.Advance(2 * TicksPerMillisecond);
    EXPECT_EQ(waiter->status, ThreadStatus::WaitSynchronizationAny);
}

TEST(Scheduler, AMutexLendsItsHolderThePriorityOfItsWaiters) {
    Scheduler scheduler;
    const auto holder = scheduler.CreateThread(Low, 0);
    const auto mutex = std::make_shared<Mutex>();
    ASSERT_EQ(scheduler.Reschedule(), holder.get());
    mutex->Acquire(*holder);
    mutex->Acquire(*holder);
    EXPECT_EQ(mutex->lock_count, 2U) << "the holder may lock it again";

    const auto waiter = scheduler.CreateThread(High, 0);
    ASSERT_EQ(scheduler.Reschedule(), waiter.get());
    ASSERT_TRUE(mutex->ShouldWait(*waiter));
    scheduler.Wait(Objects(mutex), false, Forever, false);
    EXPECT_EQ(holder->current_priority, High);
    ASSERT_EQ(scheduler.Reschedule(), holder.get());

    EXPECT_EQ(scheduler.ReleaseMutex(*waiter, *mutex), result::WrongLockingThread);
    EXPECT_EQ(scheduler.ReleaseMutex(*holder, *mutex), result::Success);
    EXPECT_EQ(waiter->status, ThreadStatus::WaitSynchronizationAny) << "still locked once";
    EXPECT_EQ(scheduler.ReleaseMutex(*holder, *mutex), result::Success);

    EXPECT_EQ(mutex->holder, waiter);
    EXPECT_EQ(holder->current_priority, Low);
    EXPECT_EQ(scheduler.Reschedule(), waiter.get());
}

TEST(Scheduler, ASemaphoreWakesAsManyAsItHasRoomFor) {
    Scheduler scheduler;
    const auto first = scheduler.CreateThread(Normal, 0);
    const auto second = scheduler.CreateThread(Normal, 0);
    const auto semaphore = std::make_shared<Semaphore>(0, 2);
    ASSERT_EQ(scheduler.Reschedule(), first.get());
    scheduler.Wait(Objects(semaphore), false, Forever, false);
    ASSERT_EQ(scheduler.Reschedule(), second.get());
    scheduler.Wait(Objects(semaphore), false, Forever, false);

    semaphore->available = 1;
    scheduler.Signal(*semaphore);
    EXPECT_EQ(first->status, ThreadStatus::Ready);
    EXPECT_EQ(second->status, ThreadStatus::WaitSynchronizationAny);
    EXPECT_EQ(semaphore->available, 0);
}

TEST(Scheduler, AnArbiterWakesThreadsWaitingOnTheSignalledAddressOnly) {
    Scheduler scheduler;
    const auto arbiter = std::make_shared<AddressArbiter>();
    const auto low = scheduler.CreateThread(Low, 0);
    ASSERT_EQ(scheduler.Reschedule(), low.get());
    scheduler.WaitOnArbiter(*arbiter, ArbitratedAddress, Forever);
    const auto other = scheduler.CreateThread(Normal, 0);
    ASSERT_EQ(scheduler.Reschedule(), other.get());
    scheduler.WaitOnArbiter(*arbiter, OtherAddress, Forever);
    const auto high = scheduler.CreateThread(High, 0);
    ASSERT_EQ(scheduler.Reschedule(), high.get());
    scheduler.WaitOnArbiter(*arbiter, ArbitratedAddress, Forever);

    EXPECT_EQ(scheduler.SignalArbiter(*arbiter, ArbitratedAddress, 1), 1U);
    EXPECT_EQ(high->status, ThreadStatus::Ready) << "the highest priority one first";
    EXPECT_EQ(low->status, ThreadStatus::WaitArbiter);

    EXPECT_EQ(scheduler.SignalArbiter(*arbiter, ArbitratedAddress, -1), 1U);
    EXPECT_EQ(low->status, ThreadStatus::Ready);
    EXPECT_EQ(other->status, ThreadStatus::WaitArbiter);
    EXPECT_EQ(arbiter->waiting.size(), 1U);
}

TEST(Scheduler, AnArbiterWaitCanTimeOut) {
    Scheduler scheduler;
    const auto arbiter = std::make_shared<AddressArbiter>();
    const auto waiter = scheduler.CreateThread(Normal, 0);
    ASSERT_EQ(scheduler.Reschedule(), waiter.get());
    scheduler.WaitOnArbiter(*arbiter, ArbitratedAddress, OneMillisecond);
    EXPECT_TRUE(scheduler.AdvanceToNextEvent());
    EXPECT_EQ(waiter->status, ThreadStatus::Ready);
    EXPECT_TRUE(arbiter->waiting.empty());
}

// The waiter, which must be running, waits for the timer and the clock moves
// to its next firing. Answers whether that woke the waiter.
bool WakesAtTheNextFiring(Scheduler& scheduler,
                          const std::shared_ptr<Thread>& waiter,
                          const std::shared_ptr<Timer>& timer) {
    scheduler.Wait(Objects(timer), false, Forever, false);
    return scheduler.AdvanceToNextEvent() && waiter->status == ThreadStatus::Ready &&
           scheduler.Reschedule() == waiter.get();
}

TEST(Scheduler, APeriodicTimerFiresUntilDisarmed) {
    Scheduler scheduler;
    const auto waiter = scheduler.CreateThread(Normal, 0);
    const auto timer = std::make_shared<Timer>(ResetType::Pulse);
    ASSERT_EQ(scheduler.Reschedule(), waiter.get());
    scheduler.ArmTimer(timer, OneMillisecond, OneMillisecond);

    EXPECT_TRUE(WakesAtTheNextFiring(scheduler, waiter, timer));
    EXPECT_TRUE(WakesAtTheNextFiring(scheduler, waiter, timer));
    EXPECT_TRUE(WakesAtTheNextFiring(scheduler, waiter, timer));
    EXPECT_EQ(scheduler.Ticks(), 3 * TicksPerMillisecond);

    Scheduler::DisarmTimer(*timer);
    scheduler.Wait(Objects(timer), false, Forever, false);
    scheduler.Advance(ASecond);
    EXPECT_EQ(waiter->status, ThreadStatus::WaitSynchronizationAny);
}

TEST(Scheduler, ExitingWakesJoinersAndReleasesMutexes) {
    Scheduler scheduler;
    const auto worker = scheduler.CreateThread(High, 0);
    const auto joiner = scheduler.CreateThread(Normal, 0);
    const auto mutex = std::make_shared<Mutex>();
    ASSERT_EQ(scheduler.Reschedule(), worker.get());
    mutex->Acquire(*worker);
    scheduler.Exit(*worker);

    ASSERT_EQ(scheduler.Reschedule(), joiner.get());
    EXPECT_FALSE(worker->ShouldWait(*joiner)) << "a dead thread is available to wait on";
    EXPECT_FALSE(mutex->ShouldWait(*joiner)) << "its mutexes were released";
}

TEST(Scheduler, KnowsWhenNothingCanEverRunAgain) {
    Scheduler scheduler;
    const auto only = scheduler.CreateThread(Normal, 0);
    const auto event = std::make_shared<Event>(ResetType::OneShot);
    ASSERT_EQ(scheduler.Reschedule(), only.get());
    scheduler.Wait(Objects(event), false, Forever, false);
    EXPECT_EQ(scheduler.Reschedule(), nullptr);
    EXPECT_FALSE(scheduler.AdvanceToNextEvent());
}

TEST(Scheduler, RunsEventsDueAtTheSameTickInTheOrderTheyWereScheduled) {
    Scheduler scheduler;
    constexpr std::uint64_t Early = 5;
    constexpr std::uint64_t Late = 10;
    std::vector<int> order;
    scheduler.ScheduleAt(Late, [&] { order.push_back(2); });
    scheduler.ScheduleAt(Early, [&] { order.push_back(1); });
    scheduler.ScheduleAt(Late, [&] { order.push_back(3); });
    scheduler.Advance(Late);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

}  // namespace
}  // namespace hexlith::domain::kernel
