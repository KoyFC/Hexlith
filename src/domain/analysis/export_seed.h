#ifndef HEXLITH_DOMAIN_ANALYSIS_EXPORT_SEED_H
#define HEXLITH_DOMAIN_ANALYSIS_EXPORT_SEED_H

#include "core/result.h"
#include "domain/analysis/code_walk.h"
#include "domain/analysis/tables.h"
#include "domain/ctr/cro.h"

#include <span>
#include <vector>

namespace hexlith::domain::analysis {

// Where the analysis layer starts.
//
// The cartridge gives away 461 symbol names in the static module's export
// table and between one and two in each loadable module, and that is the whole
// of the free symbol information. Everything else has to be derived or
// recognised by a person, so these records are the seed rather than the result.
//
// An export in a code segment settles something the analysis would otherwise
// have to guess: the low bit of its position is the interworking flag, so the
// table says which entry points are Thumb and which are ARM. That is why
// seeding produces function records and not only symbols.

struct SeededTables {
    std::vector<Symbol> symbols;
    std::vector<FunctionRange> functions;
};

// Turns the named exports of one module into records for that module's tables.
//
// The addresses are in whatever space the module's segment offsets are in: file
// offsets for a loadable module, virtual addresses for the static one. Mixing
// the two in one table is what the per-module layout exists to prevent.
//
// An export whose position lies in a segment the module does not declare is a
// failure. The reader refuses the same thing, so reaching it here means the
// records and the header did not come from the same module.
core::Result<SeededTables> SeedFromExports(const ctr::CroHeader& header,
                                           std::span<const ctr::CroNamedExport> exports);

// Turns positions that other modules import from a module into walk seeds in
// that module's address space: the ones in code, with the instruction set the
// interworking bit of each position gives.
//
// An import is a reference the console's loader resolves, so a position in code
// that something imports is an entry point as surely as an export is, whether
// or not it has a name. In this title it never has: every import is by position.
// A position that resolves nowhere in the module is left out, and so is one in
// data, which is an object rather than an entry point.
std::vector<WalkSeed> SeedFromImports(const ctr::CroHeader& exporter,
                                      std::span<const ctr::CroSegmentTag> imported);

}  // namespace hexlith::domain::analysis

#endif  // HEXLITH_DOMAIN_ANALYSIS_EXPORT_SEED_H
