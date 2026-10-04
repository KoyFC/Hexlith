#ifndef HEXLITH_APPLICATION_ANALYSIS_TABLES_H
#define HEXLITH_APPLICATION_ANALYSIS_TABLES_H

#include "core/result.h"
#include "domain/analysis/merge.h"
#include "domain/analysis/tables.h"
#include "ports/blob_sink.h"

#include <string>
#include <string_view>
#include <vector>

namespace hexlith::application {

// The tables as they stand, so that a second run replaces what it can derive
// again and leaves alone what it cannot. Empty on a first run.
struct ExistingTables {
    std::string symbols;
    std::string functions;
    std::string regions;
    std::string pointers;
};

// What one pass derived, before it is merged with what is already written.
struct DerivedTables {
    std::vector<domain::analysis::Symbol> symbols;
    std::vector<domain::analysis::FunctionRange> functions;
    std::vector<domain::analysis::Region> regions;
    std::vector<domain::analysis::CodePointer> pointers;
};

struct WrittenTables {
    domain::analysis::MergeReport merged_symbols;
    domain::analysis::MergeReport merged_functions;
    domain::analysis::MergeReport merged_regions;
    domain::analysis::MergeReport merged_pointers;

    std::size_t symbols = 0;
    std::size_t functions = 0;
    std::size_t regions = 0;
    std::size_t pointers = 0;

    [[nodiscard]] std::size_t Preserved() const {
        return merged_symbols.preserved + merged_functions.preserved + merged_regions.preserved +
               merged_pointers.preserved;
    }
};

// Merges what a pass derived into what is already written, by the provenance
// rules of ADR 0006, and writes the four tables into a directory of the sink.
// An empty directory is the sink's own root.
//
// A table that is there and does not parse stops the run rather than being
// overwritten: the work in it may be the only copy.
core::Result<WrittenTables> MergeAndWriteTables(const DerivedTables& derived,
                                                const ExistingTables& existing,
                                                ports::BlobSink& sink,
                                                std::string_view directory);

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_ANALYSIS_TABLES_H
