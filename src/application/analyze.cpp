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
    const DerivedTables derived{.symbols = seeded.Value().symbols,
                                .functions = bounds.functions,
                                .regions = walked.regions,
                                .pointers = walked.pointers};
    const core::Result<WrittenTables> written = MergeAndWriteTables(derived, existing, sink, "");
    if (!written) {
        return written.Failure();
    }
    report.tables = written.Value();

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
                            report.tables.pointers,
                            report.walk.words_without_a_prologue));
    text.append(std::format("wrote {} symbol(s), {} function(s), {} region(s), {} pointer(s)\n",
                            report.tables.symbols,
                            report.tables.functions,
                            report.tables.regions,
                            report.tables.pointers));
    text.append(std::format("preserved {} record(s) nothing can derive again\n",
                            report.tables.Preserved()));
    return text;
}

}  // namespace hexlith::application
