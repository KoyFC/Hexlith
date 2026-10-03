#ifndef HEXLITH_TESTS_SUPPORT_SYNTHETIC_CRO_H
#define HEXLITH_TESTS_SUPPORT_SYNTHETIC_CRO_H

#include "domain/ctr/cro.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Builds CRO modules in memory.
//
// The header is a flat array of 32-bit fields whose table offsets and counts
// alternate, which is exactly the shape that makes an off-by-one easy. A
// builder writing the same fields a parser reads is the only way to test that
// without a module from the cartridge.
namespace hexlith::test {

struct SyntheticCroSegment {
    domain::ctr::CroSegmentType type = domain::ctr::CroSegmentType::Code;
    std::size_t size = 0;

    // Used in place of the file offset when the module is the static one,
    // whose segments are already mapped.
    std::uint32_t virtual_address = 0;
};

struct SyntheticCroSpec {
    std::string name = "DllTest";

    // The static module is named with pipe characters and carries virtual
    // addresses rather than file offsets.
    bool static_module = false;

    // A module the loader has already relocated reports FIXD.
    bool fixed_up = false;

    std::vector<SyntheticCroSegment> segments;

    // Written into the header as declared. Phase 1 reads the counts without
    // walking the tables, so only the named export table is laid out for
    // real.
    domain::ctr::CroTableCounts counts;
};

std::vector<std::uint8_t> BuildSyntheticCro(const SyntheticCroSpec& spec);

// A module shaped like one of the title's: four segments, almost no exports
// and a great many relocations and imports, which is the shape
// docs/formats/cro0.md measured.
SyntheticCroSpec DefaultCroSpec();

// The CRS, whose segment offsets are the virtual addresses of the main
// executable's segments.
SyntheticCroSpec StaticModuleSpec();

}  // namespace hexlith::test

#endif  // HEXLITH_TESTS_SUPPORT_SYNTHETIC_CRO_H
