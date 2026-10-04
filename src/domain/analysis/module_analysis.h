#ifndef HEXLITH_DOMAIN_ANALYSIS_MODULE_ANALYSIS_H
#define HEXLITH_DOMAIN_ANALYSIS_MODULE_ANALYSIS_H

#include "core/byte_reader.h"
#include "core/result.h"
#include "domain/analysis/code_walk.h"
#include "domain/analysis/export_seed.h"
#include "domain/analysis/function_bounds.h"
#include "domain/analysis/tables.h"
#include "domain/linker/cro_linker.h"

#include <cstdint>
#include <span>
#include <vector>

namespace hexlith::domain::analysis {

// Analysing a CRO module, which differs from analysing the main executable in
// two ways that both come from the relocation tables.
//
// A module does not know where it will be loaded, so it is analysed in an
// address space of its own: the module at address zero, its data where the file
// has it, and its BSS just past the end of the file. Every address in its
// tables is then an offset into the file, except in the BSS, which is not in
// the file. ADR 0006 keeps each module's tables apart for this reason.
//
// And a module says which of its words are addresses. Every one is the target
// of a relocation, because the loader would otherwise leave it pointing into
// whatever was at the address the module was built for. So nothing here guesses
// from a word's value whether it is a pointer: a relocated word is one, and an
// unrelocated word is not.

// The placement a module is analysed at. docs/classification.md has the layout.
linker::Placement AnalysisPlacement(const ctr::CroHeader& header);

// The seeds a module's own tables do not give: positions in its code that
// other modules import from it. They come from those modules' tables, which is
// why the caller collects them.
struct ModuleAnalysisRequest {
    const linker::CroModule* module = nullptr;
    core::ByteSpan file;
    std::span<const ctr::CroSegmentTag> imported_by_others;
};

struct ModuleAnalysisReport {
    std::size_t exported_functions = 0;
    std::size_t imported_positions = 0;
    std::size_t relocated_code_pointers = 0;

    // Relocated words that point out of the module, at an import. They are
    // the module's calls into others, and say nothing about its own code.
    std::size_t imported_words = 0;

    std::uint32_t code_bytes = 0;
    std::uint32_t code_reached = 0;

    WalkReport walk;
    BoundsReport bounds;
};

struct ModuleAnalysis {
    std::vector<Symbol> symbols;
    std::vector<FunctionRange> functions;
    std::vector<Region> regions;
    std::vector<CodePointer> pointers;
    ModuleAnalysisReport report;
};

core::Result<ModuleAnalysis> AnalyzeModule(const linker::CroModule& static_module,
                                           const ModuleAnalysisRequest& request);

}  // namespace hexlith::domain::analysis

#endif  // HEXLITH_DOMAIN_ANALYSIS_MODULE_ANALYSIS_H
