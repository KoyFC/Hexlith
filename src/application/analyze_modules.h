#ifndef HEXLITH_APPLICATION_ANALYZE_MODULES_H
#define HEXLITH_APPLICATION_ANALYZE_MODULES_H

#include "application/analysis_tables.h"
#include "core/byte_reader.h"
#include "core/result.h"
#include "domain/analysis/module_analysis.h"
#include "ports/blob_sink.h"

#include <map>
#include <span>
#include <string>
#include <vector>

namespace hexlith::application {

// The analysis pass over every CRO module: each one in its own address space,
// seeded from its exports, its relocations and the positions other modules
// import from it, with its four tables written into a directory of its own.
//
// The modules are analysed together only because a module's entry points are
// partly written in the others' import tables. Nothing is linked across them:
// docs/classification.md has why each stands alone.

// Where a module's tables go, under the sink's root.
constexpr std::string_view ModulesDirectory = "modules";

struct AnalyzeModulesRequest {
    core::ByteSpan static_module;
    std::span<const core::ByteSpan> modules;
};

struct ModuleSummary {
    std::string name;
    domain::analysis::ModuleAnalysisReport report;
    WrittenTables tables;
};

struct AnalyzeModulesReport {
    std::vector<ModuleSummary> modules;

    std::uint64_t code_bytes = 0;
    std::uint64_t code_reached = 0;
    std::size_t functions = 0;
};

// The existing tables of each module, by module name. A module missing from the
// map is a first run for it.
using ExistingModuleTables = std::map<std::string, ExistingTables>;

core::Result<AnalyzeModulesReport> AnalyzeModules(const AnalyzeModulesRequest& request,
                                                  const ExistingModuleTables& existing,
                                                  ports::BlobSink& sink);

// One line per module, then the totals. Written beside the tables, because 133
// lines are more than a person wants on a terminal.
std::string RenderModulesReport(const AnalyzeModulesReport& report);

// The totals alone, for the terminal.
std::string RenderModulesSummary(const AnalyzeModulesReport& report);

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_ANALYZE_MODULES_H
