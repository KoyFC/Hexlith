#ifndef HEXLITH_DOMAIN_SERVICES_SPOT_PASS_H
#define HEXLITH_DOMAIN_SERVICES_SPOT_PASS_H

#include "domain/kernel/kernel.h"
#include "domain/services/command_table.h"

#include <span>

namespace hexlith::domain::services {

// boss:U, SpotPass, which downloads data for a title in the background. The
// console is offline and has no task database, so a session opens, storage is
// accepted and forgotten, and nothing is ever downloaded. Commands: 3dbrew,
// BOSS Services; answers: Module::Interface in Azahar,
// src/core/hle/service/boss/boss.cpp, which answers the same way when its
// database is missing, as it is on a first boot.
class SpotPass final : public CommandTable<SpotPass> {
public:
    [[nodiscard]] std::string_view Name() const override {
        return "boss:U";
    }

    static std::span<const Command> Commands();

    [[nodiscard]] bool HasSession() const {
        return session_;
    }

private:
    void InitializeSession(kernel::IpcRequest& request);
    void UnregisterStorage(kernel::IpcRequest& request);
    void RegisterStorageEntry(kernel::IpcRequest& request);
    void GetStorageEntryInfo(kernel::IpcRequest& request);

    bool session_ = false;
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_SPOT_PASS_H
