#include "domain/kernel/objects.h"

#include <algorithm>

namespace hexlith::domain::kernel {

void WaitObject::AddWaiter(const std::shared_ptr<Thread>& thread) {
    if (std::ranges::find(waiters_, thread) == waiters_.end()) {
        waiters_.push_back(thread);
    }
}

void WaitObject::RemoveWaiter(const Thread& thread) {
    std::erase_if(waiters_,
                  [&](const std::shared_ptr<Thread>& waiter) { return waiter.get() == &thread; });
}

bool Thread::ShouldWait(const Thread& /*thread*/) const {
    return status != ThreadStatus::Dead;
}

void Thread::Acquire(Thread& /*thread*/) {}

bool Event::ShouldWait(const Thread& /*thread*/) const {
    return !signaled;
}

void Event::Acquire(Thread& /*thread*/) {
    if (reset_type == ResetType::OneShot) {
        signaled = false;
    }
}

void Event::AfterSignal() {
    if (reset_type == ResetType::Pulse) {
        signaled = false;
    }
}

bool Timer::ShouldWait(const Thread& /*thread*/) const {
    return !signaled;
}

void Timer::Acquire(Thread& /*thread*/) {
    if (reset_type == ResetType::OneShot) {
        signaled = false;
    }
}

void Timer::AfterSignal() {
    if (reset_type == ResetType::Pulse) {
        signaled = false;
    }
}

bool Mutex::ShouldWait(const Thread& thread) const {
    return lock_count > 0 && holder.get() != &thread;
}

void Mutex::Acquire(Thread& thread) {
    // The holder may lock it again; only the first lock makes it the holder.
    if (lock_count == 0) {
        holder = std::static_pointer_cast<Thread>(thread.shared_from_this());
        thread.held_mutexes.push_back(std::static_pointer_cast<Mutex>(shared_from_this()));
    }
    ++lock_count;
}

bool Semaphore::ShouldWait(const Thread& /*thread*/) const {
    return available <= 0;
}

void Semaphore::Acquire(Thread& /*thread*/) {
    --available;
}

}  // namespace hexlith::domain::kernel
