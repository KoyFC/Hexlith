#ifndef HEXLITH_DOMAIN_GPU_MEMORY_TRANSFER_H
#define HEXLITH_DOMAIN_GPU_MEMORY_TRANSFER_H

#include "domain/gpu/gpu_memory.h"

#include <cstdint>

namespace hexlith::domain::gpu {

// The GPU's two engines outside the PICA: the memory fill units, which clear
// buffers, and the transfer engine, which turns a tiled colour buffer into
// the linear framebuffer a screen shows, converting and scaling on the way,
// or copies raw bytes. Source: GBATEK, 3DS GPU External Registers - Memfill
// and Memcopy, for the registers; Azahar's sw_blitter.cpp for what GBATEK
// leaves open. docs/gpu-transfers.md has the details and where the two part.

// GPU_MEMFILL_*: a unit's range of physical memory, the end not included, its
// value and its control register.
struct MemoryFillConfig {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    std::uint32_t value = 0;
    std::uint32_t control = 0;
};

// Fills the range with the value's low 16, 24 or 32 bits, as the control
// register's fill width says. False, and nothing filled, when the range is
// not in GPU memory or is empty.
bool RunMemoryFill(GpuMemory& memory, const MemoryFillConfig& config);

// GPU_MEMCOPY_*: the transfer engine's registers, as gsp::Gpu fills them from
// a display transfer or texture copy command.
struct TransferConfig {
    // Physical addresses.
    std::uint32_t input = 0;
    std::uint32_t output = 0;
    // GPU_MEMCOPY_DISPLAY_GAP and DISPLAY_SIZE, which the GX command calls
    // the input and output buffer sizes: a width in the low halfword and a
    // height in the high one. The output's width is the number of pixels
    // taken from each of the input's rows.
    std::uint32_t input_size = 0;
    std::uint32_t output_size = 0;
    // GPU_MEMCOPY_FLAGS.
    std::uint32_t flags = 0;
    // GPU_MEMCOPY_TEX_SIZE, TEX_SRC_WIDTH and TEX_DST_WIDTH, for a texture
    // copy: the bytes to copy, then each side's width and gap in 16-byte
    // units, the gap in the high halfword.
    std::uint32_t texture_size = 0;
    std::uint32_t texture_input = 0;
    std::uint32_t texture_output = 0;
};

enum class TransferOutcome : std::uint8_t {
    Done,
    // An address outside GPU memory or a size of zero: the hardware is said
    // to hang, and nothing is written.
    Refused,
    // A downscale of linear input, or the downscale value 3, which Azahar
    // does not implement and GBATEK calls invalid. Nothing is written.
    Unsupported,
};

// A display transfer, or a texture copy when GPU_MEMCOPY_FLAGS bit 3 is set.
TransferOutcome RunTransfer(GpuMemory& memory, const TransferConfig& config);

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_MEMORY_TRANSFER_H
