#include "domain/gpu/gpu_memory.h"

#include <algorithm>

namespace hexlith::domain::gpu {
namespace {

bool Within(std::uint32_t address, std::uint32_t size, std::uint32_t base, std::uint32_t length) {
    return address >= base && size <= length && address - base <= length - size;
}

}  // namespace

GpuMemory::GpuMemory()
    : owned_vram_(VramSize), owned_fcram_(FcramSize), vram_(owned_vram_), fcram_(owned_fcram_) {}

GpuMemory::GpuMemory(std::span<std::uint8_t> vram, std::span<std::uint8_t> fcram)
    : vram_(vram.first(std::min<std::size_t>(vram.size(), VramSize))),
      fcram_(fcram.first(std::min<std::size_t>(fcram.size(), FcramSize))) {}

std::optional<std::uint32_t> GpuMemory::PhysicalFromVirtual(std::uint32_t address) {
    if (Within(address, 1, VramVirtualBase, VramSize)) {
        return VramBase + (address - VramVirtualBase);
    }
    for (const std::uint32_t linear : {OldLinearHeapBase, NewLinearHeapBase}) {
        if (Within(address, 1, linear, FcramSize)) {
            return FcramBase + (address - linear);
        }
    }
    return std::nullopt;
}

std::span<const std::uint8_t> GpuMemory::Bytes(std::uint32_t address, std::uint32_t size) const {
    if (Within(address, size, VramBase, static_cast<std::uint32_t>(vram_.size()))) {
        return vram_.subspan(address - VramBase, size);
    }
    if (Within(address, size, FcramBase, static_cast<std::uint32_t>(fcram_.size()))) {
        return fcram_.subspan(address - FcramBase, size);
    }
    return {};
}

std::span<std::uint8_t> GpuMemory::Bytes(std::uint32_t address, std::uint32_t size) {
    if (Within(address, size, VramBase, static_cast<std::uint32_t>(vram_.size()))) {
        return vram_.subspan(address - VramBase, size);
    }
    if (Within(address, size, FcramBase, static_cast<std::uint32_t>(fcram_.size()))) {
        return fcram_.subspan(address - FcramBase, size);
    }
    return {};
}

bool GpuMemory::Write(std::uint32_t address, std::span<const std::uint8_t> bytes) {
    const std::span<std::uint8_t> target = Bytes(address, static_cast<std::uint32_t>(bytes.size()));
    if (target.size() != bytes.size()) {
        return false;
    }
    std::ranges::copy(bytes, target.begin());
    return true;
}

}  // namespace hexlith::domain::gpu
