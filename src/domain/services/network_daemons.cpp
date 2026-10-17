#include "domain/services/network_daemons.h"

namespace hexlith::domain::services {
namespace {

// One bit per daemon in the masks the commands take.
constexpr std::uint32_t DaemonMask = 0xF;

bool Selects(std::uint32_t mask, std::size_t daemon) {
    return ((mask >> daemon) & 1U) != 0;
}

}  // namespace

std::span<const NetworkDaemonManager::Command> NetworkDaemonManager::Commands() {
    static constexpr std::array<Command, 10> Table = {{
        {.id = 0x0001,
         .name = "EnterExclusiveState",
         .handler = &NetworkDaemonManager::EnterExclusiveState},
        {.id = 0x0002, .name = "LeaveExclusiveState", .handler = &NetworkDaemonManager::Succeed},
        {.id = 0x0003, .name = "QueryExclusiveMode"},
        {.id = 0x0006, .name = "SuspendDaemons", .handler = &NetworkDaemonManager::SuspendDaemons},
        {.id = 0x0007, .name = "ResumeDaemons", .handler = &NetworkDaemonManager::ResumeDaemons},
        {.id = 0x0008, .name = "SuspendScheduler"},
        {.id = 0x0009, .name = "ResumeScheduler"},
        {.id = 0x000D, .name = "QueryStatus"},
        {.id = 0x0014,
         .name = "OverrideDefaultDaemons",
         .handler = &NetworkDaemonManager::OverrideDefaultDaemons},
        {.id = 0x0015, .name = "ResetDefaultDaemons"},
    }};
    return Table;
}

void NetworkDaemonManager::Succeed(kernel::IpcRequest& request) {
    exclusive_state_ = 0;
    request.ReplyWith(kernel::result::Success);
}

void NetworkDaemonManager::EnterExclusiveState(kernel::IpcRequest& request) {
    exclusive_state_ = request.Pop();
    static_cast<void>(request.PopProcessId());
    request.ReplyWith(kernel::result::Success);
}

void NetworkDaemonManager::SuspendDaemons(kernel::IpcRequest& request) {
    const std::uint32_t mask = request.Pop() & DaemonMask;
    for (std::size_t daemon = 0; daemon < DaemonCount; ++daemon) {
        if (Selects(mask, daemon)) {
            ++suspended_.at(daemon);
        }
    }
    request.ReplyWith(kernel::result::Success);
}

void NetworkDaemonManager::ResumeDaemons(kernel::IpcRequest& request) {
    const std::uint32_t mask = request.Pop() & DaemonMask;
    for (std::size_t daemon = 0; daemon < DaemonCount; ++daemon) {
        if (Selects(mask, daemon) && suspended_.at(daemon) == 0) {
            request.ReplyWith(NotSuspended);
            return;
        }
    }
    for (std::size_t daemon = 0; daemon < DaemonCount; ++daemon) {
        if (Selects(mask, daemon)) {
            --suspended_.at(daemon);
        }
    }
    request.ReplyWith(kernel::result::Success);
}

void NetworkDaemonManager::OverrideDefaultDaemons(kernel::IpcRequest& request) {
    default_daemons_ = request.Pop() & DaemonMask;
    request.ReplyWith(kernel::result::Success);
}

}  // namespace hexlith::domain::services
