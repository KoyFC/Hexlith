// The system calls the HLE kernel implements, one function each. The
// semantics and the result codes are those of SVC in Azahar,
// src/core/hle/kernel/svc.cpp, and docs/horizon-kernel.md lists them.

#include "domain/kernel/kernel.h"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

namespace hexlith::domain::kernel {
namespace {

using memory::Access;

// ControlMemory's operation word: the operation in the low byte, a region in
// bits 8 to 11 that Azahar ignores, and a flag for linear memory. Source: the
// MEMOP constants in Azahar's svc.cpp; 3dbrew, SVC.
constexpr std::uint32_t OperationMask = 0xFF;
constexpr std::uint32_t RegionMask = 0xF00;
constexpr std::uint32_t LinearFlag = 0x10000;
constexpr std::uint32_t OperationFree = 1;
constexpr std::uint32_t OperationCommit = 3;
constexpr std::uint32_t OperationMap = 4;
constexpr std::uint32_t OperationUnmap = 5;
constexpr std::uint32_t OperationProtect = 6;

// MemoryPermission values. DontCare lets a memory block be mapped with
// whatever the other side asks for.
constexpr std::uint32_t PermissionRead = 1;
constexpr std::uint32_t PermissionReadWrite = 3;
constexpr std::uint32_t PermissionReadWriteExecute = 7;
constexpr std::uint32_t PermissionDontCare = 0x10000000;

constexpr std::uint32_t PageMask = layout::PageSize - 1;

// CreateThread's processor IDs: -2 for the core the extended header prefers,
// -1 for any, then the cores themselves, of which a New 3DS has four.
constexpr std::int32_t DefaultProcessor = -2;
constexpr std::int32_t AnyProcessor = -1;
constexpr std::int32_t ProcessorCount = 4;

// The application's resource limits. Source: the APPLICATION category in
// Azahar, src/core/hle/kernel/resource_limit.cpp.
constexpr std::uint32_t PriorityLimit = 0;
constexpr std::uint32_t CommitLimit = 1;
constexpr std::uint32_t ThreadLimit = 2;
constexpr std::uint32_t EventLimit = 3;
constexpr std::uint32_t MutexLimit = 4;
constexpr std::uint32_t SemaphoreLimit = 5;
constexpr std::uint32_t TimerLimit = 6;
constexpr std::uint32_t SharedMemoryLimit = 7;
constexpr std::uint32_t AddressArbiterLimit = 8;
constexpr std::uint32_t ResourceTypeCount = 10;

constexpr std::uint32_t HighestApplicationPriority = 0x18;
constexpr std::int64_t ApplicationThreads = 0x20;
constexpr std::int64_t ApplicationEvents = 0x20;
constexpr std::int64_t ApplicationMutexes = 0x20;
constexpr std::int64_t ApplicationSemaphores = 0x8;
constexpr std::int64_t ApplicationTimers = 0x8;
constexpr std::int64_t ApplicationSharedMemories = 0x10;
constexpr std::int64_t ApplicationAddressArbiters = 0x2;

// ArbitrateAddress's types. Source: 3dbrew, SVC; ArbitrationType in Azahar.
constexpr std::uint32_t ArbitrationSignal = 0;
constexpr std::uint32_t ArbitrationWaitIfLessThan = 1;
constexpr std::uint32_t ArbitrationDecrementAndWaitIfLessThan = 2;
constexpr std::uint32_t ArbitrationWaitIfLessThanWithTimeout = 3;
constexpr std::uint32_t ArbitrationDecrementAndWaitIfLessThanWithTimeout = 4;

// A signal that wakes nobody, made by a thread of priority 49 or lower, costs
// the caller 1614 ticks. Azahar takes the count from the console's kernel and
// lowers the priority threshold by one; it stops a low priority thread that
// signals in a loop from starving the rest.
constexpr std::uint32_t EmptySignalPenaltyPriority = 49;
constexpr std::uint64_t EmptySignalPenaltyTicks = 1614;

// GetHandleInfo's types.
constexpr std::uint32_t HandleInfoProcessTicks = 0;
constexpr std::uint32_t HandleInfoReferenceCount = 1;
constexpr std::uint32_t HandleInfoStubbed = 2;
constexpr std::uint32_t HandleInfoStubbedToo = 0x32107;

constexpr std::uint32_t AddressBits = 32;

// An event or timer's reset type is the low byte of its argument.
constexpr std::uint32_t ResetTypeMask = 0xFF;

// What WaitSynchronizationN reports as the index when nothing woke it: -1.
constexpr std::uint32_t NoWaitIndex = 0xFFFFFFFF;

// The sizes of the guest values the calls read and write in arrays.
constexpr std::uint32_t WordBytes = 4;
constexpr std::uint32_t DoublewordBytes = 8;

// A port name is at most 11 characters. Source: PortNameMaxLength in Azahar's
// svc.cpp; 3dbrew, SVC, ConnectToPort.
constexpr std::uint32_t PortNameMaxLength = 11;

namespace result_codes {
constexpr ResultCode PortNameTooLong{0xE0E0181E};
constexpr ResultCode NotAuthorized{0xD9001BEA};
constexpr ResultCode InvalidCombinationKernel{0xD90007EE};
constexpr ResultCode InvalidEnumValueFnd{0xD8E093ED};
constexpr ResultCode WrongPermission{0xD900182E};
}  // namespace result_codes

std::uint32_t& Register(Thread& thread, std::size_t index) {
    return thread.context.registers.at(index);
}

std::int64_t Combine(std::uint32_t low, std::uint32_t high) {
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(high) << AddressBits) | low);
}

void Answer64(Thread& thread, std::size_t low_register, std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    Register(thread, low_register) = static_cast<std::uint32_t>(bits);
    Register(thread, low_register + 1) = static_cast<std::uint32_t>(bits >> AddressBits);
}

bool IsBlockPermission(std::uint32_t permission) {
    return permission <= PermissionReadWrite || permission == PermissionDontCare;
}

bool IsMappingPermission(std::uint32_t permission) {
    return (permission >= PermissionRead && permission <= PermissionReadWriteExecute) ||
           permission == PermissionDontCare;
}

Access AccessOf(std::uint32_t permission) {
    return static_cast<Access>(permission & PermissionReadWriteExecute);
}

}  // namespace

void Kernel::ControlMemory(Thread& thread) {
    std::uint32_t operation = Register(thread, 0);
    const std::uint32_t first = Register(thread, 1);
    const std::uint32_t second = Register(thread, 2);
    const std::uint32_t size = Register(thread, 3);
    const std::uint32_t permission = Register(thread, 4);

    if ((first & PageMask) != 0 || (second & PageMask) != 0) {
        Answer(thread, result::MisalignedAddress);
        return;
    }
    if ((size & PageMask) != 0) {
        Answer(thread, result::MisalignedSize);
        return;
    }
    operation &= ~RegionMask;
    if ((permission & PermissionReadWrite) != permission) {
        Answer(thread, result::InvalidCombination);
        return;
    }
    const Access access = AccessOf(permission);

    switch (operation & OperationMask) {
    case OperationFree: {
        ResultCode freed = result::InvalidAddress;
        if (first >= layout::HeapBase && first < layout::HeapEnd) {
            freed = space_.FreeHeap(first, size);
        } else if (first >= space_.LinearHeapBase() && first < space_.LinearHeapEnd()) {
            freed = space_.FreeLinear(first, size);
        }
        Answer(thread, freed);
        Register(thread, 1) = first;
        return;
    }
    case OperationCommit:
        if ((operation & LinearFlag) != 0) {
            const AddressResult linear = space_.CommitLinear(first, size, access);
            Answer(thread, linear.result);
            Register(thread, 1) = linear.address;
        } else {
            Answer(thread, space_.CommitHeap(first, size, access));
            Register(thread, 1) = first;
        }
        return;
    case OperationMap:
        Answer(thread, space_.MapAlias(first, second, size, access, false));
        return;
    case OperationUnmap:
        Answer(thread, space_.UnmapAlias(first, second, size, access, false));
        return;
    case OperationProtect:
        Answer(thread, space_.Protect(first, size, access));
        return;
    default:
        Answer(thread, result::InvalidCombination);
        return;
    }
}

void Kernel::QueryMemory(Thread& thread) {
    const MemoryInfo info = space_.Query(Register(thread, 2));
    Answer(thread, result::Success);
    Register(thread, 1) = info.base;
    Register(thread, 2) = info.size;
    Register(thread, 3) = static_cast<std::uint32_t>(info.access);
    Register(thread, 4) = static_cast<std::uint32_t>(info.state);
    // Page flags, which the kernel reports as zero.
    constexpr std::size_t PageFlagsRegister = 5;
    Register(thread, PageFlagsRegister) = 0;
}

void Kernel::ExitProcess(Thread& /*thread*/) {
    outcome_ = Outcome::Exited;
}

void Kernel::CreateThread(Thread& thread) {
    const std::uint32_t priority = Register(thread, 0);
    const std::uint32_t entry = Register(thread, 1);
    const std::uint32_t argument = Register(thread, 2);
    const std::uint32_t stack_top = Register(thread, 3);
    auto processor = static_cast<std::int32_t>(Register(thread, 4));

    if (priority > LowestThreadPriority) {
        Answer(thread, result::OutOfRange);
        return;
    }
    if (priority < HighestApplicationPriority) {
        Answer(thread, result_codes::NotAuthorized);
        return;
    }
    if (processor == DefaultProcessor) {
        processor = config_.ideal_processor;
    } else if (processor == AnyProcessor) {
        processor = 0;
    } else if (processor < 0 || processor >= ProcessorCount) {
        Answer(thread, result::OutOfRange);
        return;
    }

    const std::shared_ptr<Thread> created = scheduler_.CreateThread(priority, processor);
    created->context.registers.at(0) = argument;
    created->context.registers.at(arm::ProgramCounter) = entry & ~1U;
    created->context.thumb = (entry & 1U) != 0;
    created->context.registers.at(arm::StackPointer) = stack_top;
    // Default NaN, flush to zero, round towards zero. Azahar's value for a
    // thread the game creates, which differs from the main thread's by the
    // inexact flag.
    constexpr std::uint32_t CreatedThreadFpscr = 0x03C00000;
    created->context.fpscr = CreatedThreadFpscr;
    created->thread_local_storage = space_.AllocateThreadLocalStorage();
    created->context.thread_id_read_only = created->thread_local_storage;
    AnswerHandle(thread, created);
}

void Kernel::ExitThread(Thread& thread) {
    space_.FreeThreadLocalStorage(thread.thread_local_storage);
    scheduler_.Exit(thread);
}

void Kernel::SleepThread(Thread& thread) {
    const std::int64_t nanoseconds = Combine(Register(thread, 0), Register(thread, 1));
    if (nanoseconds == 0) {
        scheduler_.Yield();
    } else {
        scheduler_.Sleep(nanoseconds);
    }
}

void Kernel::GetThreadPriority(Thread& thread) {
    const std::shared_ptr<Thread> target = ResolveThread(Register(thread, 1));
    if (target == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    Answer(thread, result::Success);
    Register(thread, 1) = target->current_priority;
}

void Kernel::SetThreadPriority(Thread& thread) {
    const std::uint32_t priority = Register(thread, 1);
    if (priority > LowestThreadPriority) {
        Answer(thread, result::OutOfRange);
        return;
    }
    const std::shared_ptr<Thread> target = ResolveThread(Register(thread, 0));
    if (target == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    if (priority < HighestApplicationPriority) {
        Answer(thread, result_codes::NotAuthorized);
        return;
    }
    scheduler_.SetPriority(*target, priority);
    Answer(thread, result::Success);
}

void Kernel::CreateMutex(Thread& thread) {
    const auto mutex = std::make_shared<Mutex>();
    if (Register(thread, 1) != 0) {
        mutex->Acquire(thread);
    }
    AnswerHandle(thread, mutex);
}

void Kernel::ReleaseMutex(Thread& thread) {
    const auto mutex = handles_.Get<Mutex>(Register(thread, 0), ObjectType::Mutex);
    if (mutex == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    Answer(thread, scheduler_.ReleaseMutex(thread, *mutex));
}

void Kernel::CreateSemaphore(Thread& thread) {
    const auto initial = static_cast<std::int32_t>(Register(thread, 1));
    const auto maximum = static_cast<std::int32_t>(Register(thread, 2));
    if (initial > maximum) {
        Answer(thread, result_codes::InvalidCombinationKernel);
        return;
    }
    AnswerHandle(thread, std::make_shared<Semaphore>(initial, maximum));
}

void Kernel::ReleaseSemaphore(Thread& thread) {
    const auto semaphore = handles_.Get<Semaphore>(Register(thread, 1), ObjectType::Semaphore);
    if (semaphore == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    const auto count = static_cast<std::int32_t>(Register(thread, 2));
    if (semaphore->maximum < count + semaphore->available) {
        Answer(thread, result::OutOfRangeKernel);
        return;
    }
    const std::int32_t previous = semaphore->available;
    semaphore->available += count;
    scheduler_.Signal(*semaphore);
    Answer(thread, result::Success);
    Register(thread, 1) = static_cast<std::uint32_t>(previous);
}

void Kernel::CreateEvent(Thread& thread) {
    AnswerHandle(
        thread,
        std::make_shared<Event>(static_cast<ResetType>(Register(thread, 1) & ResetTypeMask)));
}

void Kernel::SignalEvent(Thread& thread) {
    const auto event = handles_.Get<Event>(Register(thread, 0), ObjectType::Event);
    if (event == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    Signal(*event);
    Answer(thread, result::Success);
}

void Kernel::ClearEvent(Thread& thread) {
    const auto event = handles_.Get<Event>(Register(thread, 0), ObjectType::Event);
    if (event == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    event->signaled = false;
    Answer(thread, result::Success);
}

void Kernel::CreateTimer(Thread& thread) {
    AnswerHandle(
        thread,
        std::make_shared<Timer>(static_cast<ResetType>(Register(thread, 1) & ResetTypeMask)));
}

void Kernel::SetTimer(Thread& thread) {
    const std::int64_t initial = Combine(Register(thread, 2), Register(thread, 3));
    const std::int64_t interval = Combine(Register(thread, 1), Register(thread, 4));
    if (initial < 0 || interval < 0) {
        Answer(thread, result::OutOfRangeKernel);
        return;
    }
    const auto timer = handles_.Get<Timer>(Register(thread, 0), ObjectType::Timer);
    if (timer == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    // Answered first, because a timer with no initial delay fires at once and
    // may wake a thread that preempts this one.
    Answer(thread, result::Success);
    scheduler_.ArmTimer(timer, initial, interval);
}

void Kernel::CancelTimer(Thread& thread) {
    const auto timer = handles_.Get<Timer>(Register(thread, 0), ObjectType::Timer);
    if (timer == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    Scheduler::DisarmTimer(*timer);
    Answer(thread, result::Success);
}

void Kernel::ClearTimer(Thread& thread) {
    const auto timer = handles_.Get<Timer>(Register(thread, 0), ObjectType::Timer);
    if (timer == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    timer->signaled = false;
    Answer(thread, result::Success);
}

std::uint32_t Kernel::AllocateRegionMemory(Region region, std::uint32_t size) {
    // FCRAM holds the application, system and BASE regions in that order, and
    // a block taken from one sits at the linear address of its offset in FCRAM,
    // in the old linear range whatever the title's own linear heap. So the
    // first block of the system region, which is the shared font, lands at
    // 0x18000000. Source: KernelSystem::MemoryInit, CreateSharedMemory and
    // SharedMemory::Map in Azahar.
    RegionAllocator& allocator = region == Region::System ? system_region_ : base_region_;
    const std::uint32_t region_start = region == Region::System
                                           ? config_.regions.application
                                           : config_.regions.application + config_.regions.system;
    const std::optional<std::uint32_t> offset = allocator.TakeLowest(size);
    if (!offset.has_value()) {
        return 0;
    }
    return layout::OldLinearHeapBase + region_start + *offset;
}

std::shared_ptr<SharedMemory> Kernel::CreateSystemMemoryBlock(std::uint32_t size,
                                                              std::uint32_t other_permissions,
                                                              Region region) {
    auto block = std::make_shared<SharedMemory>();
    const std::shared_ptr<memory::Backing> backing = memory::Backing::Allocate(size);
    for (std::uint32_t offset = 0; offset < size; offset += layout::PageSize) {
        block->pages.push_back(memory::PageTarget{.backing = backing, .offset = offset});
    }
    block->size = size;
    block->linear_address = AllocateRegionMemory(region, size);
    block->permissions = PermissionReadWrite;
    block->other_permissions = other_permissions;
    return block;
}

void Kernel::CreateMemoryBlock(Thread& thread) {
    const std::uint32_t other_permission = Register(thread, 0);
    const std::uint32_t address = Register(thread, 1);
    const std::uint32_t size = Register(thread, 2);
    const std::uint32_t permission = Register(thread, 3);

    if ((size & PageMask) != 0) {
        Answer(thread, result::MisalignedSize);
        return;
    }
    if (!IsBlockPermission(permission) || !IsBlockPermission(other_permission)) {
        Answer(thread, result::InvalidCombination);
        return;
    }
    const std::uint64_t end = static_cast<std::uint64_t>(address) + size;
    if (address != 0 && (address < layout::ImageBase || end > layout::SharedMemoryEnd)) {
        Answer(thread, result::InvalidAddress);
        return;
    }

    std::shared_ptr<SharedMemory> block;
    if (address == 0) {
        block = CreateSystemMemoryBlock(size, other_permission);
        block->permissions = permission;
    } else {
        // Made from the game's own heap, which is locked to the access the
        // game gives itself for as long as the block exists.
        const ResultCode locked = space_.ChangeState(
            address, size, MemoryState::Private, MemoryState::Locked, AccessOf(permission));
        if (locked.IsError()) {
            Answer(thread, locked);
            return;
        }
        block = std::make_shared<SharedMemory>();
        for (std::uint32_t offset = 0; offset < size; offset += layout::PageSize) {
            block->pages.push_back(
                space_.Memory().TargetOf(address + offset).value_or(memory::PageTarget{}));
        }
        block->size = size;
        block->base_address = address;
        block->permissions = permission;
        block->other_permissions = other_permission;
    }
    block->made_by_game = true;
    AnswerHandle(thread, block);
}

ResultCode Kernel::MapSharedMemory(SharedMemory& block,
                                   std::uint32_t address,
                                   std::uint32_t permissions,
                                   std::uint32_t other_permissions) {
    const std::uint32_t allowed = block.made_by_game ? block.permissions : block.other_permissions;
    if (block.base_address == 0 && other_permissions != PermissionDontCare) {
        return result::InvalidCombination;
    }
    if ((permissions & ~allowed) != 0) {
        return result::InvalidCombination;
    }
    if (block.base_address != 0 && other_permissions == PermissionDontCare) {
        return result::InvalidCombination;
    }
    if (other_permissions != PermissionDontCare && (block.permissions & ~other_permissions) != 0) {
        return result_codes::WrongPermission;
    }
    const std::uint64_t end = static_cast<std::uint64_t>(address) + block.size;
    if (address != 0 && (address < layout::HeapBase || end >= layout::SharedMemoryEnd)) {
        return result::InvalidAddress;
    }

    std::uint32_t target = address;
    if (block.base_address == 0 && target == 0) {
        target = block.linear_address;
    }
    for (std::uint32_t offset = 0; offset < block.size; offset += layout::PageSize) {
        if (space_.StateAt(target + offset) != MemoryState::Free) {
            return result::InvalidAddressState;
        }
    }
    for (std::size_t page = 0; page < block.pages.size(); ++page) {
        const memory::PageTarget& source = block.pages.at(page);
        const auto offset = static_cast<std::uint32_t>(page * layout::PageSize);
        static_cast<void>(space_.MapBacking(target + offset,
                                            layout::PageSize,
                                            source.backing,
                                            source.offset,
                                            AccessOf(permissions),
                                            MemoryState::Shared));
    }
    return result::Success;
}

void Kernel::MapMemoryBlock(Thread& thread) {
    const auto block = handles_.Get<SharedMemory>(Register(thread, 0), ObjectType::SharedMemory);
    if (block == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    const std::uint32_t permissions = Register(thread, 2);
    if (!IsMappingPermission(permissions)) {
        Answer(thread, result::InvalidCombination);
        return;
    }
    Answer(thread, MapSharedMemory(*block, Register(thread, 1), permissions, Register(thread, 3)));
}

void Kernel::UnmapMemoryBlock(Thread& thread) {
    const auto block = handles_.Get<SharedMemory>(Register(thread, 0), ObjectType::SharedMemory);
    if (block == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    Answer(thread, space_.UnmapBacking(Register(thread, 1), block->size));
}

void Kernel::CreateAddressArbiter(Thread& thread) {
    AnswerHandle(thread, std::make_shared<AddressArbiter>());
}

void Kernel::ArbitrateAddress(Thread& thread) {
    const auto arbiter =
        handles_.Get<AddressArbiter>(Register(thread, 0), ObjectType::AddressArbiter);
    if (arbiter == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    const std::uint32_t address = Register(thread, 1);
    const std::uint32_t type = Register(thread, 2);
    const auto value = static_cast<std::int32_t>(Register(thread, 3));
    constexpr std::size_t TimeoutHighRegister = 5;
    const std::int64_t timeout =
        Combine(Register(thread, 4), Register(thread, TimeoutHighRegister));

    if (type == ArbitrationSignal) {
        const std::size_t woken = scheduler_.SignalArbiter(*arbiter, address, value);
        Answer(thread, result::Success);
        if (woken == 0 && thread.current_priority >= EmptySignalPenaltyPriority) {
            scheduler_.Advance(EmptySignalPenaltyTicks);
        }
        return;
    }

    const bool with_timeout = type == ArbitrationWaitIfLessThanWithTimeout ||
                              type == ArbitrationDecrementAndWaitIfLessThanWithTimeout;
    const bool decrements = type == ArbitrationDecrementAndWaitIfLessThan ||
                            type == ArbitrationDecrementAndWaitIfLessThanWithTimeout;
    if (!with_timeout && !decrements && type != ArbitrationWaitIfLessThan) {
        Answer(thread, result_codes::InvalidEnumValueFnd);
        return;
    }
    const std::optional<std::uint32_t> word = space_.Memory().Read32(address);
    if (!word.has_value()) {
        // Azahar reads the word unchecked; the console would fault the caller.
        Answer(thread, result::InvalidPointer);
        return;
    }

    // The calls with a timeout answer Timeout whether or not they slept, which
    // Azahar notes the console does.
    Answer(thread, with_timeout ? result::Timeout : result::Success);
    const auto current = static_cast<std::int32_t>(*word);
    if (current >= value) {
        return;
    }
    if (decrements) {
        static_cast<void>(
            space_.Memory().Write32(address, static_cast<std::uint32_t>(current - 1)));
    }
    scheduler_.WaitOnArbiter(*arbiter, address, with_timeout ? timeout : -1);
}

void Kernel::CloseHandle(Thread& thread) {
    Answer(thread, handles_.Close(Register(thread, 0)) ? result::Success : result::InvalidHandle);
}

void Kernel::WaitSynchronization1(Thread& thread) {
    const std::shared_ptr<WaitObject> object = ResolveWaitObject(Register(thread, 0));
    const std::int64_t timeout = Combine(Register(thread, 2), Register(thread, 3));
    if (object == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    if (!object->ShouldWait(thread)) {
        object->Acquire(thread);
        Answer(thread, result::Success);
        return;
    }
    Answer(thread, result::Timeout);
    if (timeout != 0) {
        const std::vector<std::shared_ptr<WaitObject>> objects = {object};
        scheduler_.Wait(objects, false, timeout, false);
    }
}

void Kernel::WaitSynchronizationN(Thread& thread) {
    const std::int64_t timeout = Combine(Register(thread, 0), Register(thread, 4));
    const std::uint32_t handles_address = Register(thread, 1);
    const auto count = static_cast<std::int32_t>(Register(thread, 2));
    const bool wait_all = Register(thread, 3) != 0;

    if (count < 0) {
        Answer(thread, result::OutOfRange);
        return;
    }
    std::vector<std::shared_ptr<WaitObject>> objects;
    for (std::int32_t index = 0; index < count; ++index) {
        const std::optional<std::uint32_t> handle = space_.Memory().Read32(
            handles_address + (static_cast<std::uint32_t>(index) * WordBytes));
        if (!handle.has_value()) {
            Answer(thread, result::InvalidPointer);
            return;
        }
        std::shared_ptr<WaitObject> object = ResolveWaitObject(*handle);
        if (object == nullptr) {
            Answer(thread, result::InvalidHandle);
            return;
        }
        objects.push_back(std::move(object));
    }

    if (wait_all) {
        const bool all_available = std::ranges::none_of(
            objects, [&](const auto& object) { return object->ShouldWait(thread); });
        if (all_available) {
            for (const auto& object : objects) {
                object->Acquire(thread);
            }
            Answer(thread, result::Success);
            return;
        }
    } else {
        const auto ready = std::ranges::find_if(
            objects, [&](const auto& object) { return !object->ShouldWait(thread); });
        if (ready != objects.end()) {
            (*ready)->Acquire(thread);
            Answer(thread, result::Success);
            Register(thread, 1) = static_cast<std::uint32_t>(ready - objects.begin());
            return;
        }
    }

    Answer(thread, result::Timeout);
    Register(thread, 1) = NoWaitIndex;
    if (timeout != 0) {
        scheduler_.Wait(objects, wait_all, timeout, !wait_all);
    }
}

void Kernel::DuplicateHandle(Thread& thread) {
    const Handle handle = Register(thread, 1);
    std::shared_ptr<Object> object =
        handle == CurrentThreadHandle ? scheduler_.CurrentShared() : handles_.Get(handle);
    if (object == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    const std::optional<Handle> duplicate = handles_.Create(std::move(object));
    if (!duplicate.has_value()) {
        Answer(thread, result::OutOfHandles);
        return;
    }
    Answer(thread, result::Success);
    Register(thread, 1) = *duplicate;
}

void Kernel::GetSystemTick(Thread& thread) {
    Answer64(thread, 0, static_cast<std::int64_t>(scheduler_.Ticks()));
}

void Kernel::GetHandleInfo(Thread& thread) {
    const Handle handle = Register(thread, 1);
    const std::uint32_t type = Register(thread, 2);
    const bool is_process = handle == CurrentProcessHandle;
    const std::shared_ptr<Object> object = handles_.Get(handle);
    if (!is_process && object == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    std::int64_t value = 0;
    switch (type) {
    case HandleInfoProcessTicks:
        // The process was created when the clock started.
        break;
    case HandleInfoReferenceCount:
        // The local copy is not counted, as in Azahar.
        value = object == nullptr ? 0 : object.use_count() - 1;
        break;
    case HandleInfoStubbed:
    case HandleInfoStubbedToo:
        break;
    default:
        Answer(thread, result::InvalidEnumValue);
        return;
    }
    Answer(thread, result::Success);
    Answer64(thread, 1, value);
}

void Kernel::GetProcessId(Thread& thread) const {
    if (Register(thread, 1) != CurrentProcessHandle) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    Answer(thread, result::Success);
    Register(thread, 1) = config_.process_id;
}

void Kernel::GetThreadId(Thread& thread) {
    const std::shared_ptr<Thread> target = ResolveThread(Register(thread, 1));
    if (target == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    Answer(thread, result::Success);
    Register(thread, 1) = target->id;
}

void Kernel::GetResourceLimit(Thread& thread) {
    if (Register(thread, 1) != CurrentProcessHandle) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    AnswerHandle(thread, resource_limit_);
}

std::int64_t Kernel::ResourceLimitValue(std::uint32_t type) const {
    switch (type) {
    case PriorityLimit:
        return HighestApplicationPriority;
    case CommitLimit:
        return config_.regions.application;
    case ThreadLimit:
        return ApplicationThreads;
    case EventLimit:
        return ApplicationEvents;
    case MutexLimit:
        return ApplicationMutexes;
    case SemaphoreLimit:
        return ApplicationSemaphores;
    case TimerLimit:
        return ApplicationTimers;
    case SharedMemoryLimit:
        return ApplicationSharedMemories;
    case AddressArbiterLimit:
        return ApplicationAddressArbiters;
    default:
        return 0;
    }
}

std::int64_t Kernel::ResourceCurrentValue(std::uint32_t type) const {
    auto live = [&](ObjectType object_type) {
        const auto& made = created_.at(static_cast<std::size_t>(object_type));
        return static_cast<std::int64_t>(
            std::ranges::count_if(made, [](const auto& weak) { return !weak.expired(); }));
    };
    switch (type) {
    case CommitLimit:
        return space_.RegionUsed();
    case ThreadLimit:
        return static_cast<std::int64_t>(
            std::ranges::count_if(scheduler_.Threads(), [](const auto& each) {
                return each->status != ThreadStatus::Dead;
            }));
    case EventLimit:
        return live(ObjectType::Event);
    case MutexLimit:
        return live(ObjectType::Mutex);
    case SemaphoreLimit:
        return live(ObjectType::Semaphore);
    case TimerLimit:
        return live(ObjectType::Timer);
    case SharedMemoryLimit:
        return live(ObjectType::SharedMemory);
    case AddressArbiterLimit:
        return live(ObjectType::AddressArbiter);
    default:
        return 0;
    }
}

void Kernel::GetResourceLimitLimitValues(Thread& thread) {
    const std::uint32_t values = Register(thread, 0);
    const std::uint32_t names = Register(thread, 2);
    const std::uint32_t count = Register(thread, 3);
    if (handles_.Get<ResourceLimit>(Register(thread, 1), ObjectType::ResourceLimit) == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t type =
            space_.Memory().Read32(names + (index * WordBytes)).value_or(~0U);
        if (type >= ResourceTypeCount) {
            Answer(thread, result::InvalidEnumValue);
            return;
        }
        static_cast<void>(
            space_.Memory().Write64(values + (index * DoublewordBytes),
                                    static_cast<std::uint64_t>(ResourceLimitValue(type))));
    }
    Answer(thread, result::Success);
}

void Kernel::GetResourceLimitCurrentValues(Thread& thread) {
    const std::uint32_t values = Register(thread, 0);
    const std::uint32_t names = Register(thread, 2);
    const std::uint32_t count = Register(thread, 3);
    if (handles_.Get<ResourceLimit>(Register(thread, 1), ObjectType::ResourceLimit) == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t type =
            space_.Memory().Read32(names + (index * WordBytes)).value_or(~0U);
        static_cast<void>(
            space_.Memory().Write64(values + (index * DoublewordBytes),
                                    static_cast<std::uint64_t>(ResourceCurrentValue(type))));
    }
    Answer(thread, result::Success);
}

void Kernel::Break(Thread& thread) {
    break_reason_ = Register(thread, 0);
    outcome_ = Outcome::Break;
}

void Kernel::OutputDebugString(Thread& thread) {
    const std::uint32_t address = Register(thread, 0);
    const auto length = static_cast<std::int32_t>(Register(thread, 1));
    std::string text;
    for (std::int32_t index = 0; index < length; ++index) {
        const std::optional<std::uint8_t> byte =
            space_.Memory().Read8(address + static_cast<std::uint32_t>(index));
        if (!byte.has_value()) {
            break;
        }
        text.push_back(static_cast<char>(*byte));
    }
    if (debug_output_observer_) {
        debug_output_observer_(text);
    }
}

void Kernel::ConnectToPort(Thread& thread) {
    const std::uint32_t name_address = Register(thread, 1);
    std::string name;
    for (std::uint32_t index = 0; index <= PortNameMaxLength; ++index) {
        const std::optional<std::uint8_t> byte = space_.Memory().Read8(name_address + index);
        if (!byte.has_value()) {
            Answer(thread, result::InvalidPointer);
            return;
        }
        if (*byte == 0) {
            break;
        }
        name.push_back(static_cast<char>(*byte));
    }
    if (name.size() > PortNameMaxLength) {
        Answer(thread, result_codes::PortNameTooLong);
        return;
    }
    const auto port = ports_.find(name);
    if (port == ports_.end()) {
        Answer(thread, result::NotFound);
        return;
    }
    AnswerHandle(thread, std::make_shared<ClientSession>(port->second));
}

void Kernel::SendSyncRequest(Thread& thread) {
    const auto session =
        handles_.Get<ClientSession>(Register(thread, 0), ObjectType::ClientSession);
    if (session == nullptr) {
        Answer(thread, result::InvalidHandle);
        return;
    }
    IpcRequest request(*this, thread);
    // The system call succeeds once the request reaches the service; what the
    // service makes of it is in the reply.
    Answer(thread, result::Success);
    const bool implemented = session->handler->Handle(request);
    if (implemented) {
        request.Complete();
    } else {
        outcome_ = Outcome::UnimplementedCommand;
        unimplemented_service_ = std::string(session->handler->Name());
        if (request.Declined().has_value()) {
            unimplemented_service_ += " (" + *request.Declined() + ")";
        }
        unimplemented_header_ = request.Header();
    }
    if (!service_request_observers_.empty()) {
        constexpr std::uint32_t ResultWordOffset = ipc::CommandBufferOffset + WordBytes;
        const ServiceRequestRecord record{
            .tick = scheduler_.Ticks(),
            .thread_id = thread.id,
            .service = session->handler->Name(),
            .header = request.Header(),
            .result = implemented ? space_.Memory()
                                        .Read32(thread.thread_local_storage + ResultWordOffset)
                                        .value_or(0)
                                  : 0,
            .implemented = implemented};
        for (const auto& observer : service_request_observers_) {
            observer(record);
        }
    }
}

}  // namespace hexlith::domain::kernel
