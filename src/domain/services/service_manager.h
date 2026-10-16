#ifndef HEXLITH_DOMAIN_SERVICES_SERVICE_MANAGER_H
#define HEXLITH_DOMAIN_SERVICES_SERVICE_MANAGER_H

#include "domain/kernel/kernel.h"
#include "domain/services/command_table.h"
#include "domain/services/registry.h"

#include <memory>
#include <set>
#include <span>

namespace hexlith::domain::services {

// srv:, the service manager's port, which the game connects to with
// ConnectToPort and through which it opens every other service. Commands and
// their meaning: 3dbrew, SRV; the answers are the ones SRV in Azahar gives,
// src/core/hle/service/sm/srv.cpp.
class ServiceManager final : public CommandTable<ServiceManager> {
public:
    explicit ServiceManager(const ServiceRegistry& registry);

    [[nodiscard]] std::string_view Name() const override {
        return "srv:";
    }

    static std::span<const Command> Commands();

    // Whether the game has registered itself as a client, and the
    // notifications it has subscribed to.
    [[nodiscard]] bool IsRegistered() const {
        return registered_;
    }

    [[nodiscard]] const std::set<std::uint32_t>& Subscriptions() const {
        return subscriptions_;
    }

    // Answered when a name is longer than eight characters, and when the title
    // may not open the service.
    static constexpr kernel::ResultCode InvalidNameSize{0xD9006405};
    static constexpr kernel::ResultCode AccessDenied{0xD8E06406};

private:
    void RegisterClient(kernel::IpcRequest& request);
    void EnableNotification(kernel::IpcRequest& request);
    void GetServiceHandle(kernel::IpcRequest& request);
    void Subscribe(kernel::IpcRequest& request);
    void Unsubscribe(kernel::IpcRequest& request);

    // Never null; the registry outlives the port.
    const ServiceRegistry* registry_;
    bool registered_ = false;
    std::set<std::uint32_t> subscriptions_;
    std::shared_ptr<kernel::Semaphore> notifications_;
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_SERVICE_MANAGER_H
