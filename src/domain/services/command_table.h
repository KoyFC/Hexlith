#ifndef HEXLITH_DOMAIN_SERVICES_COMMAND_TABLE_H
#define HEXLITH_DOMAIN_SERVICES_COMMAND_TABLE_H

#include "domain/kernel/ipc.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace hexlith::domain::services {

// A service whose commands are a table: each command ID with its name, from
// 3dbrew's page for the service, and the member that answers it, or none.
//
// A command with no member is known and not implemented, and stops the run by
// name; a command not in the table stops it by header. The derived class
// provides the table as a static function Commands().
template<typename Derived>
class CommandTable : public kernel::SessionHandler {
public:
    struct Command {
        std::uint32_t id = 0;
        std::string_view name;
        void (Derived::*handler)(kernel::IpcRequest&) = nullptr;
    };

    bool Handle(kernel::IpcRequest& request) override {
        for (const Command& command : Derived::Commands()) {
            if (command.id == request.Command()) {
                if (command.handler == nullptr) {
                    return false;
                }
                (static_cast<Derived&>(*this).*command.handler)(request);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::string_view CommandName(std::uint32_t id) const override {
        for (const Command& command : Derived::Commands()) {
            if (command.id == id) {
                return command.name;
            }
        }
        return {};
    }

private:
    // Only the service that names itself as Derived can be one.
    CommandTable() = default;
    friend Derived;
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_COMMAND_TABLE_H
