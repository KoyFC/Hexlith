#ifndef HEXLITH_DOMAIN_KERNEL_OBJECTS_H
#define HEXLITH_DOMAIN_KERNEL_OBJECTS_H

#include "domain/arm/cpu_state.h"
#include "domain/kernel/result_code.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hexlith::domain::kernel {

// What a handle refers to. The kernel checks the type before it uses an
// object, and a handle of the wrong type is an invalid handle to the caller.
enum class ObjectType : std::uint8_t {
    Thread,
    Event,
    Mutex,
    Semaphore,
    Timer,
    AddressArbiter,
    SharedMemory,
    ResourceLimit,
    ClientPort,
    ClientSession,
};

// A kernel object a handle can refer to.
//
// Objects are reached through a virtual interface, which ARCHITECTURE.md keeps
// off the path that runs per instruction. This one runs per system call, and a
// system call already costs a stop of the interpreter and a table lookup.
//
// Objects are always owned by shared pointers, from the handle table, from the
// threads waiting on them and from the scheduler, so an object can hand out a
// pointer to itself.
class Object : public std::enable_shared_from_this<Object> {
public:
    Object() = default;
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    Object(Object&&) = delete;
    Object& operator=(Object&&) = delete;
    virtual ~Object() = default;

    [[nodiscard]] virtual ObjectType Type() const = 0;
};

class AddressArbiter;
class Mutex;
class Thread;

// An object a thread can wait for. Source for the model: WaitObject in Azahar,
// src/core/hle/kernel/wait_object.cpp.
class WaitObject : public Object {
public:
    // Whether the thread would have to wait to acquire the object now.
    [[nodiscard]] virtual bool ShouldWait(const Thread& thread) const = 0;

    // Takes the object for the thread, which must not have to wait for it.
    virtual void Acquire(Thread& thread) = 0;

    // What becomes of the object once every thread that could take it has been
    // woken: an event or a timer that pulses goes back to unsignalled.
    virtual void AfterSignal() {}

    // The object as a mutex, or null, for the scheduler's priority inheritance.
    virtual Mutex* AsMutex() {
        return nullptr;
    }

    // The threads waiting for this object, in the order they started waiting.
    [[nodiscard]] const std::vector<std::shared_ptr<Thread>>& Waiters() const {
        return waiters_;
    }

    void AddWaiter(const std::shared_ptr<Thread>& thread);
    void RemoveWaiter(const Thread& thread);

private:
    std::vector<std::shared_ptr<Thread>> waiters_;
};

// What a thread is doing. Source: ThreadStatus in Azahar.
enum class ThreadStatus : std::uint8_t {
    Ready,
    Running,
    // Sleeping for a time.
    WaitSleep,
    // Waiting for any one of its objects, or for all of them.
    WaitSynchronizationAny,
    WaitSynchronizationAll,
    // Waiting on an address arbiter.
    WaitArbiter,
    Dead,
};

// The priority range: 0 is the highest, 63 the lowest. Source: 3dbrew, SVC,
// CreateThread.
constexpr std::uint32_t HighestThreadPriority = 0;
constexpr std::uint32_t LowestThreadPriority = 63;

// A thread of the game. It waits on a mutex like any object: it becomes
// available when it exits, which is how a thread joins another.
class Thread final : public WaitObject {
public:
    [[nodiscard]] ObjectType Type() const override {
        return ObjectType::Thread;
    }

    [[nodiscard]] bool ShouldWait(const Thread& thread) const override;
    void Acquire(Thread& thread) override;

    std::uint32_t id = 0;

    // Its registers while it is not running.
    arm::CpuState context;

    // The priority it was given, and the one it runs at, which a mutex it holds
    // can raise for as long as a higher priority thread waits for that mutex.
    std::uint32_t nominal_priority = LowestThreadPriority;
    std::uint32_t current_priority = LowestThreadPriority;

    std::int32_t processor_id = 0;
    std::uint32_t thread_local_storage = 0;

    ThreadStatus status = ThreadStatus::Ready;

    // What it waits for, and how the system call that put it to sleep is
    // answered when it wakes.
    std::vector<std::shared_ptr<WaitObject>> wait_objects;
    std::shared_ptr<AddressArbiter> arbiter;
    std::uint32_t arbiter_address = 0;
    bool reports_wait_index = false;

    // Counts the waits the thread has started, so that a timeout scheduled for
    // a wait that has already ended finds the count moved on and does nothing.
    std::uint64_t wait_serial = 0;

    // The mutexes it holds, for priority inheritance and to release them when
    // it exits.
    std::vector<std::shared_ptr<Mutex>> held_mutexes;
};

// How an event or a timer behaves once it is signalled. Source: 3dbrew, SVC,
// CreateEvent; ResetType in Azahar.
enum class ResetType : std::uint8_t {
    // Stays signalled until one waiting thread takes it.
    OneShot = 0,
    // Stays signalled until cleared.
    Sticky = 1,
    // Wakes whoever is waiting at that moment and stays unsignalled.
    Pulse = 2,
};

class Event final : public WaitObject {
public:
    explicit Event(ResetType reset) : reset_type(reset) {}

    [[nodiscard]] ObjectType Type() const override {
        return ObjectType::Event;
    }

    [[nodiscard]] bool ShouldWait(const Thread& thread) const override;
    void Acquire(Thread& thread) override;
    void AfterSignal() override;

    ResetType reset_type;
    bool signaled = false;
};

class Timer final : public WaitObject {
public:
    explicit Timer(ResetType reset) : reset_type(reset) {}

    [[nodiscard]] ObjectType Type() const override {
        return ObjectType::Timer;
    }

    [[nodiscard]] bool ShouldWait(const Thread& thread) const override;
    void Acquire(Thread& thread) override;
    void AfterSignal() override;

    ResetType reset_type;
    bool signaled = false;

    // Nanoseconds between firings after the first, zero for a timer that fires
    // once. A timer is armed while it has a pending firing in the scheduler.
    std::int64_t interval_nanoseconds = 0;
    std::uint64_t generation = 0;
};

// A recursive lock with an owner. Source: Mutex in Azahar,
// src/core/hle/kernel/mutex.cpp.
class Mutex final : public WaitObject {
public:
    [[nodiscard]] ObjectType Type() const override {
        return ObjectType::Mutex;
    }

    [[nodiscard]] bool ShouldWait(const Thread& thread) const override;
    void Acquire(Thread& thread) override;

    Mutex* AsMutex() override {
        return this;
    }

    std::uint32_t lock_count = 0;
    std::shared_ptr<Thread> holder;

    // The best priority among the threads waiting for it, which its holder
    // inherits.
    std::uint32_t priority = LowestThreadPriority;
};

class Semaphore final : public WaitObject {
public:
    Semaphore(std::int32_t initial, std::int32_t limit) : available(initial), maximum(limit) {}

    [[nodiscard]] ObjectType Type() const override {
        return ObjectType::Semaphore;
    }

    [[nodiscard]] bool ShouldWait(const Thread& thread) const override;
    void Acquire(Thread& thread) override;

    std::int32_t available;
    std::int32_t maximum;
};

// An object threads wait on by guest address rather than by handle. It is not
// itself waited for: the threads it holds wait in the arbiter's own list.
class AddressArbiter final : public Object {
public:
    [[nodiscard]] ObjectType Type() const override {
        return ObjectType::AddressArbiter;
    }

    std::vector<std::shared_ptr<Thread>> waiting;
};

// The result a mutex release answers when the caller does not hold it.
// Source: ResultWrongLockingThread in Azahar's errors.h.
namespace result {
constexpr ResultCode WrongLockingThread{0xD8E0041F};
constexpr ResultCode OutOfRangeKernel{0xD8E007FD};
}  // namespace result

}  // namespace hexlith::domain::kernel

#endif  // HEXLITH_DOMAIN_KERNEL_OBJECTS_H
