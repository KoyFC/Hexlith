#ifndef HEXLITH_DOMAIN_GPU_GX_COMMAND_H
#define HEXLITH_DOMAIN_GPU_GX_COMMAND_H

#include "domain/gpu/memory_transfer.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace hexlith::domain::gpu {

// GX commands, as a game writes them in gsp::Gpu's command queue and a GPU
// recording keeps them, and what gsp::Gpu tells the GPU's engines for each.
// Source: Azahar's gsp_command.h and GPU::Execute in video_core/gpu.cpp;
// 3dbrew, GSP Shared Memory, could not be reached.

using GxCommandWords = std::array<std::uint32_t, 8>;

// The low byte of the first word.
enum class GxCommandId : std::uint8_t {
    RequestDma = 0,
    ProcessCommandList = 1,
    MemoryFill = 2,
    DisplayTransfer = 3,
    TextureCopy = 4,
    FlushCacheRegions = 5,
};

GxCommandId CommandId(const GxCommandWords& words);

// A memory fill command starts a fill unit for each of its two buffers whose
// start is not zero: the start, value and end of each, then their two control
// halfwords in one word. Addresses become physical; one that is not in GPU
// memory becomes zero, which the unit refuses.
std::vector<MemoryFillConfig> MemoryFills(const GxCommandWords& words);

// A display transfer command gives the input and output addresses, their
// sizes and the flags; a texture copy the addresses, the bytes to copy, each
// side's width and gap, and the flags. Nothing for any other command.
std::optional<TransferConfig> Transfer(const GxCommandWords& words);

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_GX_COMMAND_H
