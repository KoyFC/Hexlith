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

// One entry of an export table. A named export carries a name and a position;
// an indexed export carries only a position, and what identifies it is where it
// sits in the table.
struct SyntheticCroExport {
    std::string name;
    std::uint32_t segment_index = 0;
    std::uint32_t offset_in_segment = 0;
};

struct SyntheticCroSpec {
    std::string name = "DllTest";

    // The static module is named with pipe characters and carries virtual
    // addresses rather than file offsets.
    bool static_module = false;

    // A module the loader has already relocated reports FIXD.
    bool fixed_up = false;

    std::vector<SyntheticCroSegment> segments;

    // Laid out as real entries, with the string table they point into. An
    // empty list means the count below is declared and the table is left
    // unwritten, which is what a test of the count fields wants and what a
    // test of the entries does not.
    std::vector<SyntheticCroExport> named_exports;
    std::vector<SyntheticCroExport> indexed_exports;

    // Written into the header as declared. The tables other than the export
    // tables are not laid out, because nothing reads them yet.
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
