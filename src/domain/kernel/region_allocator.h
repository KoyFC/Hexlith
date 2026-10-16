#ifndef HEXLITH_DOMAIN_KERNEL_REGION_ALLOCATOR_H
#define HEXLITH_DOMAIN_KERNEL_REGION_ALLOCATOR_H

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace hexlith::domain::kernel {

// A stretch of physical memory, as an offset into a region and a size.
struct Extent {
    std::uint32_t offset = 0;
    std::uint32_t size = 0;

    friend bool operator==(const Extent&, const Extent&) = default;
};

// Which parts of a memory region are free, and the two ways the kernel takes
// from it.
//
// The kernel gives the application one region of FCRAM and allocates two kinds
// of memory out of it differently. Ordinary heap, code and stack come from the
// top and need not be contiguous. Linear memory, which the GPU and the DSP read
// by physical address, comes from the bottom in one contiguous piece. Which
// piece the game gets decides the linear addresses it sees, so the policy is
// kept exactly: first fit from the bottom for linear, from the top for the rest.
// Source: MemoryRegionInfo::HeapAllocate and LinearAllocate in Azahar,
// src/core/hle/kernel/memory.cpp.
class RegionAllocator {
public:
    explicit RegionAllocator(std::uint32_t size);

    // Takes size bytes from the highest free memory down, in as many pieces as
    // that needs, highest first. Nothing is taken unless all of it can be.
    [[nodiscard]] std::optional<std::vector<Extent>> TakeFromTop(std::uint32_t size);

    // Takes the lowest free piece that holds size bytes contiguously.
    [[nodiscard]] std::optional<std::uint32_t> TakeLowest(std::uint32_t size);

    // Takes a given piece, if all of it is free.
    [[nodiscard]] bool TakeAt(Extent extent);

    // Gives a piece back. It must not be free already.
    void Give(Extent extent);

    [[nodiscard]] std::uint32_t Size() const {
        return size_;
    }

    [[nodiscard]] std::uint32_t Used() const {
        return used_;
    }

private:
    void Remove(Extent extent);

    std::uint32_t size_ = 0;
    std::uint32_t used_ = 0;

    // Free pieces by offset, each mapped to its end. Neighbours are always
    // merged, so no two entries touch.
    std::map<std::uint32_t, std::uint32_t> free_;
};

}  // namespace hexlith::domain::kernel

#endif  // HEXLITH_DOMAIN_KERNEL_REGION_ALLOCATOR_H
