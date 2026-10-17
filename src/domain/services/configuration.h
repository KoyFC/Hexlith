#ifndef HEXLITH_DOMAIN_SERVICES_CONFIGURATION_H
#define HEXLITH_DOMAIN_SERVICES_CONFIGURATION_H

#include "domain/kernel/kernel.h"
#include "domain/services/command_table.h"

#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace hexlith::domain::services {

// cfg:u, the system configuration: the console's language, region, user name,
// birthday, country and model, each a block read by ID. Commands: 3dbrew, CFG
// Services; the block layouts and the default values are the ones Azahar
// creates a new console's configuration with, in
// src/core/hle/service/cfg/cfg_defaults.cpp, except where noted.
//
// A block that is not in the table declines the request rather than answering
// that it does not exist, since the console's configuration holds more blocks
// than any default list, and a game told one is missing may behave as it never
// would on a console.
class Configuration final : public CommandTable<Configuration> {
public:
    Configuration();

    [[nodiscard]] std::string_view Name() const override {
        return "cfg:u";
    }

    static std::span<const Command> Commands();

    // Block IDs. Source: ConfigBlockID in Azahar's cfg.h; 3dbrew, Config
    // Savegame.
    static constexpr std::uint32_t LanguageBlock = 0x000A0002;
    static constexpr std::uint32_t ConsoleModelBlock = 0x000F0004;
    static constexpr std::uint32_t ConsoleUniqueId2Block = 0x00090001;

    // The console's region: 1 is the Americas, which is what Azahar picked for
    // the reference title in the captured runs.
    static constexpr std::uint8_t Region = 1;

    // The model reported: the original Nintendo 3DS, to match the Old 3DS
    // memory layout the kernel gives the title. Azahar reports a New 3DS XL.
    static constexpr std::uint8_t Model = 0;

    [[nodiscard]] const std::vector<std::uint8_t>* Block(std::uint32_t id) const;

private:
    void GetConfig(kernel::IpcRequest& request);
    void GetRegion(kernel::IpcRequest& request);
    void GetTransferableId(kernel::IpcRequest& request);
    void GetSystemModel(kernel::IpcRequest& request);
    void GetModelNintendo2DS(kernel::IpcRequest& request);

    std::map<std::uint32_t, std::vector<std::uint8_t>> blocks_;
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_CONFIGURATION_H
