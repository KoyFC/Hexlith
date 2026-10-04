#ifndef HEXLITH_TESTS_SUPPORT_SYNTHETIC_CRO_H
#define HEXLITH_TESTS_SUPPORT_SYNTHETIC_CRO_H

#include "domain/ctr/cro.h"
#include "domain/ctr/cro_link_tables.h"

#include <cstddef>
#include <cstdint>
#include <map>
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

// One relocation entry. The position is where the entry patches, in the
// module's own segments for an external or internal relocation and in the
// static module's for a static one.
struct SyntheticCroRelocation {
    std::uint32_t segment_index = 0;
    std::uint32_t offset_in_segment = 0;
    std::uint8_t type = static_cast<std::uint8_t>(domain::ctr::CroRelocationType::AbsoluteAddress);
    std::uint32_t addend = 0;

    // Internal relocations only: the segment the patched word points into.
    std::uint8_t symbol_segment = 0;
};

// One import, with the batch of relocations it resolves. Which of the first
// three fields means anything depends on the table the import is written to:
// the name for a named import, the export index for an indexed one, and the
// position in the exporting module for an anonymous one or a static anonymous
// symbol.
struct SyntheticCroImport {
    std::string name;
    std::uint32_t export_index = 0;
    std::uint32_t segment_index = 0;
    std::uint32_t offset_in_segment = 0;
    std::vector<SyntheticCroRelocation> batch;
};

struct SyntheticCroImportModule {
    std::string name;
    std::vector<SyntheticCroImport> indexed;
    std::vector<SyntheticCroImport> anonymous;
};

// The value the title writes into a hook field that names nothing. Its segment
// index is fifteen, which no module declares.
constexpr std::uint32_t NoSegmentTag = 0xFFFFFFFF;

struct SyntheticCroSpec {
    std::string name = "DllTest";

    // The static module is named with pipe characters and carries virtual
    // addresses rather than file offsets.
    bool static_module = false;

    // A module the loader has already relocated reports FIXD.
    bool fixed_up = false;

    std::vector<SyntheticCroSegment> segments;

    // The bytes of a segment, by its index, for a test that needs real
    // instructions in it. A segment not named here gets a filler pattern, and
    // contents shorter than the segment leave the rest zero.
    std::map<std::size_t, std::vector<std::uint8_t>> segment_contents;

    // Laid out as real entries, with the string table they point into. An
    // empty list means the count below is declared and the table is left
    // unwritten, which is what a test of the count fields wants and what a
    // test of the entries does not.
    std::vector<SyntheticCroExport> named_exports;
    std::vector<SyntheticCroExport> indexed_exports;

    // The tables the loader links with. The external relocation table is the
    // batches of these imports laid end to end, named imports first, then each
    // module's indexed and anonymous imports in order, the same order the
    // title's own modules use. The static relocation table is built the same
    // way from the static anonymous symbols.
    std::vector<SyntheticCroImport> named_imports;
    std::vector<SyntheticCroImportModule> import_modules;
    std::vector<SyntheticCroRelocation> internal_relocations;
    std::vector<SyntheticCroImport> static_anonymous_symbols;

    std::uint32_t unknown_hook = NoSegmentTag;
    std::uint32_t on_load_hook = NoSegmentTag;
    std::uint32_t on_exit_hook = NoSegmentTag;
    std::uint32_t on_unresolved_hook = NoSegmentTag;

    // The counts of the tables the lists above leave empty. Such a table is
    // declared with this many entries and given room in the file, but its
    // entries are left zero, which is what a test of the count fields wants
    // and what a test of the entries does not.
    domain::ctr::CroTableCounts counts;
};

std::vector<std::uint8_t> BuildSyntheticCro(const SyntheticCroSpec& spec);

// The imports of each kind, spelt out. Each takes the batch of relocations the
// import resolves.
SyntheticCroImport AnonymousImport(std::uint32_t segment_index,
                                   std::uint32_t offset_in_segment,
                                   std::vector<SyntheticCroRelocation> batch);

SyntheticCroImport NamedImport(std::string name, std::vector<SyntheticCroRelocation> batch);

SyntheticCroImport IndexedImport(std::uint32_t export_index,
                                 std::vector<SyntheticCroRelocation> batch);

SyntheticCroImportModule ImportsFrom(std::string name, std::vector<SyntheticCroImport> anonymous);

// A module shaped like one of the title's: four segments, almost no exports
// and a great many relocations and imports, which is the shape
// docs/formats/cro0.md measured.
SyntheticCroSpec DefaultCroSpec();

// The CRS, whose segment offsets are the virtual addresses of the main
// executable's segments.
SyntheticCroSpec StaticModuleSpec();

}  // namespace hexlith::test

#endif  // HEXLITH_TESTS_SUPPORT_SYNTHETIC_CRO_H
