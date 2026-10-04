#ifndef HEXLITH_DOMAIN_ANALYSIS_PROVENANCE_H
#define HEXLITH_DOMAIN_ANALYSIS_PROVENANCE_H

#include <cstdint>
#include <optional>
#include <string_view>

namespace hexlith::domain::analysis {

// Where a record in an analysis table came from, carried on every record by
// ADR 0006.
//
// It is what makes a regeneration pass safe to run. Without it the automatic
// analysis either never runs again or silently discards the work of whoever
// renamed a function by hand.
enum class Provenance : std::uint8_t {
    // The named export table of a CRO-family module, static.crs included.
    CrsExport,

    // The extended header: the entry point and the segment bounds.
    ExHeader,

    // A relocation table of a CRO module, which names an address without
    // naming what is at it.
    Relocation,

    // Imported from a decompiler project.
    Ghidra,

    // Produced by this project's own analysis.
    Heuristic,

    // A decision a person made.
    Manual,
};

std::string_view Describe(Provenance source);

std::optional<Provenance> ParseProvenance(std::string_view text);

// Whether a regeneration pass may replace a record carrying this source.
//
// The rule is not about how much a source is trusted, it is about whether the
// record can be produced again from the cartridge. Everything derived from the
// image can, so rerunning the analysis reproduces it and overwriting it loses
// nothing. A name from a decompiler project or from a person cannot be derived
// from anything in the image, so a pass that dropped it would be destroying
// the only copy.
[[nodiscard]] bool IsDerivedFromTheImage(Provenance source);

}  // namespace hexlith::domain::analysis

#endif  // HEXLITH_DOMAIN_ANALYSIS_PROVENANCE_H
