#include "application/analyze.h"

#include "domain/analysis/export_seed.h"
#include "domain/analysis/record_text.h"
#include "domain/analysis/tables.h"
#include "domain/ctr/cro.h"
#include "domain/ctr/cro_link_tables.h"
#include "domain/ctr/layout.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hexlith::application {
namespace {

namespace analysis = domain::analysis;
namespace ctr = domain::ctr;

// Every position in the static module that a loadable module imports, once.
core::Result<std::vector<ctr::CroSegmentTag>>
PositionsImportedFromStatic(std::span<const core::ByteSpan> modules) {
    std::set<std::uint32_t> seen;
    std::vector<ctr::CroSegmentTag> positions;
    for (const core::ByteSpan file : modules) {
        const core::Result<ctr::CroHeader> header = ctr::ParseCroHeader(file);
        if (!header) {
            return header.Failure();
        }
        const core::Result<ctr::CroLinkTables> tables =
            ctr::ReadCroLinkTables(file, header.Value());
        if (!tables) {
            return tables.Failure().WithContext(header.Value().name);
        }
        for (const ctr::CroImportModule& from : tables.Value().import_modules) {
            if (from.name != ctr::cro::StaticModuleName) {
                continue;
            }
            for (const ctr::CroAnonymousImport& entry : from.anonymous) {
                if (seen.insert(entry.position.raw).second) {
                    positions.push_back(entry.position);
                }
            }
        }
    }
    return positions;
}

// The instructions a run executed inside the text segment, from the table
// hexlith-boot --record-executed writes. Empty text is no run.
core::Result<std::vector<analysis::ObservedInstruction>>
ObservedInText(std::string_view text, const domain::profile::GameProfile& profile) {
    std::vector<analysis::ObservedInstruction> observed;
    if (text.empty()) {
        return observed;
    }
    const core::Result<analysis::RecordFile> parsed = analysis::ParseRecordText(text);
    if (!parsed) {
        return parsed.Failure();
    }
    for (const analysis::Record& record : parsed.Value().records) {
        const core::Result<std::string_view> isa = analysis::RequiredField(record, "isa");
        if (!isa) {
            return isa.Failure();
        }
        if (isa.Value() != "arm" && isa.Value() != "thumb") {
            return core::Error{std::format("0x{:08X} was executed in an instruction set called {}",
                                           record.address,
                                           isa.Value())};
        }
        if (record.address < profile.text.address ||
            record.address - profile.text.address >= profile.text.size) {
            continue;
        }
        observed.push_back(analysis::ObservedInstruction{
            .address = record.address,
            .instruction_set = isa.Value() == "thumb" ? domain::arm::InstructionSet::Thumb
                                                      : domain::arm::InstructionSet::Arm});
    }
    return observed;
}

// What a run executed at each address, for saying which conflicts it settles.
std::map<std::uint32_t, std::string_view>
ExecutedSets(std::span<const analysis::ObservedInstruction> observed) {
    std::map<std::uint32_t, std::string_view> sets;
    for (const analysis::ObservedInstruction& instruction : observed) {
        const std::string_view name =
            instruction.instruction_set == domain::arm::InstructionSet::Thumb ? "thumb" : "arm";
        auto [at, added] = sets.emplace(instruction.address, name);
        if (!added && at->second != name) {
            at->second = "arm and thumb";
        }
    }
    return sets;
}

void CountSettledConflicts(AnalyzeReport& report,
                           std::span<const analysis::ObservedInstruction> observed) {
    const std::set<std::uint32_t> addresses(report.walk.instruction_set_conflicts.begin(),
                                            report.walk.instruction_set_conflicts.end());
    const std::map<std::uint32_t, std::string_view> executed = ExecutedSets(observed);
    report.conflict_addresses = addresses.size();
    report.conflicts_settled_by_observation = static_cast<std::size_t>(std::ranges::count_if(
        addresses, [&executed](std::uint32_t address) { return executed.contains(address); }));
    report.executed_at_conflicts.clear();
    for (const std::uint32_t address : addresses) {
        if (const auto found = executed.find(address); found != executed.end()) {
            report.executed_at_conflicts.emplace(address, std::string{found->second});
        }
    }
}

// Where a run of executed instructions starts outside every function's
// extent, as function entries. An instruction outside every extent whose
// predecessor is too belongs to the same run and to the entry made for it.
std::vector<analysis::FunctionRange>
EntriesOutsideEveryFunction(std::span<const analysis::FunctionRange> functions,
                            std::span<const analysis::WalkSeed> observed) {
    // The extents merged into sorted, disjoint ranges, so that each place is
    // one search.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> covered;
    for (const analysis::FunctionRange& function : functions) {
        // A size of zero is a function whose extent could not be measured.
        if (function.size != analysis::UnknownSize) {
            covered.emplace_back(function.address, function.address + function.size);
        }
    }
    std::ranges::sort(covered);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> merged;
    for (const auto& range : covered) {
        if (!merged.empty() && range.first <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, range.second);
        } else {
            merged.push_back(range);
        }
    }
    const auto inside = [&merged](std::uint32_t address) {
        const auto after = std::ranges::upper_bound(
            merged, address, {}, [](const auto& range) { return range.first; });
        return after != merged.begin() && address < std::prev(after)->second;
    };

    std::set<std::pair<std::uint32_t, bool>> outside;
    for (const analysis::WalkSeed& seed : observed) {
        if (!inside(seed.address)) {
            outside.emplace(seed.address,
                            seed.instruction_set == domain::arm::InstructionSet::Thumb);
        }
    }
    // An ARM instruction is four bytes, a Thumb one two or four.
    constexpr std::uint32_t ArmSize = 4;
    constexpr std::uint32_t ThumbHalf = 2;
    std::vector<analysis::FunctionRange> entries;
    for (const auto& [address, thumb] : outside) {
        const bool continues = thumb ? outside.contains({address - ThumbHalf, true}) ||
                                           outside.contains({address - (2 * ThumbHalf), true})
                                     : outside.contains({address - ArmSize, false});
        if (continues) {
            continue;
        }
        entries.push_back(
            analysis::FunctionRange{.address = address,
                                    .size = analysis::UnknownSize,
                                    .instruction_set = thumb ? domain::arm::InstructionSet::Thumb
                                                             : domain::arm::InstructionSet::Arm,
                                    .alternate_entries = {},
                                    .source = analysis::Provenance::Observed});
    }
    return entries;
}

// The entry point, plus every exported function with the instruction set its
// exported address settled.
std::vector<analysis::WalkSeed> SeedsFromImage(const domain::profile::GameProfile& profile,
                                               const analysis::SeededTables& seeded) {
    std::vector<analysis::WalkSeed> seeds;
    seeds.reserve(seeded.functions.size() + 1);
    seeds.push_back(analysis::WalkSeed{.address = profile.text.address,
                                       .instruction_set = domain::arm::InstructionSet::Arm,
                                       .source = analysis::Provenance::ExHeader});
    for (const analysis::FunctionRange& function : seeded.functions) {
        seeds.push_back(analysis::WalkSeed{.address = function.address,
                                           .instruction_set = function.instruction_set,
                                           .source = analysis::Provenance::CrsExport});
    }
    return seeds;
}

analysis::WalkOptions WalkOptionsFor(const AnalyzeRequest& request) {
    const domain::profile::GameProfile& profile = *request.profile;
    analysis::WalkOptions options;
    options.follow_literal_pointers = true;
    options.code_begin = profile.text.address;
    options.code_end = profile.text.address + profile.text.size;
    if (request.sweep_data_for_entry_points) {
        options.pointer_scan_begin = profile.read_only.address;
        options.pointer_scan_end =
            static_cast<std::uint32_t>(profile.text.address + request.code.size());
    }
    return options;
}

// Every seed is the start of a function as well as a place to walk from. The
// walk records only the functions it finds by following a call, so an
// exported function nothing in the image calls would otherwise have no
// record. The seeds come first, so that where both name an address, the
// record keeps the provenance of the seed.
std::vector<analysis::FunctionRange> EntriesOf(std::span<const analysis::WalkSeed> seeds,
                                               const analysis::WalkResult& walked) {
    std::vector<analysis::FunctionRange> entries;
    entries.reserve(seeds.size() + walked.functions.size());
    for (const analysis::WalkSeed& seed : seeds) {
        // Having run says nothing about where a function starts. The runs no
        // function covers are made entries later, once the extents are known.
        if (seed.source == analysis::Provenance::Observed) {
            continue;
        }
        entries.push_back(analysis::FunctionRange{.address = seed.address,
                                                  .size = analysis::UnknownSize,
                                                  .instruction_set = seed.instruction_set,
                                                  .alternate_entries = {},
                                                  .source = seed.source});
    }
    entries.insert(entries.end(), walked.functions.begin(), walked.functions.end());
    return entries;
}

// The functions' extents, with an entry made for each run of executed
// instructions that starts outside every function.
//
// A run that starts outside every function the image gave or a guess found
// is code that something enters which nothing in the image names. Lifted
// code can be entered at any of its blocks (ADR 0025), and only a function
// is lifted, so the start of such a run becomes an entry of its own. Runs
// that start inside a known function are left to it.
//
// A run can hold more than one function, one ending where the next
// begins, and only the extent of the first is measured from its start; so
// this is repeated until every executed instruction is in some extent, or
// the rounds run out.
// An entry whose extent cannot be measured stays outside every extent, so
// what was added once is not added again.
analysis::BoundsResult BoundsWithObservedEntries(const analysis::WalkImage& image,
                                                 std::vector<analysis::FunctionRange>& entries,
                                                 const analysis::WalkResult& walked,
                                                 std::span<const analysis::WalkSeed> observed,
                                                 AnalyzeReport& report) {
    analysis::BoundsResult bounds = analysis::FindFunctionBounds(image, entries, walked.regions);
    constexpr int MostRounds = 8;
    std::set<std::uint32_t> added;
    for (int round = 0; round < MostRounds; ++round) {
        std::vector<analysis::FunctionRange> unclaimed =
            EntriesOutsideEveryFunction(bounds.functions, observed);
        std::erase_if(unclaimed, [&added](const analysis::FunctionRange& entry) {
            return !added.insert(entry.address).second;
        });
        if (unclaimed.empty()) {
            break;
        }
        report.observed_functions += unclaimed.size();
        entries.insert(entries.end(), unclaimed.begin(), unclaimed.end());
        bounds = analysis::FindFunctionBounds(image, entries, walked.regions);
    }
    return bounds;
}

std::uint32_t CodeBytes(std::span<const analysis::Region> regions) {
    std::uint32_t bytes = 0;
    for (const analysis::Region& region : regions) {
        if (region.kind == analysis::RegionKind::Code) {
            bytes += region.size;
        }
    }
    return bytes;
}

// One line per address that two paths disagree about, with how many paths ran
// into it and the instruction set a run executed it in, if one did, so that
// each one can be read and checked against another tool.
std::string RenderConflicts(const AnalyzeReport& report) {
    std::map<std::uint32_t, std::size_t> reports;
    for (const std::uint32_t address : report.walk.instruction_set_conflicts) {
        ++reports[address];
    }
    std::string text;
    for (const auto& [address, count] : reports) {
        const auto executed = report.executed_at_conflicts.find(address);
        text.append(std::format("  conflict at 0x{:08X}, reported {} time(s){}\n",
                                address,
                                count,
                                executed == report.executed_at_conflicts.end()
                                    ? std::string{}
                                    : std::format(", executed as {}", executed->second)));
    }
    return text;
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

    std::vector<analysis::WalkSeed> seeds = SeedsFromImage(profile, seeded.Value());

    // What the modules import from the main executable's code. These are facts
    // the loader acts on, unlike the words the sweep below guesses at.
    const core::Result<std::vector<ctr::CroSegmentTag>> imported =
        PositionsImportedFromStatic(request.modules);
    if (!imported) {
        return imported.Failure();
    }
    const std::vector<analysis::WalkSeed> imported_seeds =
        analysis::SeedFromImports(header.Value(), imported.Value());
    report.imported_functions = imported_seeds.size();
    seeds.insert(seeds.end(), imported_seeds.begin(), imported_seeds.end());

    // What a run executed, which is walked from as a fact and settles what the
    // bytes are where a guess disagrees.
    const core::Result<std::vector<analysis::ObservedInstruction>> observed =
        ObservedInText(request.executed, profile);
    if (!observed) {
        return observed.Failure().WithContext("executed.txt");
    }
    const std::vector<analysis::WalkSeed> observed_seeds =
        analysis::SeedFromObservations(observed.Value());
    report.observed_seeds = observed_seeds.size();
    seeds.insert(seeds.end(), observed_seeds.begin(), observed_seeds.end());

    const analysis::WalkResult walked = analysis::WalkCode(image, seeds, WalkOptionsFor(request));
    report.walk = walked.report;

    std::vector<analysis::FunctionRange> entries = EntriesOf(seeds, walked);
    const analysis::BoundsResult bounds =
        BoundsWithObservedEntries(image, entries, walked, observed_seeds, report);
    report.bounds = bounds.report;
    CountSettledConflicts(report, observed.Value());

    report.text_bytes = profile.text.size;
    report.code_bytes = CodeBytes(walked.regions);

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
    text.append(
        std::format("{} position(s) in code imported by the modules\n", report.imported_functions));
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
                            report.walk.instruction_set_conflicts.size(),
                            report.walk.undefined_stops));
    if (report.observed_seeds > 0) {
        text.append(std::format("{} place(s) walked from because a run executed them, {} of them "
                                "outside every function and made entries; {} of {} conflict "
                                "address(es) settled by what it executed\n",
                                report.observed_seeds,
                                report.observed_functions,
                                report.conflicts_settled_by_observation,
                                report.conflict_addresses));
    }
    text.append(RenderConflicts(report));
    text.append(std::format("{} jump table(s) of {} entries, {} dispatch(es) refused\n",
                            report.walk.jump_tables,
                            report.walk.jump_table_entries,
                            report.walk.jump_tables_rejected));
    text.append(std::format("{} pointer(s) in data, {} word(s) swept without a prologue, "
                            "{} taken from runs of addresses, {} left alone for pointing into "
                            "walked code\n",
                            report.tables.pointers,
                            report.walk.words_without_a_prologue,
                            report.walk.words_taken_from_tables,
                            report.walk.words_into_walked_code));
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
