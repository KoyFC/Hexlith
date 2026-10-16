#ifndef HEXLITH_DOMAIN_KERNEL_ADDRESS_SPACE_H
#define HEXLITH_DOMAIN_KERNEL_ADDRESS_SPACE_H

#include "domain/kernel/region_allocator.h"
#include "domain/kernel/result_code.h"
#include "domain/memory/process_memory.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace hexlith::domain::kernel {

// Where things are in an application's address space. Sources: 3dbrew, Memory
// layout, and the constants of Azahar's src/core/memory.h. docs/horizon-memory.md
// has the whole map.
namespace layout {

constexpr std::uint32_t PageSize = memory::ProcessMemory::PageSize;

// Where an executable's image starts, the bottom of what a process may use.
constexpr std::uint32_t ImageBase = 0x00100000;

// The ordinary heap, which ControlMemory commits at addresses the game picks.
// The main thread's stack sits at its very top.
constexpr std::uint32_t HeapBase = 0x08000000;
constexpr std::uint32_t HeapEnd = 0x10000000;

// Where MapMemoryBlock puts a shared block when the caller leaves the address
// to the kernel.
constexpr std::uint32_t SharedMemoryBase = 0x10000000;
constexpr std::uint32_t SharedMemoryEnd = 0x14000000;

// The linear heap, which maps the application's FCRAM region one to one, so
// that a linear address and a physical address differ by a constant. A title
// built for a kernel older than 2.44 gets it at 0x14000000, a newer one at
// 0x30000000. Source: Process::GetLinearHeapAreaAddress in Azahar,
// src/core/hle/kernel/process.cpp, which compares the version the extended
// header declares with 0x22C.
constexpr std::uint32_t OldLinearHeapBase = 0x14000000;
constexpr std::uint32_t NewLinearHeapBase = 0x30000000;
constexpr std::uint16_t NewLinearHeapKernelVersion = 0x022C;

// Read-only pages every process sees: the kernel's configuration, and the page
// it shares with the system.
constexpr std::uint32_t ConfigMemory = 0x1FF80000;
constexpr std::uint32_t SharedPage = 0x1FF81000;

// Thread-local storage, 0x200 bytes a thread, eight to a page, pages from here
// up. The first 0x80 bytes are the thread's own; the IPC command buffer starts
// at 0x80.
constexpr std::uint32_t ThreadLocalStorageBase = 0x1FF82000;
constexpr std::uint32_t ThreadLocalStorageSize = 0x200;

[[nodiscard]] constexpr std::uint32_t LinearHeapBaseFor(std::uint16_t kernel_version) {
    return kernel_version < NewLinearHeapKernelVersion ? OldLinearHeapBase : NewLinearHeapBase;
}

}  // namespace layout

// What a page is for, as QueryMemory reports it. Source: 3dbrew, SVC,
// MemoryState; the values are the kernel's.
enum class MemoryState : std::uint8_t {
    Free = 0,
    Reserved = 1,
    Io = 2,
    Static = 3,
    Code = 4,
    Private = 5,
    Shared = 6,
    Continuous = 7,
    Aliased = 8,
    Alias = 9,
    AliasCode = 10,
    Locked = 11,
};

// One run of pages with the same state and access, the answer to QueryMemory.
struct MemoryInfo {
    std::uint32_t base = 0;
    std::uint32_t size = 0;
    memory::Access access = memory::Access::None;
    MemoryState state = MemoryState::Free;
};

// An address the kernel chose, or why it could not.
struct AddressResult {
    ResultCode result;
    std::uint32_t address = 0;
};

// An application's address space as Horizon manages it: which ranges hold
// what, where their physical memory comes from, and the rules for changing them.
//
// The physical side is the application's region of FCRAM, one backing that the
// heap, the linear heap, the code and the stack are all carved out of, so that
// a linear address names the same byte as the heap page it may alias. The rules
// are the kernel's, as Azahar implements them in src/core/hle/kernel/process.cpp,
// and the result codes are the ones it returns.
//
// Range and alignment checks that the system call itself makes, before it
// reaches the process, are the caller's. These operations take addresses and
// sizes that are already page aligned.
class AddressSpace {
public:
    AddressSpace(std::uint32_t region_size, std::uint32_t linear_heap_base);

    [[nodiscard]] memory::ProcessMemory& Memory() {
        return memory_;
    }

    [[nodiscard]] const memory::ProcessMemory& Memory() const {
        return memory_;
    }

    [[nodiscard]] std::uint32_t LinearHeapBase() const {
        return linear_heap_base_;
    }

    [[nodiscard]] std::uint32_t LinearHeapEnd() const {
        return linear_heap_base_ + region_.Size();
    }

    // Commits heap memory, zero filled, at an address the caller chose inside
    // the heap range. HeapAllocate in Azahar.
    ResultCode CommitHeap(std::uint32_t address, std::uint32_t size, memory::Access access);

    // The same, without the heap range check, for what the kernel places
    // itself: the executable's segments and the main thread's stack.
    ResultCode CommitFixed(std::uint32_t address,
                           std::uint32_t size,
                           memory::Access access,
                           MemoryState state);

    ResultCode FreeHeap(std::uint32_t address, std::uint32_t size);

    // Commits linear memory. With an address of zero the kernel picks the
    // lowest free piece of the region, which is what games do. LinearAllocate
    // in Azahar.
    AddressResult CommitLinear(std::uint32_t address, std::uint32_t size, memory::Access access);

    ResultCode FreeLinear(std::uint32_t address, std::uint32_t size);

    // Maps the physical memory under source at target as well, the MAP
    // operation of ControlMemory. A privileged mapping, which is what ldr:ro
    // makes for a module, leaves the source inaccessible and marks the target as
    // code; an ordinary one leaves both usable. Process::Map in Azahar.
    ResultCode MapAlias(std::uint32_t target,
                        std::uint32_t source,
                        std::uint32_t size,
                        memory::Access access,
                        bool privileged);

    // Undoes MapAlias, giving the source the access asked for.
    ResultCode UnmapAlias(std::uint32_t target,
                          std::uint32_t source,
                          std::uint32_t size,
                          memory::Access access,
                          bool privileged);

    // Moves a range from one state to another with a new access, if every page
    // of it is in the expected state. VMManager::ChangeMemoryState in Azahar,
    // which a heap-backed memory block uses to lock the heap it shares.
    ResultCode ChangeState(std::uint32_t address,
                           std::uint32_t size,
                           MemoryState expected,
                           MemoryState state,
                           memory::Access access);

    // Changes the access of a mapped range, the PROTECT operation.
    ResultCode Protect(std::uint32_t address, std::uint32_t size, memory::Access access);

    // Maps a backing that is not the region's, such as a shared memory block or
    // the configuration page, at a free range.
    ResultCode MapBacking(std::uint32_t address,
                          std::uint32_t size,
                          const std::shared_ptr<memory::Backing>& backing,
                          std::uint32_t offset,
                          memory::Access access,
                          MemoryState state);

    ResultCode UnmapBacking(std::uint32_t address, std::uint32_t size);

    // Gives a thread its 0x200 bytes of local storage, zero filled, mapping a
    // new page when the existing ones are full. Process::
    // AllocateThreadLocalStorage in Azahar.
    std::uint32_t AllocateThreadLocalStorage();
    void FreeThreadLocalStorage(std::uint32_t address);

    [[nodiscard]] MemoryInfo Query(std::uint32_t address) const;
    [[nodiscard]] MemoryState StateAt(std::uint32_t address) const;

    // How much of the application region is in use, which GetResourceLimit-
    // CurrentValues reports as the committed memory.
    [[nodiscard]] std::uint32_t RegionUsed() const {
        return region_.Used();
    }

private:
    [[nodiscard]] bool IsFree(std::uint32_t address, std::uint32_t size) const;
    [[nodiscard]] bool HasState(std::uint32_t address, std::uint32_t size, MemoryState state) const;
    void SetState(std::uint32_t address, std::uint32_t size, MemoryState state);
    ResultCode MapFromRegion(std::uint32_t address,
                             const std::vector<Extent>& extents,
                             memory::Access access,
                             MemoryState state);

    memory::ProcessMemory memory_;
    std::shared_ptr<memory::Backing> fcram_;
    RegionAllocator region_;
    std::uint32_t linear_heap_base_ = 0;

    // The state of every page, indexed by page number.
    std::vector<MemoryState> states_;

    // One entry per thread-local storage page, a bit per slot in use.
    std::vector<std::uint8_t> storage_slots_;
};

}  // namespace hexlith::domain::kernel

#endif  // HEXLITH_DOMAIN_KERNEL_ADDRESS_SPACE_H
