#include "domain/kernel/region_allocator.h"

#include <iterator>
#include <stdexcept>

namespace hexlith::domain::kernel {

RegionAllocator::RegionAllocator(std::uint32_t size) : size_(size) {
    if (size != 0) {
        free_.emplace(0, size);
    }
}

void RegionAllocator::Remove(Extent extent) {
    const std::uint32_t end = extent.offset + extent.size;
    auto containing = std::prev(free_.upper_bound(extent.offset));
    const std::uint32_t piece_start = containing->first;
    const std::uint32_t piece_end = containing->second;
    free_.erase(containing);
    if (piece_start < extent.offset) {
        free_.emplace(piece_start, extent.offset);
    }
    if (end < piece_end) {
        free_.emplace(end, piece_end);
    }
    used_ += extent.size;
}

std::optional<std::vector<Extent>> RegionAllocator::TakeFromTop(std::uint32_t size) {
    std::vector<Extent> taken;
    std::uint32_t rest = size;
    for (auto piece = free_.rbegin(); piece != free_.rend() && rest != 0; ++piece) {
        const std::uint32_t available = piece->second - piece->first;
        const std::uint32_t part = available < rest ? available : rest;
        taken.push_back(Extent{.offset = piece->second - part, .size = part});
        rest -= part;
    }
    if (rest != 0) {
        return std::nullopt;
    }
    for (const Extent& extent : taken) {
        Remove(extent);
    }
    return taken;
}

std::optional<std::uint32_t> RegionAllocator::TakeLowest(std::uint32_t size) {
    for (const auto& [start, end] : free_) {
        if (end - start >= size) {
            const std::uint32_t offset = start;
            Remove(Extent{.offset = offset, .size = size});
            return offset;
        }
    }
    return std::nullopt;
}

bool RegionAllocator::TakeAt(Extent extent) {
    const auto after = free_.upper_bound(extent.offset);
    if (after == free_.begin()) {
        return false;
    }
    const auto containing = std::prev(after);
    if (static_cast<std::uint64_t>(extent.offset) + extent.size > containing->second) {
        return false;
    }
    Remove(extent);
    return true;
}

void RegionAllocator::Give(Extent extent) {
    std::uint32_t start = extent.offset;
    std::uint32_t end = extent.offset + extent.size;

    auto after = free_.lower_bound(start);
    if (after != free_.end() && after->first < end) {
        throw std::logic_error("RegionAllocator::Give: the piece is partly free already");
    }
    if (after != free_.begin()) {
        const auto before = std::prev(after);
        if (before->second > start) {
            throw std::logic_error("RegionAllocator::Give: the piece is partly free already");
        }
        if (before->second == start) {
            start = before->first;
            free_.erase(before);
        }
    }
    if (after != free_.end() && after->first == end) {
        end = after->second;
        free_.erase(after);
    }
    free_.emplace(start, end);
    used_ -= extent.size;
}

}  // namespace hexlith::domain::kernel
