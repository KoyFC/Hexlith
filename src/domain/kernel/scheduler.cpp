#include "domain/kernel/scheduler.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace hexlith::domain::kernel {
namespace {

constexpr std::uint64_t NanosecondsPerSecond = 1'000'000'000;

// Where a thread waiting for any one object reports which one woke it.
constexpr std::size_t WaitIndexRegister = 1;

bool IsWaitingForObjects(const Thread& thread) {
    return thread.status == ThreadStatus::WaitSynchronizationAny ||
           thread.status == ThreadStatus::WaitSynchronizationAll;
}

bool IsWaiting(const Thread& thread) {
    return IsWaitingForObjects(thread) || thread.status == ThreadStatus::WaitSleep ||
           thread.status == ThreadStatus::WaitArbiter;
}

bool CanWake(const Thread& thread, const WaitObject& object) {
    if (!IsWaitingForObjects(thread) || object.ShouldWait(thread)) {
        return false;
    }
    if (thread.status == ThreadStatus::WaitSynchronizationAll) {
        return std::ranges::none_of(thread.wait_objects,
                                    [&](const auto& other) { return other->ShouldWait(thread); });
    }
    return true;
}

}  // namespace

std::uint64_t NanosecondsToTicks(std::int64_t nanoseconds) {
    if (nanoseconds <= 0) {
        return 0;
    }
    // Whole seconds and the rest apart, so that the product cannot overflow:
    // the rest is under a second, and a second's worth of ticks times a
    // second's worth of nanoseconds fits in 64 bits.
    const auto value = static_cast<std::uint64_t>(nanoseconds);
    return ((value / NanosecondsPerSecond) * Arm11TicksPerSecond) +
           ((value % NanosecondsPerSecond) * Arm11TicksPerSecond / NanosecondsPerSecond);
}

std::shared_ptr<Thread> Scheduler::CreateThread(std::uint32_t priority, std::int32_t processor_id) {
    auto thread = std::make_shared<Thread>();
    thread->id = next_thread_id_;
    ++next_thread_id_;
    thread->nominal_priority = priority;
    thread->current_priority = priority;
    thread->processor_id = processor_id;
    threads_.push_back(thread);
    MakeReady(thread, false);
    return thread;
}

void Scheduler::MakeReady(const std::shared_ptr<Thread>& thread, bool at_front) {
    thread->status = ThreadStatus::Ready;
    ReadyLevel& level = ready_.at(thread->current_priority);
    if (at_front) {
        level.push_front(thread);
    } else {
        level.push_back(thread);
    }
}

void Scheduler::RemoveFromReady(const Thread& thread) {
    std::erase_if(ready_.at(thread.current_priority),
                  [&](const std::shared_ptr<Thread>& ready) { return ready.get() == &thread; });
}

std::optional<std::uint32_t> Scheduler::HighestReadyPriority() const {
    for (std::uint32_t priority = HighestThreadPriority; priority <= LowestThreadPriority;
         ++priority) {
        if (!ready_.at(priority).empty()) {
            return priority;
        }
    }
    return std::nullopt;
}

bool Scheduler::AnyReady() const {
    return HighestReadyPriority().has_value();
}

std::shared_ptr<Thread> Scheduler::TakeHighestReady() {
    const std::optional<std::uint32_t> priority = HighestReadyPriority();
    if (!priority.has_value()) {
        return nullptr;
    }
    ReadyLevel& level = ready_.at(*priority);
    std::shared_ptr<Thread> thread = level.front();
    level.pop_front();
    return thread;
}

Thread* Scheduler::Reschedule() {
    if (current_ != nullptr && current_->status == ThreadStatus::Running) {
        const std::optional<std::uint32_t> best = HighestReadyPriority();
        if (!best.has_value() || *best >= current_->current_priority) {
            return current_.get();
        }
        // Preempted: it goes back to the front of its priority, so it runs
        // again before anything that was already waiting there.
        MakeReady(current_, true);
    }
    current_ = TakeHighestReady();
    if (current_ != nullptr) {
        current_->status = ThreadStatus::Running;
    }
    return current_.get();
}

std::optional<std::uint64_t> Scheduler::NextEventTick() const {
    if (events_.empty()) {
        return std::nullopt;
    }
    return events_.begin()->first.first;
}

void Scheduler::Advance(std::uint64_t ticks) {
    const std::uint64_t target = ticks_ + ticks;
    while (!events_.empty() && events_.begin()->first.first <= target) {
        const auto due = events_.begin();
        ticks_ = std::max(ticks_, due->first.first);
        const std::function<void()> action = due->second;
        events_.erase(due);
        action();
    }
    ticks_ = target;
}

bool Scheduler::AdvanceToNextEvent() {
    const std::optional<std::uint64_t> next = NextEventTick();
    if (!next.has_value()) {
        return false;
    }
    Advance(*next > ticks_ ? *next - ticks_ : 0);
    return true;
}

void Scheduler::ScheduleAt(std::uint64_t tick, std::function<void()> action) {
    events_.emplace(std::pair{tick, next_sequence_}, std::move(action));
    ++next_sequence_;
}

void Scheduler::ScheduleWakeup(const std::shared_ptr<Thread>& thread, std::int64_t nanoseconds) {
    const std::uint64_t serial = thread->wait_serial;
    const std::weak_ptr<Thread> weak = thread;
    ScheduleAt(ticks_ + NanosecondsToTicks(nanoseconds), [this, weak, serial] {
        if (const std::shared_ptr<Thread> sleeper = weak.lock()) {
            WakeOnTimeout(sleeper, serial);
        }
    });
}

void Scheduler::WakeOnTimeout(const std::shared_ptr<Thread>& thread, std::uint64_t serial) {
    if (thread->wait_serial != serial || !IsWaiting(*thread)) {
        return;
    }
    // Whatever the system call answers on a timeout is already in the
    // thread's registers.
    StopWaiting(*thread);
    Resume(thread);
}

void Scheduler::StopWaiting(Thread& thread) {
    const std::vector<std::shared_ptr<WaitObject>> objects = std::move(thread.wait_objects);
    thread.wait_objects.clear();
    for (const std::shared_ptr<WaitObject>& object : objects) {
        object->RemoveWaiter(thread);
        if (Mutex* mutex = object->AsMutex()) {
            UpdateMutexPriority(*mutex);
        }
    }
    if (thread.arbiter != nullptr) {
        std::erase_if(thread.arbiter->waiting, [&](const std::shared_ptr<Thread>& waiter) {
            return waiter.get() == &thread;
        });
        thread.arbiter = nullptr;
    }
    ++thread.wait_serial;
}

void Scheduler::Resume(const std::shared_ptr<Thread>& thread) {
    MakeReady(thread, false);
}

void Scheduler::Yield() {
    if (!AnyReady()) {
        return;
    }
    Sleep(0);
}

void Scheduler::Sleep(std::int64_t nanoseconds) {
    current_->status = ThreadStatus::WaitSleep;
    // A negative time sleeps for ever, the way a negative timeout waits for
    // ever.
    if (nanoseconds >= 0) {
        ScheduleWakeup(current_, nanoseconds);
    }
}

void Scheduler::Wait(std::span<const std::shared_ptr<WaitObject>> objects,
                     bool wait_all,
                     std::int64_t timeout,
                     bool reports_index) {
    const std::shared_ptr<Thread> thread = current_;
    thread->status =
        wait_all ? ThreadStatus::WaitSynchronizationAll : ThreadStatus::WaitSynchronizationAny;
    thread->wait_objects.assign(objects.begin(), objects.end());
    thread->reports_wait_index = reports_index;
    for (const std::shared_ptr<WaitObject>& object : objects) {
        object->AddWaiter(thread);
        if (Mutex* mutex = object->AsMutex()) {
            UpdateMutexPriority(*mutex);
        }
    }
    if (timeout >= 0) {
        ScheduleWakeup(thread, timeout);
    }
}

void Scheduler::Signal(WaitObject& object) {
    while (true) {
        // The highest priority thread that can take the object now, the first
        // to have started waiting among equals.
        std::shared_ptr<Thread> chosen;
        for (const std::shared_ptr<Thread>& waiter : object.Waiters()) {
            if ((chosen == nullptr || waiter->current_priority < chosen->current_priority) &&
                CanWake(*waiter, object)) {
                chosen = waiter;
            }
        }
        if (chosen == nullptr) {
            break;
        }

        if (chosen->status == ThreadStatus::WaitSynchronizationAll) {
            for (const std::shared_ptr<WaitObject>& each : chosen->wait_objects) {
                each->Acquire(*chosen);
            }
        } else {
            object.Acquire(*chosen);
        }

        chosen->context.registers.at(0) = result::Success.raw;
        if (chosen->reports_wait_index) {
            const auto found = std::ranges::find_if(
                chosen->wait_objects, [&](const auto& each) { return each.get() == &object; });
            chosen->context.registers.at(WaitIndexRegister) =
                static_cast<std::uint32_t>(std::distance(chosen->wait_objects.begin(), found));
        }
        StopWaiting(*chosen);
        // It may now hold a mutex others still wait for, and inherit from them.
        UpdatePriority(*chosen);
        Resume(chosen);
    }

    object.AfterSignal();
}

void Scheduler::WaitOnArbiter(AddressArbiter& arbiter,
                              std::uint32_t address,
                              std::int64_t timeout) {
    const std::shared_ptr<Thread> thread = current_;
    thread->status = ThreadStatus::WaitArbiter;
    thread->arbiter = std::static_pointer_cast<AddressArbiter>(arbiter.shared_from_this());
    thread->arbiter_address = address;
    arbiter.waiting.push_back(thread);
    if (timeout >= 0) {
        ScheduleWakeup(thread, timeout);
    }
}

std::size_t
Scheduler::SignalArbiter(AddressArbiter& arbiter, std::uint32_t address, std::int32_t count) {
    std::size_t woken = 0;
    while (count < 0 || std::cmp_less(woken, count)) {
        // Among the threads waiting on this address, the highest priority one,
        // and the first of those when several share it. AddressArbiter::
        // ResumeHighestPriorityThread in Azahar, which notes the console does
        // the same.
        std::shared_ptr<Thread> chosen;
        for (const std::shared_ptr<Thread>& waiter : arbiter.waiting) {
            if (waiter->arbiter_address == address &&
                (chosen == nullptr || waiter->current_priority < chosen->current_priority)) {
                chosen = waiter;
            }
        }
        if (chosen == nullptr) {
            break;
        }
        StopWaiting(*chosen);
        Resume(chosen);
        ++woken;
    }
    return woken;
}

ResultCode Scheduler::ReleaseMutex(Thread& thread, Mutex& mutex) {
    if (mutex.holder.get() != &thread || mutex.lock_count == 0) {
        return result::WrongLockingThread;
    }
    --mutex.lock_count;
    if (mutex.lock_count == 0) {
        std::erase_if(thread.held_mutexes,
                      [&](const std::shared_ptr<Mutex>& held) { return held.get() == &mutex; });
        mutex.holder = nullptr;
        UpdatePriority(thread);
        Signal(mutex);
    }
    return result::Success;
}

void Scheduler::UpdateMutexPriority(Mutex& mutex) {
    if (RefreshMutexPriority(mutex) && mutex.holder != nullptr) {
        UpdatePriority(*mutex.holder);
    }
}

bool Scheduler::RefreshMutexPriority(Mutex& mutex) {
    std::uint32_t best = LowestThreadPriority;
    for (const std::shared_ptr<Thread>& waiter : mutex.Waiters()) {
        best = std::min(best, waiter->current_priority);
    }
    if (best == mutex.priority) {
        return false;
    }
    mutex.priority = best;
    return true;
}

void Scheduler::UpdatePriority(Thread& first) {
    // A change of priority travels along a chain: a thread that waits for a
    // mutex lends its priority to the holder, who may wait for another mutex.
    // The chain is walked with a list rather than by recursion.
    std::vector<Thread*> pending = {&first};
    while (!pending.empty()) {
        Thread& thread = *pending.back();
        pending.pop_back();

        std::uint32_t best = thread.nominal_priority;
        for (const std::shared_ptr<Mutex>& held : thread.held_mutexes) {
            best = std::min(best, held->priority);
        }
        if (best == thread.current_priority) {
            continue;
        }
        MoveToPriority(thread, best);

        for (const std::shared_ptr<WaitObject>& object : thread.wait_objects) {
            Mutex* mutex = object->AsMutex();
            if (mutex != nullptr && RefreshMutexPriority(*mutex) && mutex->holder != nullptr) {
                pending.push_back(mutex->holder.get());
            }
        }
    }
}

void Scheduler::MoveToPriority(Thread& thread, std::uint32_t priority) {
    if (thread.status == ThreadStatus::Ready) {
        RemoveFromReady(thread);
        thread.current_priority = priority;
        MakeReady(std::static_pointer_cast<Thread>(thread.shared_from_this()), false);
    } else {
        thread.current_priority = priority;
    }
}

void Scheduler::SetPriority(Thread& thread, std::uint32_t priority) {
    thread.nominal_priority = priority;
    // Running at the new priority first, so that UpdatePriority starts from it
    // and only lending a mutex can raise it again.
    MoveToPriority(thread, priority);
    UpdatePriority(thread);
}

void Scheduler::ArmTimer(const std::shared_ptr<Timer>& timer,
                         std::int64_t initial_nanoseconds,
                         std::int64_t interval_nanoseconds) {
    ++timer->generation;
    timer->interval_nanoseconds = interval_nanoseconds;
    const std::uint64_t generation = timer->generation;
    if (initial_nanoseconds == 0) {
        // Timer::Set in Azahar fires a timer with no initial delay at once.
        FireTimer(timer, generation);
        return;
    }
    ScheduleAt(ticks_ + NanosecondsToTicks(initial_nanoseconds),
               [this, timer, generation] { FireTimer(timer, generation); });
}

void Scheduler::DisarmTimer(Timer& timer) {
    ++timer.generation;
}

void Scheduler::FireTimer(const std::shared_ptr<Timer>& timer, std::uint64_t generation) {
    if (timer->generation != generation) {
        return;
    }
    timer->signaled = true;
    Signal(*timer);
    if (timer->interval_nanoseconds != 0) {
        ScheduleAt(ticks_ + NanosecondsToTicks(timer->interval_nanoseconds),
                   [this, timer, generation] { FireTimer(timer, generation); });
    }
}

void Scheduler::Exit(Thread& thread) {
    StopWaiting(thread);
    if (thread.status == ThreadStatus::Ready) {
        RemoveFromReady(thread);
    }
    thread.status = ThreadStatus::Dead;

    const std::vector<std::shared_ptr<Mutex>> held = std::move(thread.held_mutexes);
    thread.held_mutexes.clear();
    for (const std::shared_ptr<Mutex>& mutex : held) {
        mutex->lock_count = 0;
        mutex->holder = nullptr;
        Signal(*mutex);
    }
    Signal(thread);
}

}  // namespace hexlith::domain::kernel
