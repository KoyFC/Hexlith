#include "domain/gpu/gx_command.h"

#include <optional>

namespace hexlith::domain::gpu {
namespace {

constexpr std::uint32_t CommandIdMask = 0xFF;
constexpr unsigned HalfwordBits = 16;
constexpr std::uint32_t HalfwordMask = 0xFFFF;

// The fill and transfer address registers hold an address in 16-byte units.
// Source: GBATEK, Memfill and Memcopy; Azahar keeps 8-byte units.
constexpr std::uint32_t AddressUnitMask = 0xF;

std::uint32_t Physical(std::uint32_t address) {
    return GpuMemory::PhysicalFromVirtual(address).value_or(0) & ~AddressUnitMask;
}

// The end of a fill may be the end of the memory it is in, which belongs to
// no address of it. Source: Azahar's GPU::VirtualToPhysicalAddress, whose
// ranges include their ends.
std::uint32_t PhysicalEnd(std::uint32_t address) {
    if (address == 0) {
        return 0;
    }
    const std::optional<std::uint32_t> last = GpuMemory::PhysicalFromVirtual(address - 1);
    return last.has_value() ? (*last + 1) & ~AddressUnitMask : 0;
}

}  // namespace

GxCommandId CommandId(const GxCommandWords& words) {
    return static_cast<GxCommandId>(words[0] & CommandIdMask);
}

std::vector<MemoryFillConfig> MemoryFills(const GxCommandWords& words) {
    // Words 1 to 3 for the first buffer, 4 to 6 for the second, 7 the
    // controls.
    constexpr std::size_t Second = 3;
    std::vector<MemoryFillConfig> fills;
    if (CommandId(words) != GxCommandId::MemoryFill) {
        return fills;
    }
    for (std::size_t unit = 0; unit < 2; ++unit) {
        const std::size_t at = 1 + (unit * Second);
        if (words.at(at) == 0) {
            continue;
        }
        fills.push_back(
            MemoryFillConfig{.start = Physical(words.at(at)),
                             .end = PhysicalEnd(words.at(at + 2)),
                             .value = words.at(at + 1),
                             .control = (words[7] >> (HalfwordBits * unit)) & HalfwordMask});
    }
    return fills;
}

std::optional<TransferConfig> Transfer(const GxCommandWords& words) {
    switch (CommandId(words)) {
    case GxCommandId::DisplayTransfer:
        return TransferConfig{.input = Physical(words[1]),
                              .output = Physical(words[2]),
                              .input_size = words[3],
                              .output_size = words[4],
                              .flags = words[5],
                              .texture_size = 0,
                              .texture_input = 0,
                              .texture_output = 0};
    case GxCommandId::TextureCopy:
        return TransferConfig{.input = Physical(words[1]),
                              .output = Physical(words[2]),
                              .input_size = 0,
                              .output_size = 0,
                              .flags = words[6],
                              .texture_size = words[3],
                              .texture_input = words[4],
                              .texture_output = words[5]};
    default:
        return std::nullopt;
    }
}

}  // namespace hexlith::domain::gpu
