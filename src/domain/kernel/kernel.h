#ifndef HEXLITH_DOMAIN_KERNEL_KERNEL_H
#define HEXLITH_DOMAIN_KERNEL_KERNEL_H

#include "core/result.h"
#include "domain/arm/interpreter/interpreter.h"
#include "domain/kernel/address_space.h"
#include "domain/kernel/executable_loader.h"
#include "domain/kernel/handle_table.h"
#include "domain/kernel/ipc.h"
#include "domain/kernel/objects.h"
#include "domain/kernel/region_allocator.h"
#include "domain/kernel/scheduler.h"
#include "domain/kernel/system_pages.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace hexlith::domain::kernel {

// The process ID Azahar gave the reference title in the captured runs.
constexpr std::uint32_t AzaharApplicationProcessId = 11;

// What the kernel is built for: the FCRAM layout, where the linear heap is, the
// handle table's size and the core a thread lands on when it asks for the
// default one. The extended header supplies all of it.
struct KernelConfig {
    MemoryRegionSizes regions = ProductionMemoryRegions;
    std::uint8_t memory_mode = ProductionMemoryMode;
    std::uint32_t linear_heap_base = layout::OldLinearHeapBase;
    std::size_t handle_capacity = 0;
    std::uint8_t ideal_processor = 0;

    // The process ID the game gets for itself. The console's depends on how
    // many system modules started before it; 11 is what Azahar gave the
    // reference title in the captured runs, kept so that the two traces read
    // alike.
    std::uint32_t process_id = AzaharApplicationProcessId;
};

// Why Run returned.
enum class KernelStopReason : std::uint8_t {
    // It ran as many instructions as it was asked to.
    BudgetSpent,
    // The game made a system call nothing here implements. The detail is its
    // number, and the calling thread is left before the call, so that the
    // call can be implemented and the run carried on from there.
    UnimplementedSystemCall,
    // The interpreter stopped for a reason other than a system call: a fault,
    // an undefined instruction, an unsupported mode. The stop says which.
    ProcessorStop,
    // No thread is ready and nothing is scheduled that could make one ready.
    Deadlock,
    // The game called ExitProcess.
    ProcessExited,
    // The game called Break. The detail is the reason it gave.
    Break,
    // The game sent a service a command nothing here implements. The detail is
    // the command's header and the service is named. The calling thread is
    // left before its SendSyncRequest, as for a system call.
    UnimplementedServiceCommand,
};

struct KernelStop {
    KernelStopReason reason = KernelStopReason::BudgetSpent;
    std::uint32_t detail = 0;
    arm::Stop processor{};
    std::uint32_t thread_id = 0;
    std::uint32_t address = 0;
    std::string service;
};

// r0 to r4, the registers a system call takes its arguments in. CreateThread
// and ControlMemory use all five.
constexpr std::size_t SystemCallArgumentCount = 5;

// One system call as the game made it, for a trace.
struct SystemCallRecord {
    std::uint64_t tick = 0;
    std::uint32_t thread_id = 0;
    // The address of the svc instruction.
    std::uint32_t address = 0;
    std::uint32_t number = 0;
    // r0 to r4 as the call found them, and r0 as it left it. A call that puts
    // the thread to sleep leaves the answer it gives on a timeout.
    std::array<std::uint32_t, SystemCallArgumentCount> arguments{};
    std::uint32_t result = 0;
    bool implemented = false;
};

// One IPC request as the game sent it, for a trace.
struct ServiceRequestRecord {
    std::uint64_t tick = 0;
    std::uint32_t thread_id = 0;
    std::string_view service;
    std::uint32_t header = 0;
    // The result word of the reply, or zero for a command not implemented.
    std::uint32_t result = 0;
    bool implemented = false;
};

// The HLE kernel: one process, its address space, its handles and its threads,
// running on the interpreter, with each system call the game makes answered
// here. docs/horizon-kernel.md lists what each call does and where it came
// from.
class Kernel {
public:
    explicit Kernel(const KernelConfig& config);

    [[nodiscard]] AddressSpace& Space() {
        return space_;
    }

    [[nodiscard]] const AddressSpace& Space() const {
        return space_;
    }

    [[nodiscard]] Scheduler& Threads() {
        return scheduler_;
    }

    [[nodiscard]] const Scheduler& Threads() const {
        return scheduler_;
    }

    [[nodiscard]] HandleTable& Handles() {
        return handles_;
    }

    [[nodiscard]] const SystemPages& Pages() const {
        return pages_;
    }

    // Creates the main thread where the loader says it starts.
    void Start(const MainThreadStart& start);

    // Runs the game's threads for up to a number of instructions.
    KernelStop Run(std::uint64_t instructions);

    // Every instruction run so far, which is also the clock in ticks.
    [[nodiscard]] std::uint64_t Executed() const {
        return executed_;
    }

    void OnSystemCall(std::function<void(const SystemCallRecord&)> observer) {
        system_call_observer_ = std::move(observer);
    }

    void OnDebugOutput(std::function<void(std::string_view)> observer) {
        debug_output_observer_ = std::move(observer);
    }

    void OnServiceRequest(std::function<void(const ServiceRequestRecord&)> observer) {
        service_request_observer_ = std::move(observer);
    }

    // Makes a service reachable by ConnectToPort under a port name. srv: is
    // the one port the game connects to; every other service it reaches
    // through srv:.
    void RegisterPort(std::string name, std::shared_ptr<SessionHandler> handler);

    [[nodiscard]] std::uint32_t ProcessId() const {
        return config_.process_id;
    }

    // The name of a system call, from 3dbrew's list, and whether it is
    // implemented here.
    [[nodiscard]] static std::string_view SystemCallName(std::uint32_t number);
    [[nodiscard]] static bool IsImplemented(std::uint32_t number);

    // A block of memory from the BASE region, the kind a service hands out for
    // the game to map, such as the GPU's shared memory.
    [[nodiscard]] std::shared_ptr<SharedMemory>
    CreateSystemMemoryBlock(std::uint32_t size, std::uint32_t other_permissions);

    // Signals an event the way SignalEvent does, for services.
    void Signal(Event& event);

private:
    // The svc immediate has 24 bits, and the kernel's table 128 entries; a
    // number past the table is unknown.
    static constexpr std::size_t SystemCallCount = 0x80;

    using SystemCall = void (*)(Kernel&, Thread&);

    // Calls a member that answers a system call, const or not, so that every
    // entry of the table has one type.
    template<auto Method>
    static void Invoke(Kernel& kernel, Thread& thread) {
        (kernel.*Method)(thread);
    }

    struct SystemCallEntry {
        std::string_view name;
        SystemCall handler = nullptr;
    };

    static const std::array<SystemCallEntry, SystemCallCount>& SystemCalls();

    // What a system call asks of Run beyond answering in registers.
    enum class Outcome : std::uint8_t { Continue, Exited, Break, UnimplementedCommand };

    bool Dispatch(Thread& thread, std::uint32_t number);

    // Answers the system call a thread stopped at, and says whether Run has to
    // stop for it.
    std::optional<KernelStop> AnswerSystemCall(Thread& thread, std::uint32_t number);
    [[nodiscard]] std::shared_ptr<Thread> ResolveThread(Handle handle) const;
    [[nodiscard]] std::shared_ptr<WaitObject> ResolveWaitObject(Handle handle) const;
    static void Answer(Thread& thread, ResultCode result);
    void AnswerHandle(Thread& thread, const std::shared_ptr<Object>& object);
    std::uint32_t AllocateBaseMemory(std::uint32_t size);
    ResultCode MapSharedMemory(SharedMemory& block,
                               std::uint32_t address,
                               std::uint32_t permissions,
                               std::uint32_t other_permissions);

    // One function per system call, named after it, reading its arguments
    // from the calling thread's registers and answering in them. The
    // register each argument arrives in is the one Azahar's SVCWrapper derives
    // from the call's C prototype, the convention 3dbrew documents.
    void ControlMemory(Thread& thread);
    void QueryMemory(Thread& thread);
    void ExitProcess(Thread& thread);
    void CreateThread(Thread& thread);
    void ExitThread(Thread& thread);
    void SleepThread(Thread& thread);
    void GetThreadPriority(Thread& thread);
    void SetThreadPriority(Thread& thread);
    void CreateMutex(Thread& thread);
    void ReleaseMutex(Thread& thread);
    void CreateSemaphore(Thread& thread);
    void ReleaseSemaphore(Thread& thread);
    void CreateEvent(Thread& thread);
    void SignalEvent(Thread& thread);
    void ClearEvent(Thread& thread);
    void CreateTimer(Thread& thread);
    void SetTimer(Thread& thread);
    void CancelTimer(Thread& thread);
    void ClearTimer(Thread& thread);
    void CreateMemoryBlock(Thread& thread);
    void MapMemoryBlock(Thread& thread);
    void UnmapMemoryBlock(Thread& thread);
    void CreateAddressArbiter(Thread& thread);
    void ArbitrateAddress(Thread& thread);
    void CloseHandle(Thread& thread);
    void WaitSynchronization1(Thread& thread);
    void WaitSynchronizationN(Thread& thread);
    void DuplicateHandle(Thread& thread);
    void GetSystemTick(Thread& thread);
    void GetHandleInfo(Thread& thread);
    void GetProcessId(Thread& thread) const;
    void GetThreadId(Thread& thread);
    void GetResourceLimit(Thread& thread);
    void GetResourceLimitLimitValues(Thread& thread);
    void GetResourceLimitCurrentValues(Thread& thread);
    void Break(Thread& thread);
    void OutputDebugString(Thread& thread);
    void ConnectToPort(Thread& thread);
    void SendSyncRequest(Thread& thread);

    [[nodiscard]] std::int64_t ResourceLimitValue(std::uint32_t type) const;
    [[nodiscard]] std::int64_t ResourceCurrentValue(std::uint32_t type) const;

    KernelConfig config_;
    AddressSpace space_;
    Scheduler scheduler_;
    HandleTable handles_;
    SystemPages pages_;
    std::shared_ptr<ResourceLimit> resource_limit_;
    RegionAllocator base_region_;

    // Every object the game has made, by type, for the resource counts.
    std::array<std::vector<std::weak_ptr<Object>>, 16> created_;

    std::uint64_t executed_ = 0;
    Thread* last_run_ = nullptr;
    Outcome outcome_ = Outcome::Continue;
    std::uint32_t break_reason_ = 0;
    std::string unimplemented_service_;
    std::uint32_t unimplemented_header_ = 0;

    std::map<std::string, std::shared_ptr<SessionHandler>, std::less<>> ports_;

    std::function<void(const SystemCallRecord&)> system_call_observer_;
    std::function<void(std::string_view)> debug_output_observer_;
    std::function<void(const ServiceRequestRecord&)> service_request_observer_;
};

}  // namespace hexlith::domain::kernel

#endif  // HEXLITH_DOMAIN_KERNEL_KERNEL_H
