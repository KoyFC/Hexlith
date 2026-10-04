#include "application/analyze.h"

#include "domain/analysis/export_seed.h"
#include "domain/analysis/tables.h"
#include "domain/ctr/cro.h"

#include <format>
#include <vector>

namespace hexlith::application {
namespace {

namespace analysis = domain::analysis;
namespace ctr = domain::ctr;

// Parses a table that is already there, so that regenerating preserves whatever
// a person put in it. A file that does not parse stops the run rather than being
// overwritten: the work in it is the only copy.
template<typename T, typename Parse>
core::Result<std::vector<T>>
ReadExisting(std::string_view text, Parse parse, std::string_view what) {
    if (text.empty()) {
        return std::vector<T>{};
    }
    const core::Result<std::vector<T>> parsed = parse(text);
    if (!parsed) {
        return parsed.Failure().WithContext(what);
    }
    return parsed.Value();
}

core::Status WriteTable(ports::BlobSink& sink, std::string_view name, std::string_view text) {
    return sink.WriteText(name, text);
}

}  // namespace

core::Result<AnalyzeReport>
Analyze(const AnalyzeRequest& request, const ExistingTables& existing, ports::BlobSink& sink) {
    if (request.profile == nullptr) {
        return core::Error{"no profile was given, and the segment addresses come from it"};
    }

    AnalyzeReport report;

    // The names, which are the only thing here that could not be derived again.
    const core::Result<ctr::CroHeader> header = ctr::ParseCroHeader(request.static_module);
    if (!header) {
        return header.Failure().WithContext("the static module");
    }
    if (!header.Value().IsStaticModule()) {
        return core::Error{
            std::format("the module is named {} and not the static one", header.Value().name)};
    }

    const core::Result<std::vector<ctr::CroNamedExport>> exports =
        ctr::ReadCroNamedExports(request.static_module, header.Value());
    if (!exports) {
        return exports.Failure();
    }

    const core::Result<analysis::SeededTables> seeded =
        analysis::SeedFromExports(header.Value(), exports.Value());
    if (!seeded) {
        return seeded.Failure();
    }

    report.exported_symbols = seeded.Value().symbols.size();
    report.exported_functions = seeded.Value().functions.size();

    const domain::profile::GameProfile& profile = *request.profile;
    const analysis::WalkImage image{.bytes = request.code, .base = profile.text.address};

    if (request.code.size() < profile.text.size) {
        return core::Error{std::format(
            "the code image is {:#x} bytes and the profile's text segment alone is {:#x}",
            request.code.size(),
            profile.text.size)};
    }

    // The entry point, plus every exported function with the instruction set its
    // exported address settled.
    std::vector<analysis::WalkSeed> seeds;
    seeds.reserve(seeded.Value().functions.size() + 1);
    seeds.push_back(analysis::WalkSeed{.address = profile.text.address,
                                       .instruction_set = domain::arm::InstructionSet::Arm,
                                       .source = analysis::Provenance::ExHeader});
    for (const analysis::FunctionRange& function : seeded.Value().functions) {
        seeds.push_back(analysis::WalkSeed{.address = function.address,
                                           .instruction_set = function.instruction_set,
                                           .source = analysis::Provenance::CrsExport});
    }

    const auto image_end = static_cast<std::uint32_t>(profile.text.address + request.code.size());
    analysis::WalkOptions options;
    options.follow_literal_pointers = true;
    options.code_begin = profile.text.address;
    options.code_end = profile.text.address + profile.text.size;
    if (request.sweep_data_for_entry_points) {
        options.pointer_scan_begin = profile.read_only.address;
        options.pointer_scan_end = image_end;
    }

    const analysis::WalkResult walked = analysis::WalkCode(image, seeds, options);
    report.walk = walked.report;

    // Every seed is the start of a function as well as a place to walk from. The
    // walk records only the functions it finds by following a call, so an
    // exported function nothing in the image calls would otherwise have no
    // record. The seeds come first, so that where both name an address, the
    // record keeps the provenance of the seed.
    std::vector<analysis::FunctionRange> entries;
    entries.reserve(seeds.size() + walked.functions.size());
    for (const analysis::WalkSeed& seed : seeds) {
        entries.push_back(analysis::FunctionRange{.address = seed.address,
                                                  .size = analysis::UnknownSize,
                                                  .instruction_set = seed.instruction_set,
                                                  .alternate_entries = {},
                                                  .source = seed.source});
    }
    entries.insert(entries.end(), walked.functions.begin(), walked.functions.end());

    const analysis::BoundsResult bounds =
        analysis::FindFunctionBounds(image, entries, walked.regions);
    report.bounds = bounds.report;

    report.text_bytes = profile.text.size;
    for (const analysis::Region& region : walked.regions) {
        if (region.kind == analysis::RegionKind::Code) {
            report.code_bytes += region.size;
        }
    }

    // Everything derived is merged into what is already written, which keeps a
    // name a person gave an address from being replaced by one derived for it.
    const core::Result<std::vector<analysis::Symbol>> old_symbols =
        ReadExisting<analysis::Symbol>(existing.symbols, analysis::ParseSymbols, "symbols.txt");
    if (!old_symbols) {
        return old_symbols.Failure();
    }
    const core::Result<std::vector<analysis::FunctionRange>> old_functions =
        ReadExisting<analysis::FunctionRange>(
            existing.functions, analysis::ParseFunctions, "functions.txt");
    if (!old_functions) {
        return old_functions.Failure();
    }
    const core::Result<std::vector<analysis::Region>> old_regions =
        ReadExisting<analysis::Region>(existing.regions, analysis::ParseRegions, "regions.txt");
    if (!old_regions) {
        return old_regions.Failure();
    }
    const core::Result<std::vector<analysis::CodePointer>> old_pointers =
        ReadExisting<analysis::CodePointer>(
            existing.pointers, analysis::ParsePointers, "pointers.txt");
    if (!old_pointers) {
        return old_pointers.Failure();
    }

    const std::vector<analysis::Symbol> symbols =
        analysis::MergeRecords(old_symbols.Value(), seeded.Value().symbols, report.merged_symbols);
    const std::vector<analysis::FunctionRange> functions =
        analysis::MergeRecords(old_functions.Value(), bounds.functions, report.merged_functions);
    const std::vector<analysis::Region> regions =
        analysis::MergeRecords(old_regions.Value(), walked.regions, report.merged_regions);
    const std::vector<analysis::CodePointer> pointers =
        analysis::MergeRecords(old_pointers.Value(), walked.pointers, report.merged_pointers);

    report.symbols = symbols.size();
    report.functions = functions.size();
    report.regions = regions.size();
    report.pointers = pointers.size();

    const core::Status wrote_symbols =
        WriteTable(sink, analysis::SymbolsFileName, analysis::RenderSymbols(symbols));
    if (!wrote_symbols) {
        return wrote_symbols.Failure();
    }
    const core::Status wrote_functions =
        WriteTable(sink, analysis::FunctionsFileName, analysis::RenderFunctions(functions));
    if (!wrote_functions) {
        return wrote_functions.Failure();
    }
    const core::Status wrote_regions =
        WriteTable(sink, analysis::RegionsFileName, analysis::RenderRegions(regions));
    if (!wrote_regions) {
        return wrote_regions.Failure();
    }
    const core::Status wrote_pointers =
        WriteTable(sink, analysis::PointersFileName, analysis::RenderPointers(pointers));
    if (!wrote_pointers) {
        return wrote_pointers.Failure();
    }

    return report;
}

std::string RenderAnalyzeSummary(const AnalyzeReport& report) {
    const double reached = report.text_bytes == 0 ? 0.0
                                                  : 100.0 * static_cast<double>(report.code_bytes) /
                                                        static_cast<double>(report.text_bytes);

    std::string text;
    text.append(std::format("{} exported symbol(s), {} of them functions\n",
                            report.exported_symbols,
                            report.exported_functions));
    text.append(std::format("{} instruction(s) decoded from {} seed(s)\n",
                            report.walk.instructions,
                            report.walk.seeds_followed));
    text.append(std::format(
        "{} of {} text byte(s) reached, {:.1f}%\n", report.code_bytes, report.text_bytes, reached));
    text.append(std::format("{} function(s), {} secondary entry point(s), {} overlapping\n",
                            report.bounds.functions,
                            report.bounds.alternate_entries,
                            report.bounds.overlapping));
    text.append(std::format("{} instruction set conflict(s), {} stop(s) at a word that is not an "
                            "instruction\n",
                            report.walk.instruction_set_conflicts,
                            report.walk.undefined_stops));
    text.append(std::format("{} pointer(s) in data, {} word(s) swept without a prologue\n",
                            report.pointers,
                            report.walk.words_without_a_prologue));
    text.append(std::format("wrote {} symbol(s), {} function(s), {} region(s), {} pointer(s)\n",
                            report.symbols,
                            report.functions,
                            report.regions,
                            report.pointers));
    text.append(std::format("preserved {} record(s) nothing can derive again\n",
                            report.merged_symbols.preserved + report.merged_functions.preserved +
                                report.merged_regions.preserved +
                                report.merged_pointers.preserved));
    return text;
}

}  // namespace hexlith::application
