#ifndef HEXLITH_DOMAIN_ANALYSIS_MERGE_H
#define HEXLITH_DOMAIN_ANALYSIS_MERGE_H

#include "domain/analysis/provenance.h"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hexlith::domain::analysis {

// Regenerating an analysis table without destroying the work of whoever edited
// it by hand. ADR 0006 states the rule and this is the one place that applies
// it, so that a new pass cannot get it subtly wrong in its own way.
//
// A record whose source is derived from the image is produced again every time
// the pass runs, so the old copy is dropped and the new one takes its place. A
// record that came from a person or from a decompiler project cannot be
// produced again from anything, so it survives.
//
// Identity is the address and nothing else. A person who renamed the symbol at
// an address has decided what is at that address, so a record a pass derives
// for an address a preserved record already holds is dropped rather than added
// beside it. Keeping both would quietly undo the rename: the file would hold
// the old name and the new one, and whichever consumer read the first match
// would decide which of the two meant anything.
//
// What that loses is the automatic answer for that address, which is why the
// report counts it. A pass that suppresses a lot is either fighting a person's
// decisions or working from a wrong address, and both are worth noticing.

template<typename T>
concept AnalysisRecord = requires(const T& record) {
    { record.address } -> std::convertible_to<std::uint32_t>;
    { record.source } -> std::convertible_to<Provenance>;
};

struct MergeReport {
    // Records kept from the existing table because nothing can derive them
    // again.
    std::size_t preserved = 0;

    // Records dropped from the existing table because this pass derives them
    // again.
    std::size_t discarded = 0;

    // Records this pass contributed.
    std::size_t taken = 0;

    // Records this pass produced for an address a preserved record holds.
    std::size_t suppressed = 0;
};

template<AnalysisRecord T>
std::vector<T>
MergeRecords(const std::vector<T>& existing, const std::vector<T>& derived, MergeReport& report) {
    std::vector<T> merged;
    merged.reserve(existing.size() + derived.size());

    std::vector<std::uint32_t> preserved_addresses;

    for (const T& record : existing) {
        if (IsDerivedFromTheImage(record.source)) {
            ++report.discarded;
            continue;
        }
        merged.push_back(record);
        preserved_addresses.push_back(record.address);
        ++report.preserved;
    }

    std::ranges::sort(preserved_addresses);
    const auto duplicates = std::ranges::unique(preserved_addresses);
    preserved_addresses.erase(duplicates.begin(), duplicates.end());

    for (const T& record : derived) {
        if (std::ranges::binary_search(preserved_addresses, record.address)) {
            ++report.suppressed;
            continue;
        }
        merged.push_back(record);
        ++report.taken;
    }

    // The writers sort, so the order here is the order the two inputs happened
    // to be in and nothing depends on it.
    return merged;
}

}  // namespace hexlith::domain::analysis

#endif  // HEXLITH_DOMAIN_ANALYSIS_MERGE_H
