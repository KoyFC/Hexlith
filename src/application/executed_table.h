#ifndef HEXLITH_APPLICATION_EXECUTED_TABLE_H
#define HEXLITH_APPLICATION_EXECUTED_TABLE_H

#include "domain/arm/interpreter/executed_addresses.h"

#include <cstdint>
#include <string>

namespace hexlith::application {

// Where the table of a run's executed instructions goes, under the title's
// directory in the workspace, and what it is called.
constexpr std::string_view ObservedDirectory = "observed";
constexpr std::string_view ExecutedTableName = "executed.txt";

// The instructions a run executed inside one address range, as a table in the
// grammar of docs/analysis-tables.md:
//
//     0x00100000 isa=arm count=3
//
// The range is for the main executable, whose addresses mean the same thing in
// every run. A module's addresses depend on where the game had it loaded, so a
// run's record of them is not comparable with another run's and is left out.
std::string RenderExecutedTable(const domain::arm::ExecutedAddresses& executed,
                                std::uint32_t first,
                                std::uint32_t end);

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_EXECUTED_TABLE_H
