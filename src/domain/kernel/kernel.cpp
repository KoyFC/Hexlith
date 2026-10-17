#include "domain/kernel/kernel.h"

#include <algorithm>

namespace hexlith::domain::kernel {
namespace {

// When the extended header does not say, the size Azahar gives every handle
// table. Source: HandleTable::MAX_COUNT.
constexpr std::size_t DefaultHandleCapacity = 4096;

// The bytes an svc instruction takes, to find it again from r15.
constexpr std::uint32_t ArmInstructionSize = 4;
constexpr std::uint32_t ThumbInstructionSize = 2;

// Run never hands the interpreter more than this at once, so that a thread
// spinning with nothing scheduled still returns to the loop now and then.
constexpr std::uint64_t LongestSlice = 1'000'000;

// A stop, with the thread and address it happened at when there is one.
KernelStop StopFor(KernelStopReason reason,
                   std::uint32_t detail = 0,
                   const Thread* thread = nullptr,
                   std::uint32_t address = 0) {
    KernelStop stop;
    stop.reason = reason;
    stop.detail = detail;
    stop.thread_id = thread == nullptr ? 0 : thread->id;
    stop.address = address;
    return stop;
}

}  // namespace

Kernel::Kernel(const KernelConfig& config)
    : config_(config),
      space_(config.regions.application, config.linear_heap_base),
      handles_(config.handle_capacity == 0 ? DefaultHandleCapacity : config.handle_capacity),
      pages_(MakeSystemPages(config.memory_mode, config.regions)),
      resource_limit_(std::make_shared<ResourceLimit>()),
      system_region_(config.regions.system),
      base_region_(config.regions.base) {}

void Kernel::Start(const MainThreadStart& start) {
    const std::shared_ptr<Thread> thread =
        scheduler_.CreateThread(start.priority, start.ideal_processor);
    thread->context.registers.at(arm::ProgramCounter) = start.entry_point & ~1U;
    thread->context.thumb = (start.entry_point & 1U) != 0;
    thread->context.registers.at(arm::StackPointer) = start.stack_top;
    thread->context.fpscr = start.fpscr;
    thread->thread_local_storage = space_.AllocateThreadLocalStorage();
    thread->context.thread_id_read_only = thread->thread_local_storage;
    created_.at(static_cast<std::size_t>(ObjectType::Thread)).push_back(thread);
}

KernelStop Kernel::Run(std::uint64_t instructions) {
    std::uint64_t done = 0;
    while (done < instructions) {
        Thread* thread = scheduler_.Reschedule();
        if (thread == nullptr) {
            if (!scheduler_.AdvanceToNextEvent()) {
                return StopFor(KernelStopReason::Deadlock);
            }
            continue;
        }
        // The kernel clears the exclusive monitor when it switches threads, so a
        // store exclusive never succeeds on a load made by another thread.
        if (thread != last_run_) {
            thread->context.exclusive_open = false;
            last_run_ = thread;
        }

        // Run up to the next timed event, so it happens at its tick.
        std::uint64_t slice = std::min(instructions - done, LongestSlice);
        if (const std::optional<std::uint64_t> next = scheduler_.NextEventTick()) {
            const std::uint64_t now = scheduler_.Ticks();
            slice = std::min(slice, *next > now ? *next - now : 1);
        }

        std::uint64_t executed = 0;
        const arm::Stop stop = arm::Run(thread->context, space_.Memory(), slice, executed);
        done += executed;
        executed_ += executed;
        scheduler_.Advance(executed);

        if (stop.reason == arm::StopReason::StepLimit) {
            continue;
        }
        if (stop.reason != arm::StopReason::SupervisorCall) {
            KernelStop halt = StopFor(KernelStopReason::ProcessorStop,
                                      0,
                                      thread,
                                      thread->context.registers.at(arm::ProgramCounter));
            halt.processor = stop;
            return halt;
        }

        if (std::optional<KernelStop> halt = AnswerSystemCall(*thread, stop.detail)) {
            return *halt;
        }
    }
    return StopFor(KernelStopReason::BudgetSpent);
}

std::optional<KernelStop> Kernel::AnswerSystemCall(Thread& thread, std::uint32_t number) {
    const std::uint32_t call_address =
        thread.context.registers.at(arm::ProgramCounter) -
        (thread.context.thumb ? ThumbInstructionSize : ArmInstructionSize);
    SystemCallRecord record{.tick = scheduler_.Ticks(),
                            .thread_id = thread.id,
                            .address = call_address,
                            .number = number};
    std::copy_n(
        thread.context.registers.begin(), SystemCallArgumentCount, record.arguments.begin());

    outcome_ = Outcome::Continue;
    record.implemented = Dispatch(thread, number);
    record.result = thread.context.registers.at(0);
    if (system_call_observer_) {
        system_call_observer_(record);
    }

    if (!record.implemented) {
        // Left before the call, so that once it is implemented the run can
        // carry on from exactly here.
        thread.context.registers.at(arm::ProgramCounter) = call_address;
        return StopFor(KernelStopReason::UnimplementedSystemCall, number, &thread, call_address);
    }
    if (outcome_ == Outcome::UnimplementedCommand) {
        thread.context.registers.at(arm::ProgramCounter) = call_address;
        KernelStop halt = StopFor(KernelStopReason::UnimplementedServiceCommand,
                                  unimplemented_header_,
                                  &thread,
                                  call_address);
        halt.service = unimplemented_service_;
        return halt;
    }
    if (outcome_ == Outcome::Exited) {
        return StopFor(KernelStopReason::ProcessExited, 0, &thread);
    }
    if (outcome_ == Outcome::Break) {
        return StopFor(KernelStopReason::Break, break_reason_, &thread, call_address);
    }
    if (stop_requested_) {
        stop_requested_ = false;
        return StopFor(KernelStopReason::Requested,
                       0,
                       &thread,
                       thread.context.registers.at(arm::ProgramCounter));
    }
    return std::nullopt;
}

bool Kernel::Dispatch(Thread& thread, std::uint32_t number) {
    const std::array<SystemCallEntry, SystemCallCount>& calls = SystemCalls();
    if (number >= calls.size() || calls.at(number).handler == nullptr) {
        return false;
    }
    // One indirect call per system call, not per instruction: the
    // interpreter has already stopped to get here.
    calls.at(number).handler(*this, thread);
    return true;
}

std::string_view Kernel::SystemCallName(std::uint32_t number) {
    const std::array<SystemCallEntry, SystemCallCount>& calls = SystemCalls();
    return number < calls.size() ? calls.at(number).name : std::string_view{"Unknown"};
}

bool Kernel::IsImplemented(std::uint32_t number) {
    const std::array<SystemCallEntry, SystemCallCount>& calls = SystemCalls();
    return number < calls.size() && calls.at(number).handler != nullptr;
}

const std::array<Kernel::SystemCallEntry, Kernel::SystemCallCount>& Kernel::SystemCalls() {
    // Names from 3dbrew's SVC page, in the order Azahar's table gives them.
    // A call with no handler stops the run with its name.
    static const std::array<SystemCallEntry, SystemCallCount> Calls = {{
        {.name = "Unknown"},
        {.name = "ControlMemory", .handler = &Invoke<&Kernel::ControlMemory>},
        {.name = "QueryMemory", .handler = &Invoke<&Kernel::QueryMemory>},
        {.name = "ExitProcess", .handler = &Invoke<&Kernel::ExitProcess>},
        {.name = "GetProcessAffinityMask"},
        {.name = "SetProcessAffinityMask"},
        {.name = "GetProcessIdealProcessor"},
        {.name = "SetProcessIdealProcessor"},
        {.name = "CreateThread", .handler = &Invoke<&Kernel::CreateThread>},
        {.name = "ExitThread", .handler = &Invoke<&Kernel::ExitThread>},
        {.name = "SleepThread", .handler = &Invoke<&Kernel::SleepThread>},
        {.name = "GetThreadPriority", .handler = &Invoke<&Kernel::GetThreadPriority>},
        {.name = "SetThreadPriority", .handler = &Invoke<&Kernel::SetThreadPriority>},
        {.name = "GetThreadAffinityMask"},
        {.name = "SetThreadAffinityMask"},
        {.name = "GetThreadIdealProcessor"},
        {.name = "SetThreadIdealProcessor"},
        {.name = "GetCurrentProcessorNumber"},
        {.name = "Run"},
        {.name = "CreateMutex", .handler = &Invoke<&Kernel::CreateMutex>},
        {.name = "ReleaseMutex", .handler = &Invoke<&Kernel::ReleaseMutex>},
        {.name = "CreateSemaphore", .handler = &Invoke<&Kernel::CreateSemaphore>},
        {.name = "ReleaseSemaphore", .handler = &Invoke<&Kernel::ReleaseSemaphore>},
        {.name = "CreateEvent", .handler = &Invoke<&Kernel::CreateEvent>},
        {.name = "SignalEvent", .handler = &Invoke<&Kernel::SignalEvent>},
        {.name = "ClearEvent", .handler = &Invoke<&Kernel::ClearEvent>},
        {.name = "CreateTimer", .handler = &Invoke<&Kernel::CreateTimer>},
        {.name = "SetTimer", .handler = &Invoke<&Kernel::SetTimer>},
        {.name = "CancelTimer", .handler = &Invoke<&Kernel::CancelTimer>},
        {.name = "ClearTimer", .handler = &Invoke<&Kernel::ClearTimer>},
        {.name = "CreateMemoryBlock", .handler = &Invoke<&Kernel::CreateMemoryBlock>},
        {.name = "MapMemoryBlock", .handler = &Invoke<&Kernel::MapMemoryBlock>},
        {.name = "UnmapMemoryBlock", .handler = &Invoke<&Kernel::UnmapMemoryBlock>},
        {.name = "CreateAddressArbiter", .handler = &Invoke<&Kernel::CreateAddressArbiter>},
        {.name = "ArbitrateAddress", .handler = &Invoke<&Kernel::ArbitrateAddress>},
        {.name = "CloseHandle", .handler = &Invoke<&Kernel::CloseHandle>},
        {.name = "WaitSynchronization1", .handler = &Invoke<&Kernel::WaitSynchronization1>},
        {.name = "WaitSynchronizationN", .handler = &Invoke<&Kernel::WaitSynchronizationN>},
        {.name = "SignalAndWait"},
        {.name = "DuplicateHandle", .handler = &Invoke<&Kernel::DuplicateHandle>},
        {.name = "GetSystemTick", .handler = &Invoke<&Kernel::GetSystemTick>},
        {.name = "GetHandleInfo", .handler = &Invoke<&Kernel::GetHandleInfo>},
        {.name = "GetSystemInfo"},
        {.name = "GetProcessInfo"},
        {.name = "GetThreadInfo"},
        {.name = "ConnectToPort", .handler = &Invoke<&Kernel::ConnectToPort>},
        {.name = "SendSyncRequest1"},
        {.name = "SendSyncRequest2"},
        {.name = "SendSyncRequest3"},
        {.name = "SendSyncRequest4"},
        {.name = "SendSyncRequest", .handler = &Invoke<&Kernel::SendSyncRequest>},
        {.name = "OpenProcess"},
        {.name = "OpenThread"},
        {.name = "GetProcessId", .handler = &Invoke<&Kernel::GetProcessId>},
        {.name = "GetProcessIdOfThread"},
        {.name = "GetThreadId", .handler = &Invoke<&Kernel::GetThreadId>},
        {.name = "GetResourceLimit", .handler = &Invoke<&Kernel::GetResourceLimit>},
        {.name = "GetResourceLimitLimitValues",
         .handler = &Invoke<&Kernel::GetResourceLimitLimitValues>},
        {.name = "GetResourceLimitCurrentValues",
         .handler = &Invoke<&Kernel::GetResourceLimitCurrentValues>},
        {.name = "GetThreadContext"},
        {.name = "Break", .handler = &Invoke<&Kernel::Break>},
        {.name = "OutputDebugString", .handler = &Invoke<&Kernel::OutputDebugString>},
        {.name = "ControlPerformanceCounter"},
        {.name = "Unknown"},
        {.name = "Unknown"},
        {.name = "Unknown"},
        {.name = "Unknown"},
        {.name = "Unknown"},
        {.name = "Unknown"},
        {.name = "Unknown"},
        {.name = "Unknown"},
        {.name = "CreatePort"},
        {.name = "CreateSessionToPort"},
        {.name = "CreateSession"},
        {.name = "AcceptSession"},
        {.name = "ReplyAndReceive1"},
        {.name = "ReplyAndReceive2"},
        {.name = "ReplyAndReceive3"},
        {.name = "ReplyAndReceive4"},
        {.name = "ReplyAndReceive"},
        {.name = "BindInterrupt"},
        {.name = "UnbindInterrupt"},
        {.name = "InvalidateProcessDataCache"},
        {.name = "StoreProcessDataCache"},
        {.name = "FlushProcessDataCache"},
        {.name = "StartInterProcessDma"},
        {.name = "StopDma"},
        {.name = "GetDmaState"},
        {.name = "RestartDma"},
        {.name = "SetGpuProt"},
        {.name = "SetWifiEnabled"},
        {.name = "Unknown"},
        {.name = "Unknown"},
        {.name = "Unknown"},
        {.name = "Unknown"},
        {.name = "Unknown"},
        {.name = "DebugActiveProcess"},
        {.name = "BreakDebugProcess"},
        {.name = "TerminateDebugProcess"},
        {.name = "GetProcessDebugEvent"},
        {.name = "ContinueDebugEvent"},
        {.name = "GetProcessList"},
        {.name = "GetThreadList"},
        {.name = "GetDebugThreadContext"},
        {.name = "SetDebugThreadContext"},
        {.name = "QueryDebugProcessMemory"},
        {.name = "ReadProcessMemory"},
        {.name = "WriteProcessMemory"},
        {.name = "SetHardwareBreakPoint"},
        {.name = "GetDebugThreadParam"},
        {.name = "Unknown"},
        {.name = "Unknown"},
        {.name = "ControlProcessMemory"},
        {.name = "MapProcessMemory"},
        {.name = "UnmapProcessMemory"},
        {.name = "CreateCodeSet"},
        {.name = "RandomStub"},
        {.name = "CreateProcess"},
        {.name = "TerminateProcess"},
        {.name = "SetProcessResourceLimits"},
        {.name = "CreateResourceLimit"},
        {.name = "SetResourceLimitLimitValues"},
        {.name = "AddCodeSegment"},
        {.name = "Backdoor"},
        {.name = "KernelSetState"},
        {.name = "QueryProcessMemory"},
        {.name = "Unused"},
        {.name = "Unused"},
    }};
    return Calls;
}

void Kernel::Answer(Thread& thread, ResultCode result) {
    thread.context.registers.at(0) = result.raw;
}

void Kernel::AnswerHandle(Thread& thread, const std::shared_ptr<Object>& object) {
    const std::optional<Handle> handle = handles_.Create(object);
    if (!handle.has_value()) {
        Answer(thread, result::OutOfHandles);
        return;
    }
    created_.at(static_cast<std::size_t>(object->Type())).push_back(object);
    Answer(thread, result::Success);
    thread.context.registers.at(1) = *handle;
}

std::shared_ptr<Thread> Kernel::ResolveThread(Handle handle) const {
    if (handle == CurrentThreadHandle) {
        return scheduler_.CurrentShared();
    }
    return handles_.Get<Thread>(handle, ObjectType::Thread);
}

std::shared_ptr<WaitObject> Kernel::ResolveWaitObject(Handle handle) const {
    if (handle == CurrentThreadHandle) {
        return scheduler_.CurrentShared();
    }
    return std::dynamic_pointer_cast<WaitObject>(handles_.Get(handle));
}

void Kernel::MapDeviceMemory(std::span<const ctr::MappedRange> ranges) {
    for (const ctr::MappedRange& range : ranges) {
        const bool vram = range.start >= VramBase && range.end <= VramBase + VramSize;
        const bool dsp = range.start >= DspMemoryBase && range.end <= DspMemoryBase + DspMemorySize;
        if ((!vram && !dsp) || range.end <= range.start) {
            continue;
        }
        const std::shared_ptr<memory::Backing>& backing = vram ? vram_ : dsp_memory_;
        const std::uint32_t base = vram ? VramBase : DspMemoryBase;
        static_cast<void>(
            space_.MapBacking(range.start,
                              range.end - range.start,
                              backing,
                              range.start - base,
                              range.read_only ? memory::Access::Read : memory::Access::ReadWrite,
                              range.is_static ? MemoryState::Static : MemoryState::Io));
    }
}

void Kernel::RegisterPort(std::string name, std::shared_ptr<SessionHandler> handler) {
    ports_.insert_or_assign(std::move(name), std::move(handler));
}

void Kernel::Signal(Event& event) {
    event.signaled = true;
    scheduler_.Signal(event);
}

}  // namespace hexlith::domain::kernel
