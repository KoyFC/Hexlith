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

}  // namespace hexlith::domain::analysis

#endif  // HEXLITH_DOMAIN_ANALYSIS_FUNCTION_BOUNDS_H
