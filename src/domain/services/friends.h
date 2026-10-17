#ifndef HEXLITH_DOMAIN_SERVICES_FRIENDS_H
#define HEXLITH_DOMAIN_SERVICES_FRIENDS_H

#include "domain/kernel/kernel.h"
#include "domain/services/command_table.h"

#include <span>

namespace hexlith::domain::services {

// frd:u, the friend list. There is no network and no friend: the console is
// offline, its friend key is zero and its list is empty. Commands: 3dbrew,
// Friend Services; answers: Module::Interface in Azahar,
// src/core/hle/service/frd/frd.cpp, which answers the same way.
class Friends final : public CommandTable<Friends> {
public:
    [[nodiscard]] std::string_view Name() const override {
        return "frd:u";
    }

    static std::span<const Command> Commands();

private:
    void HasLoggedIn(kernel::IpcRequest& request);
    void GetMyFriendKey(kernel::IpcRequest& request);
    void GetMyPresence(kernel::IpcRequest& request);
    void GetFriendKeyList(kernel::IpcRequest& request);
    void SetClientSdkVersion(kernel::IpcRequest& request);
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_FRIENDS_H
