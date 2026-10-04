#ifndef HEXLITH_DOMAIN_ANALYSIS_CODE_WALK_H
#define HEXLITH_DOMAIN_ANALYSIS_CODE_WALK_H

#include "core/byte_reader.h"
#include "domain/analysis/tables.h"
#include "domain/arm/instruction_set.h"

#include <cstdint>
#include <span>
#include <vector>

namespace hexlith::domain::analysis {

// Telling code from data by following control flow from addresses that are known,
// rather than by decoding everything and keeping whatever decodes.
//
// The second approach does not work here and the measurement that says so is in
// docs/arm-decoder.md: 99.5 percent of the main executable's text segment decodes
// as valid ARM, and most of that segment is Thumb. In Thumb, 89.8 percent of all
// halfwords are instructions. Whether something decodes is almost no evidence
// about what it is.
//
// So the walk starts from the entry point and the export table, follows every
// branch whose target it can work out, and stops at anything it cannot. What it
// reaches is code and the instruction set it reached it in is known, because the
// path carried it. What it does not reach is not called data: it is called
// nothing, and the tables say nothing about it.

struct WalkSeed {
    std::uint32_t address = 0;
    arm::InstructionSet instruction_set = arm::InstructionSet::Arm;
    Provenance source = Provenance::CrsExport;
};

// The bytes to walk and where they live. For the main executable the base is the
// virtual address of the first byte, which the extended header fixes.
struct WalkImage {
    core::ByteSpan bytes;
    std::uint32_t base = 0;

    [[nodiscard]] bool Contains(std::uint32_t address) const {
        return address >= base && address - base < bytes.size();
    }
};

struct WalkOptions {
    // Whether a word loaded from a literal pool that looks like a code address is
    // followed as one.
    //
    // This is where the walk gets most of its reach. A virtual call goes through
    // a table in read-only data, and nothing in the instruction stream names the
    // functions in it; the only evidence they are functions is a word that points
    // at them with the interworking bit set correctly. The risk is the obvious
    // one, that a constant which is not an address is followed and data is walked
    // as code, which is why anything found this way is marked heuristic.
    bool follow_literal_pointers = true;

    // The range a word has to point into to be taken for a code address. Outside
    // it a word is left alone. Without this every small integer in a pool would
    // look like an address near the start of the segment.
    std::uint32_t code_begin = 0;
    std::uint32_t code_end = 0;

    // A range of data to sweep for words that look like the address of a
    // function, seeding the walk from each one. Empty by default; for the main
    // executable it is the read-only data and data segments.
    //
    // This is the difference between reaching a tenth of the text segment and
    // reaching three fifths of it, because a virtual call is the normal way this
    // title reaches its own code and the table it goes through is in read-only
    // data. Measured on the reference image:
    //
    //   exports alone                 10.5 percent, 2,639 functions, 0 conflicts
    //   with the sweep                64.0 percent, 8,765 functions, 2,659
    //   with the sweep and the filter  58.9 percent, 8,416 functions, 9
    //
    // A conflict is an address reached as ARM on one path and as Thumb on
    // another, so it is a contradiction and at least one of the two paths is
    // wrong. The middle row is what an unfiltered sweep costs: it buys five
    // percentage points of reach with three hundred times the contradictions.
    //
    // The filter is the prologue test below, and it applies to this sweep only. A
    // word a function loaded from its own literal pool is evidence from code; a
    // word that merely sits in data and looks like an address is a guess, and the
    // two do not deserve the same treatment.
    std::uint32_t pointer_scan_begin = 0;
    std::uint32_t pointer_scan_end = 0;
};

// Whether the instruction at an address looks like the first one of a function:
// a push, an adjustment of the stack pointer, or a store through it that writes
// back.
//
// A heuristic, and not a weak one. 26.5 percent of the words in the reference
// image's data that look like code addresses point at an instruction of this
// shape, and following only those is what keeps the sweep from walking data as
// code. It misses a leaf function that touches no stack, which is why it filters
// the blind sweep and nothing else.
[[nodiscard]] bool
LooksLikeAnEntryPoint(const WalkImage& image, std::uint32_t address, arm::InstructionSet set);

// What the walk did, so that a run can be compared with the one before it.
//
// These are counts of decisions and not of successes. A walk that stopped at ten
// thousand undefined words has either met a great deal of data or has a bug, and
// the only way to tell is to look at how the number moves when something changes.
struct WalkReport {
    std::size_t instructions = 0;
    std::size_t seeds = 0;
    std::size_t seeds_followed = 0;

    // Seeds refused before any decoding: outside the image, or at an address the
    // instruction set cannot start at.
    std::size_t seeds_outside_the_image = 0;
    std::size_t misaligned_seeds = 0;

    // A walk that ran into a word that is not an instruction. Expected: it is
    // what the end of a function followed by data looks like.
    std::size_t undefined_stops = 0;

    // A branch whose target is outside the image. In the main executable that
    // means a call into a module, which is the normal case rather than an error.
    std::size_t targets_outside_the_image = 0;

    // An address reached as ARM on one path and as Thumb on another. This should
    // not happen and every one of them is worth looking at: one of the two paths
    // is wrong.
    std::size_t instruction_set_conflicts = 0;

    std::size_t literal_pool_words = 0;
    std::size_t pointers_followed = 0;
    std::size_t pointers_ignored = 0;

    // From the sweep of data: words that point into the code range at a plausible
    // alignment, and how many of those were dropped for not pointing at anything
    // shaped like the start of a function.
    std::size_t words_in_range = 0;
    std::size_t words_without_a_prologue = 0;
};

struct WalkResult {
    // One region per run of bytes the walk decoded, merged where they are
    // adjacent and in the same instruction set.
    std::vector<Region> regions;

    // The entry points the walk found: the target of every call, with the
    // instruction set the call establishes. Extents are not known here, which is
    // the next pass.
    std::vector<FunctionRange> functions;

    // Words in data that hold the address of code.
    std::vector<CodePointer> pointers;

    WalkReport report;
};

WalkResult
WalkCode(const WalkImage& image, std::span<const WalkSeed> seeds, const WalkOptions& options);

}  // namespace hexlith::domain::analysis

#endif  // HEXLITH_DOMAIN_ANALYSIS_CODE_WALK_H
