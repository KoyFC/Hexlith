#ifndef HEXLITH_DOMAIN_ANALYSIS_FUNCTION_BOUNDS_H
#define HEXLITH_DOMAIN_ANALYSIS_FUNCTION_BOUNDS_H

#include "domain/analysis/code_walk.h"
#include "domain/analysis/tables.h"

#include <cstdint>
#include <span>
#include <vector>

namespace hexlith::domain::analysis {

// How far each function runs, and which other addresses the lifter may enter it
// at.
//
// The classification pass answers where functions start. It does not answer where
// they end, and the gap to the next one is not the answer: a function is followed
// by whatever the linker put next, which may be padding, a literal pool, or a
// function nothing has found yet.
//
// So the extent is measured the same way the entry points were: by following
// control flow. From the entry, every branch that stays inside the function is
// followed and the furthest instruction reached is the end. A call is not
// followed, because it comes back. A branch to another known entry point is not
// followed either, because that is a tail call and the function it reaches is its
// own.

struct BoundsReport {
    std::size_t entry_points = 0;
    std::size_t functions = 0;

    // Entry points that turned out to be inside another function, which makes
    // them the secondary entry points the lifter is allowed to dispatch to rather
    // than functions of their own.
    std::size_t alternate_entries = 0;

    // Functions whose extent could not be measured: the entry does not sit in a
    // region the walk classified as code, or it decodes to nothing.
    std::size_t without_an_extent = 0;

    // Functions that reach into another function's extent. A function that shares
    // a tail with another produces one, and so does a wrong entry point, so these
    // are worth reading rather than counting.
    std::size_t overlapping = 0;

    std::size_t bytes_covered = 0;

    // The largest extent measured, which is a useful smell test: a function of
    // forty kilobytes is a sign that two of them were run together.
    std::uint32_t largest = 0;
};

struct BoundsResult {
    std::vector<FunctionRange> functions;
    BoundsReport report;
};

// Measures the extent of every entry point, folding the ones that turn out to be
// inside another function into its alternate entries.
//
// The regions say which bytes the walk reached and in which encoding, which is
// what keeps the measurement from running off the end of a function into bytes
// nothing has claimed.
BoundsResult FindFunctionBounds(const WalkImage& image,
                                std::span<const FunctionRange> entries,
                                std::span<const Region> regions);

// The same, with an entry made for each run of executed instructions that
// starts outside every function, and the extents measured again with them.
//
// A run that starts outside every function the image gave or a guess found is
// code that something enters which nothing in the image names. Lifted code can
// be entered at any of its blocks (ADR 0025), and only a function is lifted, so
// the start of such a run becomes an entry of its own. Runs that start inside a
// known function are left to it.
//
// A run can hold more than one function, one ending where the next begins, and
// only the extent of the first is measured from its start; so this is repeated
// until every executed instruction is in some extent, or the rounds run out. An
// entry whose extent cannot be measured stays outside every extent, so what was
// added once is not added again. The entries are added to the vector, and
// `added` counts them.
BoundsResult FindFunctionBoundsCoveringObserved(const WalkImage& image,
                                                std::vector<FunctionRange>& entries,
                                                std::span<const Region> regions,
                                                std::span<const WalkSeed> observed,
                                                std::size_t& added);

}  // namespace hexlith::domain::analysis

#endif  // HEXLITH_DOMAIN_ANALYSIS_FUNCTION_BOUNDS_H
