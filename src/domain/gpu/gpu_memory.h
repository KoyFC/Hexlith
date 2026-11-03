#ifndef HEXLITH_DOMAIN_GPU_GPU_MEMORY_H
#define HEXLITH_DOMAIN_GPU_GPU_MEMORY_H

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hexlith::domain::gpu {

// A run of bytes at a physical address.
struct MemoryRange {
    std::uint32_t start = 0;
    std::uint32_t size = 0;

    bool operator==(const MemoryRange&) const = default;
};

// The memory the GPU reads and writes, by the physical addresses its
// registers hold: VRAM, and FCRAM, where a title's linear heap lies. Both are
// flat, so that a vertex, a texel or a pixel is an offset from a base and
// nothing more.
//
// Physical addresses and sizes: VRAM_PADDR, VRAM_SIZE, FCRAM_PADDR and
// FCRAM_SIZE in Azahar's src/core/memory.h; 3dbrew, Memory layout. FCRAM is
// the Old 3DS's 128 MiB, which is all the reference title runs in.
//
// It holds the bytes itself, for a replay, or stands over bytes a running
// title holds, so that what the GPU draws is drawn in the game's own memory.
class GpuMemory {
public:
    static constexpr std::uint32_t VramBase = 0x18000000;
    static constexpr std::uint32_t VramSize = 0x00600000;
    static constexpr std::uint32_t FcramBase = 0x20000000;
    static constexpr std::uint32_t FcramSize = 0x08000000;

    // Where a game sees each: VRAM at 0x1F000000, and FCRAM at the base of
    // either linear heap, which maps it one to one. Source: VRAM_VADDR and
    // the linear heap addresses in Azahar's memory.h; docs/horizon-memory.md.
    static constexpr std::uint32_t VramVirtualBase = 0x1F000000;
    static constexpr std::uint32_t OldLinearHeapBase = 0x14000000;
    static constexpr std::uint32_t NewLinearHeapBase = 0x30000000;

    // Memory of its own, zeroed.
    GpuMemory();

    // Memory someone else holds, which has to outlive this. FCRAM may be
    // shorter than the console's, from its start: the application's region,
    // where the linear heap lies.
    GpuMemory(std::span<std::uint8_t> vram, std::span<std::uint8_t> fcram);

    GpuMemory(const GpuMemory&) = delete;
    GpuMemory& operator=(const GpuMemory&) = delete;
    GpuMemory(GpuMemory&&) = default;
    GpuMemory& operator=(GpuMemory&&) = default;
    ~GpuMemory() = default;

    // The physical address behind a game's virtual address in VRAM or a linear
    // heap, if it is in one.
    [[nodiscard]] static std::optional<std::uint32_t> PhysicalFromVirtual(std::uint32_t address);

    // The bytes at a physical address, if all of them are in one of the two
    // memories. Empty otherwise.
    [[nodiscard]] std::span<const std::uint8_t> Bytes(std::uint32_t address,
                                                      std::uint32_t size) const;
    [[nodiscard]] std::span<std::uint8_t> Bytes(std::uint32_t address, std::uint32_t size);

    // Writes bytes at a physical address. False, and nothing written, if they
    // do not all fall in one of the two memories.
    bool Write(std::uint32_t address, std::span<const std::uint8_t> bytes);

private:
    // What the memory holds when it is its own. A vector's storage stays put
    // when the vector is moved, so the views below survive a move.
    std::vector<std::uint8_t> owned_vram_;
    std::vector<std::uint8_t> owned_fcram_;
    std::span<std::uint8_t> vram_;
    std::span<std::uint8_t> fcram_;
};

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_GPU_MEMORY_H
