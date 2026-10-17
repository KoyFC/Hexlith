#include "domain/services/power_manager.h"

#include <array>
#include <vector>

namespace hexlith::domain::services {
namespace {

// One step count per hour, a halfword each.
constexpr std::uint32_t StepCountSize = 2;

}  // namespace

std::span<const PowerManager::Command> PowerManager::Commands() {
    static constexpr std::array<Command, 9> Table = {{
        {.id = 0x0005, .name = "GetAdapterState"},
        {.id = 0x0006, .name = "GetShellState"},
        {.id = 0x0007, .name = "GetBatteryLevel"},
        {.id = 0x0008, .name = "GetBatteryChargeState"},
        {.id = 0x0009, .name = "GetPedometerState"},
        {.id = 0x000A, .name = "GetStepHistoryEntry"},
        {.id = 0x000B, .name = "GetStepHistory", .handler = &PowerManager::GetStepHistory},
        {.id = 0x000C, .name = "GetTotalStepCount"},
        {.id = 0x000F, .name = "GetStepHistoryAll"},
    }};
    return Table;
}

void PowerManager::GetStepHistory(kernel::IpcRequest& request) {
    const std::uint32_t hours = request.Pop();
    static_cast<void>(request.Pop64());  // the hour the history starts at
    const kernel::MappedBuffer buffer = request.PopMappedBuffer();

    const std::vector<std::uint8_t> steps(static_cast<std::size_t>(hours) * StepCountSize, 0);
    const bool written = request.GetKernel().Space().Memory().Load(buffer.address, steps).Ok();
    request.Begin(1, 2);
    request.Push(written ? kernel::result::Success : kernel::result::InvalidPointer);
    request.PushMappedBuffer(buffer);
}

}  // namespace hexlith::domain::services
