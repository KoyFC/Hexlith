#include "domain/services/registry.h"

namespace hexlith::domain::services {

ServiceRegistry::ServiceRegistry(const std::vector<std::string>& allowed) {
    for (const std::string& name : allowed) {
        services_.emplace(name, std::make_shared<UnimplementedService>(name));
    }
}

void ServiceRegistry::Provide(std::shared_ptr<kernel::SessionHandler> service) {
    const auto found = services_.find(service->Name());
    if (found != services_.end()) {
        found->second = std::move(service);
    }
}

bool ServiceRegistry::IsAllowed(std::string_view name) const {
    return services_.contains(name);
}

std::shared_ptr<kernel::SessionHandler> ServiceRegistry::Find(std::string_view name) const {
    const auto found = services_.find(name);
    return found == services_.end() ? nullptr : found->second;
}

std::vector<std::string> ServiceRegistry::Names() const {
    std::vector<std::string> names;
    names.reserve(services_.size());
    for (const auto& [name, service] : services_) {
        names.push_back(name);
    }
    return names;
}

}  // namespace hexlith::domain::services
