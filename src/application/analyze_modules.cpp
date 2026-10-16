#include "application/analyze_modules.h"

#include <format>

namespace hexlith::application {
namespace {

namespace analysis = domain::analysis;
namespace ctr = domain::ctr;
namespace linker = domain::linker;

constexpr double Percent = 100.0;

// Every position any module imports from another, by the name of the module it
// is imported from. The static module imports from the modules too.
std::map<std::string, std::vector<ctr::CroSegmentTag>>
ImportedPositions(const linker::CroModule& static_module,
                  const std::vector<linker::CroModule>& modules) {
    std::map<std::string, std::vector<ctr::CroSegmentTag>> imported;
    const auto collect = [&imported](const linker::CroModule& importer) {
        for (const ctr::CroImportModule& from : importer.tables.import_modules) {
            std::vector<ctr::CroSegmentTag>& positions = imported[from.name];
            for (const ctr::CroAnonymousImport& entry : from.anonymous) {
                positions.push_back(entry.position);
            }
        }
    };
    collect(static_module);
    for (const linker::CroModule& module : modules) {
        collect(module);
    }
    return imported;
}

double PercentOf(std::uint64_t part, std::uint64_t whole) {
    return whole == 0 ? 0.0 : Percent * static_cast<double>(part) / static_cast<double>(whole);
}

}  // namespace

core::Result<AnalyzeModulesReport> AnalyzeModules(const AnalyzeModulesRequest& request,
                                                  const ExistingModuleTables& existing,
                                                  ports::BlobSink& sink) {
    const core::Result<linker::CroModule> static_module =
        linker::ReadCroModule(request.static_module);
    if (!static_module) {
        return static_module.Failure().WithContext("the static module");
    }

    std::vector<linker::CroModule> modules;
    modules.reserve(request.modules.size());
    for (const core::ByteSpan file : request.modules) {
        const core::Result<linker::CroModule> module = linker::ReadCroModule(file);
        if (!module) {
            return module.Failure();
        }
        modules.push_back(module.Value());
    }

    const std::map<std::string, std::vector<ctr::CroSegmentTag>> imported =
        ImportedPositions(static_module.Value(), modules);
    static const std::vector<ctr::CroSegmentTag> NothingImported;

    AnalyzeModulesReport report;
    for (std::size_t index = 0; index < modules.size(); ++index) {
        const linker::CroModule& module = modules.at(index);
        const auto found = imported.find(module.header.name);

        const analysis::ModuleAnalysisRequest one{
            .module = &module,
            .file = request.modules[index],
            .imported_by_others = found == imported.end() ? NothingImported : found->second};
        const core::Result<analysis::ModuleAnalysis> analysed =
            analysis::AnalyzeModule(static_module.Value(), one);
        if (!analysed) {
            return analysed.Failure();
        }

        const DerivedTables derived{.symbols = analysed.Value().symbols,
                                    .functions = analysed.Value().functions,
                                    .regions = analysed.Value().regions,
                                    .pointers = analysed.Value().pointers};
        const auto old = existing.find(module.header.name);
        const core::Result<WrittenTables> written =
            MergeAndWriteTables(derived,
                                old == existing.end() ? ExistingTables{} : old->second,
                                sink,
                                std::format("{}/{}", ModulesDirectory, module.header.name));
        if (!written) {
            return written.Failure().WithContext(module.header.name);
        }

        report.code_bytes += analysed.Value().report.code_bytes;
        report.code_reached += analysed.Value().report.code_reached;
        report.functions += written.Value().functions;
        report.modules.push_back(ModuleSummary{.name = module.header.name,
                                               .report = analysed.Value().report,
                                               .tables = written.Value()});
    }
    return report;
}

std::string RenderModulesReport(const AnalyzeModulesReport& report) {
    std::string text =
        "# One line per module: code reached of code in the module, functions, and the\n"
        "# seeds the analysis started from. Written by hexlith-analyze.\n";
    for (const ModuleSummary& module : report.modules) {
        const analysis::ModuleAnalysisReport& one = module.report;
        text.append(std::format(
            "{} code={} reached={} ({:.1f}%) functions={} exported={} imported-by-others={} "
            "relocated-code-pointers={} imported-words={} conflicts={} undefined-stops={}\n",
            module.name,
            one.code_bytes,
            one.code_reached,
            PercentOf(one.code_reached, one.code_bytes),
            module.tables.functions,
            one.exported_functions,
            one.imported_positions,
            one.relocated_code_pointers,
            one.imported_words,
            one.walk.instruction_set_conflicts.size(),
            one.walk.undefined_stops));
    }
    return text;
}

std::string RenderModulesSummary(const AnalyzeModulesReport& report) {
    std::size_t conflicts = 0;
    std::size_t stops = 0;
    std::size_t preserved = 0;
    for (const ModuleSummary& module : report.modules) {
        conflicts += module.report.walk.instruction_set_conflicts.size();
        stops += module.report.walk.undefined_stops;
        preserved += module.tables.Preserved();
    }
    std::string text;
    text.append(std::format("{} module(s) analysed\n", report.modules.size()));
    text.append(std::format("{} of {} module code byte(s) reached, {:.1f}%\n",
                            report.code_reached,
                            report.code_bytes,
                            PercentOf(report.code_reached, report.code_bytes)));
    text.append(std::format("{} function(s)\n", report.functions));
    text.append(std::format("{} instruction set conflict(s), {} stop(s) at a word that is not an "
                            "instruction\n",
                            conflicts,
                            stops));
    text.append(std::format("preserved {} record(s) nothing can derive again\n", preserved));
    return text;
}

}  // namespace hexlith::application
