#ifndef HEXLITH_APPLICATION_ANALYZE_H
#define HEXLITH_APPLICATION_ANALYZE_H

#include "core/byte_reader.h"
#include "core/result.h"
#include "domain/analysis/code_walk.h"
#include "domain/analysis/function_bounds.h"
#include "domain/analysis/merge.h"
#include "domain/profile/game_profile.h"
#include "ports/blob_sink.h"

#include <string>

namespace hexlith::application {

// The analysis pass over the main executable: seed from the export table, walk
// what is reachable, measure where each function ends, and write the four tables
// of ADR 0006.
//
// The inputs are two files the extraction produced, and nothing here goes looking
// for them. Where they come from is the tool's business.

struct AnalyzeRequest {
    // The decompressed .code image, which holds the three segments contiguously
    // at the addresses the profile gives.
    core::ByteSpan code;

    // static.crs out of the RomFS, for the only names the cartridge gives away.
    core::ByteSpan static_module;

    const domain::profile::GameProfile* profile = nullptr;

    // Whether to sweep read-only data and data for words that look like the
    // address of a function. docs/classification.md has what it costs and what it
    // buys.
    bool sweep_data_for_entry_points = true;
};

// The tables as they stand, so that a second run replaces what it can derive
// again and leaves alone what it cannot. Empty on a first run.
struct ExistingTables {
    std::string symbols;
    std::string functions;
    std::string regions;
    std::string pointers;
};

struct AnalyzeReport {
    std::size_t exported_symbols = 0;
    std::size_t exported_functions = 0;

    domain::analysis::WalkReport walk;
    domain::analysis::BoundsReport bounds;

    domain::analysis::MergeReport merged_symbols;
    domain::analysis::MergeReport merged_functions;
    domain::analysis::MergeReport merged_regions;
    domain::analysis::MergeReport merged_pointers;

    std::size_t symbols = 0;
    std::size_t functions = 0;
    std::size_t regions = 0;
    std::size_t pointers = 0;

    // How much of the text segment the walk reached, which is the number worth
    // watching between runs.
    std::uint32_t code_bytes = 0;
    std::uint32_t text_bytes = 0;
};

core::Result<AnalyzeReport>
Analyze(const AnalyzeRequest& request, const ExistingTables& existing, ports::BlobSink& sink);

// A summary a person reads on the way out, in the same shape the extraction's is.
std::string RenderAnalyzeSummary(const AnalyzeReport& report);

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_ANALYZE_H
