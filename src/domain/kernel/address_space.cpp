#include "domain/kernel/address_space.h"

#include <algorithm>
#include <bit>
#include <optional>

namespace hexlith::domain::kernel {
namespace {

using memory::Access;

constexpr std::uint64_t AddressSpaceSize = std::uint64_t{1} << 32U;
constexpr std::size_t PageCount = AddressSpaceSize / layout::PageSize;
constexpr std::uint32_t SlotsPerPage = layout::PageSize / layout::ThreadLocalStorageSize;
constexpr std::uint8_t AllSlots = (1U << SlotsPerPage) - 1U;

std::uint32_t PageNumber(std::uint32_t address) {
    return address / layout::PageSize;
}

bool InRange(std::uint32_t address, std::uint32_t size, std::uint32_t begin, std::uint32_t end) {
    const std::uint64_t last = static_cast<std::uint64_t>(address) + size;
    return address >= begin && last <= end;
}

// Whether two ranges of the same size share a byte, the way Process::Map
// tests it, with unsigned wrap-around doing the work of two comparisons.
bool Overlap(std::uint32_t first, std::uint32_t second, std::uint32_t size) {
    return first - second < size || second - first < size;
}

}  // namespace

AddressSpace::AddressSpace(std::uint32_t region_size, std::uint32_t linear_heap_base)
    : fcram_(memory::Backing::Allocate(region_size)),
      region_(region_size),
      linear_heap_base_(linear_heap_base),
      states_(PageCount, MemoryState::Free) {}

bool AddressSpace::IsFree(std::uint32_t address, std::uint32_t size) const {
    return HasState(address, size, MemoryState::Free);
}

bool AddressSpace::HasState(std::uint32_t address, std::uint32_t size, MemoryState state) const {
    for (std::uint32_t done = 0; done < size; done += layout::PageSize) {
        if (states_.at(PageNumber(address + done)) != state) {
            return false;
        }
    }
    return true;
}

void AddressSpace::SetState(std::uint32_t address, std::uint32_t size, MemoryState state) {
    for (std::uint32_t done = 0; done < size; done += layout::PageSize) {
        states_.at(PageNumber(address + done)) = state;
    }
}

MemoryState AddressSpace::StateAt(std::uint32_t address) const {
    return states_.at(PageNumber(address));
}

ResultCode AddressSpace::MapFromRegion(std::uint32_t address,
                                       const std::vector<Extent>& extents,
                                       Access access,
                                       MemoryState state) {
    std::uint32_t at = address;
    for (const Extent& extent : extents) {
        // Memory given back earlier may be handed out again, and the kernel
        // clears it first.
        std::ranges::fill(fcram_->Bytes().subspan(extent.offset, extent.size), 0);
        if (!memory_.Map(at, extent.size, fcram_, extent.offset, access)) {
            return result::InvalidAddressState;
        }
        SetState(at, extent.size, state);
        at += extent.size;
    }
    return result::Success;
}

ResultCode AddressSpace::CommitHeap(std::uint32_t address, std::uint32_t size, Access access) {
    if (!InRange(address, size, layout::HeapBase, layout::HeapEnd)) {
        return result::InvalidAddress;
    }
    return CommitFixed(address, size, access, MemoryState::Private);
}

ResultCode AddressSpace::CommitFixed(std::uint32_t address,
                                     std::uint32_t size,
                                     Access access,
                                     MemoryState state) {
    if (!IsFree(address, size)) {
        return result::InvalidAddressState;
    }
    const auto extents = region_.TakeFromTop(size);
    if (!extents) {
        return result::OutOfHeapMemory;
    }
    return MapFromRegion(address, *extents, access, state);
}

ResultCode AddressSpace::FreeHeap(std::uint32_t address, std::uint32_t size) {
    if (!InRange(address, size, layout::HeapBase, layout::HeapEnd)) {
        return result::InvalidAddress;
    }
    if (size == 0) {
        return result::Success;
    }

    // Every page has to be heap of this region before any is given back, so a
    // refused free changes nothing.
    std::vector<std::uint32_t> offsets;
    for (std::uint32_t done = 0; done < size; done += layout::PageSize) {
        const std::optional<memory::PageTarget> target = memory_.TargetOf(address + done);
        if (!target.has_value() || target->backing != fcram_) {
            return result::InvalidAddressState;
        }
        offsets.push_back(target->offset);
    }
    for (const std::uint32_t offset : offsets) {
        region_.Give(Extent{.offset = offset, .size = layout::PageSize});
    }
    static_cast<void>(memory_.Unmap(address, size));
    SetState(address, size, MemoryState::Free);
    return result::Success;
}

AddressResult AddressSpace::CommitLinear(std::uint32_t address, std::uint32_t size, Access access) {
    std::uint32_t offset = 0;
    if (address == 0) {
        const auto lowest = region_.TakeLowest(size);
        if (!lowest) {
            return {.result = result::OutOfHeapMemory};
        }
        offset = *lowest;
        address = linear_heap_base_ + offset;
    } else {
        if (!InRange(address, size, linear_heap_base_, LinearHeapEnd())) {
            return {.result = result::InvalidAddress};
        }
        offset = address - linear_heap_base_;
        if (!region_.TakeAt(Extent{.offset = offset, .size = size})) {
            return {.result = result::InvalidAddressState};
        }
    }
    const ResultCode mapped = MapFromRegion(
        address, {Extent{.offset = offset, .size = size}}, access, MemoryState::Continuous);
    return {.result = mapped, .address = address};
}

ResultCode AddressSpace::FreeLinear(std::uint32_t address, std::uint32_t size) {
    if (!InRange(address, size, linear_heap_base_, LinearHeapEnd())) {
        return result::InvalidAddress;
    }
    if (size == 0) {
        return result::Success;
    }
    if (!HasState(address, size, MemoryState::Continuous)) {
        return result::InvalidAddressState;
    }
    static_cast<void>(memory_.Unmap(address, size));
    SetState(address, size, MemoryState::Free);
    region_.Give(Extent{.offset = address - linear_heap_base_, .size = size});
    return result::Success;
}

ResultCode AddressSpace::MapAlias(std::uint32_t target,
                                  std::uint32_t source,
                                  std::uint32_t size,
                                  Access access,
                                  bool privileged) {
    if (!privileged && !InRange(source, size, layout::HeapBase, layout::HeapEnd)) {
        return result::InvalidAddress;
    }
    if (Overlap(source, target, size)) {
        if (!privileged) {
            return result::InvalidAddressState;
        }
        if (source != target) {
            return result::InvalidAddress;
        }
        // A privileged map of a range onto itself changes only its state and
        // its access, which is how ldr:ro turns a module's own pages into code.
        if (!HasState(source, size, MemoryState::Private)) {
            return result::InvalidAddressState;
        }
        static_cast<void>(memory_.Protect(source, size, access));
        SetState(source, size, MemoryState::AliasCode);
        return result::Success;
    }
    if (!IsFree(target, size) || !HasState(source, size, MemoryState::Private)) {
        return result::InvalidAddressState;
    }

    for (std::uint32_t done = 0; done < size; done += layout::PageSize) {
        const std::optional<memory::PageTarget> page = memory_.TargetOf(source + done);
        if (page.has_value()) {
            static_cast<void>(
                memory_.Map(target + done, layout::PageSize, page->backing, page->offset, access));
        }
    }
    SetState(target, size, privileged ? MemoryState::AliasCode : MemoryState::Alias);
    static_cast<void>(memory_.Protect(source, size, privileged ? Access::None : Access::ReadWrite));
    SetState(source, size, privileged ? MemoryState::Locked : MemoryState::Aliased);
    return result::Success;
}

ResultCode AddressSpace::UnmapAlias(std::uint32_t target,
                                    std::uint32_t source,
                                    std::uint32_t size,
                                    Access access,
                                    bool privileged) {
    if (!privileged && !InRange(source, size, layout::HeapBase, layout::HeapEnd)) {
        return result::InvalidAddress;
    }
    if (Overlap(source, target, size)) {
        if (!privileged) {
            return result::InvalidAddressState;
        }
        if (source != target) {
            return result::InvalidAddress;
        }
        if (!HasState(source, size, MemoryState::AliasCode)) {
            return result::InvalidAddressState;
        }
        static_cast<void>(memory_.Protect(source, size, access));
        SetState(source, size, MemoryState::Private);
        return result::Success;
    }

    const MemoryState source_state = privileged ? MemoryState::Locked : MemoryState::Aliased;
    const MemoryState target_state = privileged ? MemoryState::AliasCode : MemoryState::Alias;
    if (!HasState(target, size, target_state) || !HasState(source, size, source_state)) {
        return result::InvalidAddressState;
    }
    static_cast<void>(memory_.Unmap(target, size));
    SetState(target, size, MemoryState::Free);
    static_cast<void>(memory_.Protect(source, size, access));
    SetState(source, size, MemoryState::Private);
    return result::Success;
}

ResultCode AddressSpace::Protect(std::uint32_t address, std::uint32_t size, Access access) {
    if (!memory_.Protect(address, size, access)) {
        return result::InvalidAddressState;
    }
    return result::Success;
}

ResultCode AddressSpace::MapBacking(std::uint32_t address,
                                    std::uint32_t size,
                                    const std::shared_ptr<memory::Backing>& backing,
                                    std::uint32_t offset,
                                    Access access,
                                    MemoryState state) {
    if (!IsFree(address, size)) {
        return result::InvalidAddressState;
    }
    if (!memory_.Map(address, size, backing, offset, access)) {
        return result::InvalidAddress;
    }
    SetState(address, size, state);
    return result::Success;
}

ResultCode AddressSpace::UnmapBacking(std::uint32_t address, std::uint32_t size) {
    if (!memory_.Unmap(address, size)) {
        return result::InvalidAddressState;
    }
    SetState(address, size, MemoryState::Free);
    return result::Success;
}

std::uint32_t AddressSpace::AllocateThreadLocalStorage() {
    auto page =
        std::ranges::find_if(storage_slots_, [](std::uint8_t slots) { return slots != AllSlots; });
    if (page == storage_slots_.end()) {
        const auto index = static_cast<std::uint32_t>(storage_slots_.size());
        // The console takes these pages from the BASE region, which the
        // application cannot otherwise reach, so they get a backing of their own.
        static_cast<void>(MapBacking(layout::ThreadLocalStorageBase + (index * layout::PageSize),
                                     layout::PageSize,
                                     memory::Backing::Allocate(layout::PageSize),
                                     0,
                                     Access::ReadWrite,
                                     MemoryState::Locked));
        storage_slots_.push_back(0);
        page = std::prev(storage_slots_.end());
    }
    const auto slot = static_cast<std::uint32_t>(std::countr_one(*page));
    *page = static_cast<std::uint8_t>(*page | (1U << slot));

    const auto index = static_cast<std::uint32_t>(page - storage_slots_.begin());
    const std::uint32_t address = layout::ThreadLocalStorageBase + (index * layout::PageSize) +
                                  (slot * layout::ThreadLocalStorageSize);
    const std::vector<std::uint8_t> zeros(layout::ThreadLocalStorageSize, 0);
    static_cast<void>(memory_.Load(address, zeros));
    return address;
}

void AddressSpace::FreeThreadLocalStorage(std::uint32_t address) {
    const std::uint32_t offset = address - layout::ThreadLocalStorageBase;
    const std::uint32_t index = offset / layout::PageSize;
    const std::uint32_t slot = (offset % layout::PageSize) / layout::ThreadLocalStorageSize;
    if (index < storage_slots_.size()) {
        storage_slots_.at(index) =
            static_cast<std::uint8_t>(storage_slots_.at(index) & ~(1U << slot));
    }
}

MemoryInfo AddressSpace::Query(std::uint32_t address) const {
    const std::uint32_t page = PageNumber(address);
    const MemoryState state = states_.at(page);
    const Access access = memory_.AccessAt(address);
    auto same = [&](std::uint32_t other) {
        return states_.at(other) == state && memory_.AccessAt(other * layout::PageSize) == access;
    };

    std::uint32_t first = page;
    while (first > 0 && same(first - 1)) {
        --first;
    }
    std::uint32_t last = page;
    while (last + 1 < PageCount && same(last + 1)) {
        ++last;
    }
    return MemoryInfo{.base = first * layout::PageSize,
                      .size = (last - first + 1) * layout::PageSize,
                      .access = access,
                      .state = state};
}

}  // namespace hexlith::domain::kernel
