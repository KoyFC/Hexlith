#ifndef HEXLITH_DOMAIN_KERNEL_SCHEDULER_H
#define HEXLITH_DOMAIN_KERNEL_SCHEDULER_H

#include "domain/kernel/objects.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace hexlith::domain::kernel {

// The ARM11's clock, in ticks a second, which GetSystemTick counts in. Source:
// BASE_CLOCK_RATE_ARM11 in Azahar, src/core/core_timing.h; 3dbrew, Hardware,
// gives 268 MHz.
constexpr std::uint64_t Arm11TicksPerSecond = 268111856;

[[nodiscard]] std::uint64_t NanosecondsToTicks(std::int64_t nanoseconds);

// Who runs, and when anything that waits wakes up. ADR 0013 records the model.
//
// Every thread of the game runs on one host thread, one at a time. The thread
// that runs is the highest priority one that is ready, the first of them to
// become ready when several share a priority. It runs until it waits, yields,
// exits, or a system call or an event makes a thread of higher priority ready.
// There is no time slicing, as there is none in Horizon.
//
// Time is virtual. The clock counts ticks, one per instruction executed, and
// moves only when the caller says instructions ran or that nothing is ready to
// run; then it jumps to the next timed event. So a run is the same every time,
// which is what lets it be compared with another run instruction by
// instruction.
//
// It knows nothing of the processor or of memory: a thread's registers are
// data it carries, and the caller runs them.
class Scheduler {
public:
    // A new thread, ready, at the back of its priority. Its registers are the
    // caller's to set.
    std::shared_ptr<Thread> CreateThread(std::uint32_t priority, std::int32_t processor_id);

    // The thread that is running, or null before the first reschedule and when
    // nothing is ready.
    [[nodiscard]] Thread* Current() const {
        return current_.get();
    }

    [[nodiscard]] std::shared_ptr<Thread> CurrentShared() const {
        return current_;
    }

    // Decides which thread runs now and makes it current: the running thread
    // carries on unless it stopped running or a thread of higher priority is
    // ready, in which case it goes to the front of its priority. Answers the
    // thread, or null when nothing is ready.
    Thread* Reschedule();

    [[nodiscard]] bool AnyReady() const;

    // The clock.
    [[nodiscard]] std::uint64_t Ticks() const {
        return ticks_;
    }

    [[nodiscard]] std::optional<std::uint64_t> NextEventTick() const;

    // Moves the clock forward and runs every event that falls due, in order of
    // time and then of scheduling.
    void Advance(std::uint64_t ticks);

    // Moves the clock to the next event and runs it, for when nothing is ready.
    // False when there is no event, which with nothing ready is a deadlock.
    bool AdvanceToNextEvent();

    // Runs an action at a tick, for services that act on time, such as the
    // GPU's vertical blank. Actions due at the same tick run in the order they
    // were scheduled.
    void ScheduleAt(std::uint64_t tick, std::function<void()> action);

    // The current thread gives way to the others: SleepThread with zero
    // nanoseconds. It sleeps until the clock next moves, so every ready thread
    // runs before it again, and when nothing else is ready it carries on.
    // Source: SVC::SleepThread in Azahar.
    void Yield();

    // The current thread sleeps for a time.
    void Sleep(std::int64_t nanoseconds);

    // The current thread waits for objects. A negative timeout waits forever.
    // reports_index says whether waking up writes, into r1, which object woke
    // it, which WaitSynchronizationN does when it waits for any object. The
    // caller has already written what the system call answers if the wait
    // times out; a signal overwrites it with success.
    void Wait(std::span<const std::shared_ptr<WaitObject>> objects,
              bool wait_all,
              std::int64_t timeout,
              bool reports_index);

    // Wakes, highest priority first, every waiting thread that can now take the
    // object. WaitObject::WakeupAllWaitingThreads in Azahar. An event or a timer
    // that pulses goes back to unsignalled afterwards.
    void Signal(WaitObject& object);

    // The current thread waits on an arbiter at an address, for a time or, with
    // a negative timeout, until signalled.
    void WaitOnArbiter(AddressArbiter& arbiter, std::uint32_t address, std::int64_t timeout);

    // Wakes up to count threads waiting on an address, highest priority first,
    // or all of them for a negative count. Answers how many woke.
    std::size_t SignalArbiter(AddressArbiter& arbiter, std::uint32_t address, std::int32_t count);

    // Releases a mutex the thread holds. Fails if it does not.
    ResultCode ReleaseMutex(Thread& thread, Mutex& mutex);

    // Arms a timer to fire after a delay, then every interval if that is not
    // zero, or disarms it.
    void ArmTimer(const std::shared_ptr<Timer>& timer,
                  std::int64_t initial_nanoseconds,
                  std::int64_t interval_nanoseconds);
    static void DisarmTimer(Timer& timer);

    void SetPriority(Thread& thread, std::uint32_t priority);

    // The thread stops for good: it releases what it holds, wakes whoever waits
    // for it, and never runs again.
    void Exit(Thread& thread);

    [[nodiscard]] const std::vector<std::shared_ptr<Thread>>& Threads() const {
        return threads_;
    }

private:
    using ReadyLevel = std::deque<std::shared_ptr<Thread>>;

    void MakeReady(const std::shared_ptr<Thread>& thread, bool at_front);
    void RemoveFromReady(const Thread& thread);
    [[nodiscard]] std::shared_ptr<Thread> TakeHighestReady();
    [[nodiscard]] std::optional<std::uint32_t> HighestReadyPriority() const;

    void ScheduleWakeup(const std::shared_ptr<Thread>& thread, std::int64_t nanoseconds);
    void WakeOnTimeout(const std::shared_ptr<Thread>& thread, std::uint64_t serial);
    void StopWaiting(Thread& thread);
    void Resume(const std::shared_ptr<Thread>& thread);

    void UpdatePriority(Thread& first);
    void UpdateMutexPriority(Mutex& mutex);
    static bool RefreshMutexPriority(Mutex& mutex);
    void MoveToPriority(Thread& thread, std::uint32_t priority);
    void FireTimer(const std::shared_ptr<Timer>& timer, std::uint64_t generation);

    std::vector<std::shared_ptr<Thread>> threads_;
    std::array<ReadyLevel, LowestThreadPriority + 1> ready_;
    std::shared_ptr<Thread> current_;
    std::uint32_t next_thread_id_ = 1;

    std::uint64_t ticks_ = 0;
    std::uint64_t next_sequence_ = 0;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::function<void()>> events_;
};

}  // namespace hexlith::domain::kernel

#endif  // HEXLITH_DOMAIN_KERNEL_SCHEDULER_H
