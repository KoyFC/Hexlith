#ifndef HEXLITH_DOMAIN_SERVICES_NETWORK_DAEMONS_H
#define HEXLITH_DOMAIN_SERVICES_NETWORK_DAEMONS_H

#include "domain/kernel/kernel.h"
#include "domain/services/command_table.h"

#include <array>
#include <cstdint>
#include <span>

namespace hexlith::domain::services {

// ndm:u, the network daemon manager. An application tells it which of the
// four background daemons (CEC, BOSS, friends, the scheduler) may run while it
// does, and suspends and resumes them. Nothing here runs a daemon, so what is
// kept is the bookkeeping the answers depend on: which daemons are allowed and
// how often each has been suspended. Commands: 3dbrew, NDM Services; answers:
// NDM_U in Azahar, src/core/hle/service/ndm/ndm_u.cpp.
class NetworkDaemonManager final : public CommandTable<NetworkDaemonManager> {
public:
    [[nodiscard]] std::string_view Name() const override {
        return "ndm:u";
    }

    static std::span<const Command> Commands();

    static constexpr std::size_t DaemonCount = 4;

    // Answered when a daemon is resumed that was never suspended.
    static constexpr kernel::ResultCode NotSuspended{0xC8A0680D};

    [[nodiscard]] std::uint32_t DefaultDaemons() const {
        return default_daemons_;
    }

    [[nodiscard]] std::uint32_t SuspendCount(std::size_t daemon) const {
        return suspended_.at(daemon);
    }

private:
    void Succeed(kernel::IpcRequest& request);
    void EnterExclusiveState(kernel::IpcRequest& request);
    void SuspendDaemons(kernel::IpcRequest& request);
    void ResumeDaemons(kernel::IpcRequest& request);
    void OverrideDefaultDaemons(kernel::IpcRequest& request);

    std::uint32_t default_daemons_ = 0;
    std::uint32_t exclusive_state_ = 0;
    std::array<std::uint32_t, DaemonCount> suspended_{};
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_NETWORK_DAEMONS_H
