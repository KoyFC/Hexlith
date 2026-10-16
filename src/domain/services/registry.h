#ifndef HEXLITH_DOMAIN_SERVICES_REGISTRY_H
#define HEXLITH_DOMAIN_SERVICES_REGISTRY_H

#include "domain/kernel/ipc.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::domain::services {

// A service the title may reach and that nothing here implements yet. Every
// command sent to it stops the run with the service's name and the command's
// header, so the first command the game needs from it is found by running the
// game rather than guessed.
class UnimplementedService final : public kernel::SessionHandler {
public:
    explicit UnimplementedService(std::string name) : name_(std::move(name)) {}

    [[nodiscard]] std::string_view Name() const override {
        return name_;
    }

    bool Handle(kernel::IpcRequest& /*request*/) override {
        return false;
    }

private:
    std::string name_;
};

// The services srv: can hand out, by name. Only the ones the title's extended
// header lists are allowed, which is the console's rule: a process may not open
// a service its access control list does not name. Source: 3dbrew, SRV,
// GetServiceHandle.
class ServiceRegistry {
public:
    // Every service the list names, each unimplemented until replaced.
    explicit ServiceRegistry(const std::vector<std::string>& allowed);

    // Replaces a service with an implementation. A service the title is not
    // allowed is not added, since the game could never reach it.
    void Provide(std::shared_ptr<kernel::SessionHandler> service);

    [[nodiscard]] bool IsAllowed(std::string_view name) const;

    // The service, or null when the title may not reach it.
    [[nodiscard]] std::shared_ptr<kernel::SessionHandler> Find(std::string_view name) const;

    // Every service the title may reach, in name order.
    [[nodiscard]] std::vector<std::string> Names() const;

private:
    std::map<std::string, std::shared_ptr<kernel::SessionHandler>, std::less<>> services_;
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_REGISTRY_H
