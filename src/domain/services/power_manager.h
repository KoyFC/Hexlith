#ifndef HEXLITH_DOMAIN_SERVICES_POWER_MANAGER_H
#define HEXLITH_DOMAIN_SERVICES_POWER_MANAGER_H

#include "domain/kernel/kernel.h"
#include "domain/services/command_table.h"

#include <span>

namespace hexlith::domain::services {

// ptm:u, the power and time manager as an application sees it: the battery,
// the shell, and the pedometer's history. Nobody walks with this console, so
// every hour of the history counts zero steps. Commands: 3dbrew, PTM
// Services; answers: Module::Interface in Azahar,
// src/core/hle/service/ptm/ptm.cpp, with its default of zero steps an hour.
class PowerManager final : public CommandTable<PowerManager> {
public:
    [[nodiscard]] std::string_view Name() const override {
        return "ptm:u";
    }

    static std::span<const Command> Commands();

private:
    void GetStepHistory(kernel::IpcRequest& request);
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_POWER_MANAGER_H
