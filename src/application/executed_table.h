#ifndef HEXLITH_APPLICATION_EXECUTED_TABLE_H
#define HEXLITH_APPLICATION_EXECUTED_TABLE_H

#include "domain/arm/interpreter/executed_addresses.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::application {

// Where the table of a run's executed instructions goes, under the title's
// directory in the workspace, and what it is called.
constexpr std::string_view ObservedDirectory = "observed";
constexpr std::string_view ExecutedTableName = "executed.txt";

// Where the tables of the modules are, one directory to a module by the name in
// its header, beside the tables of the analysis (modules/<name>/).
constexpr std::string_view ObservedModulesDirectory = "modules";

// The instructions a run executed inside one address range, as a table in the
// grammar of docs/analysis-tables.md:
//
//     0x00100000 isa=arm count=3
//
// For the main executable the range is its text segment and the addresses are
// where it is mapped, which mean the same thing in every run. A module's
// addresses depend on where the game had it loaded, so what a run executed in a
// module is recorded as offsets from the module's start, the file offsets its
// analysis tables use, and the range is its code segment (ExecutionRecorder).
std::string RenderExecutedTable(const domain::arm::ExecutedAddresses& executed,
                                std::uint32_t first,
                                std::uint32_t end);

// Records what a run executed, in the main executable by address and in each
// module by offset from where it is loaded now.
//
// The same module loaded at two addresses, in the same run or in two, is one
// record, because an offset means the same instruction wherever the module is.
// An instruction is a module's when its address is in the code segment of a
// module that is loaded, and the executable's otherwise.
class ExecutionRecorder final : public domain::arm::ExecutionObserver {
public:
    struct ModuleRecord {
        std::uint32_t code_offset = 0;
        std::uint32_t code_size = 0;
        domain::arm::ExecutedAddresses executed;
    };

    void BeforeInstruction(std::uint32_t address, bool thumb) override;

    // The loader's events, with the module's code segment as the event gives it.
    void ModuleLoaded(std::string_view name,
                      std::uint32_t address,
                      std::uint32_t code_offset,
                      std::uint32_t code_size);
    void ModuleUnloaded(std::string_view name, std::uint32_t address);

    [[nodiscard]] const domain::arm::ExecutedAddresses& Main() const {
        return main_;
    }

    // Every module that was loaded at any point, by the name in its header.
    [[nodiscard]] const std::map<std::string, ModuleRecord, std::less<>>& Modules() const {
        return modules_;
    }

private:
    struct Resident {
        std::string name;
        std::uint32_t base = 0;
        std::uint32_t first = 0;
        std::uint32_t end = 0;
        ModuleRecord* record = nullptr;
    };

    domain::arm::ExecutedAddresses main_;
    std::map<std::string, ModuleRecord, std::less<>> modules_;
    std::vector<Resident> residents_;
};

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_EXECUTED_TABLE_H
