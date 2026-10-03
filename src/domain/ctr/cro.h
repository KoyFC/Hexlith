#ifndef HEXLITH_DOMAIN_CTR_CRO_H
#define HEXLITH_DOMAIN_CTR_CRO_H

#include "core/byte_reader.h"
#include "core/result.h"
#include "domain/ctr/layout.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hexlith::domain::ctr {

enum class CroSegmentType : std::uint8_t { Code = 0, ReadOnlyData = 1, Data = 2, Bss = 3 };

std::string_view Describe(CroSegmentType type);

struct CroSegment {
    std::size_t index = 0;

    // A file offset in a loadable module. In the static module these are
    // absolute virtual addresses, because the segments it describes are
    // already mapped, so this field cannot be bounds checked against the
    // module for that one.
    std::size_t offset = 0;
    std::size_t size = 0;

    // The raw value is kept beside the decoded one so that a type the format
    // gains later is reported rather than silently treated as code.
    std::uint32_t raw_type = 0;
    CroSegmentType type = CroSegmentType::Code;
    bool type_known = false;
};

// How many entries each table of the module declares.
//
// Phase 1 records the counts and does not walk the tables. The counts are
// plain header fields and need no interpretation; the entry layouts of the
// relocation and import tables are not recorded in docs/formats/ yet, and
// nothing is read from a format this project has not written down. They are
// needed by the lifter and by the ldr:ro reimplementation, which is where
// they will be verified and documented.
struct CroTableCounts {
    std::size_t segments = 0;
    std::size_t named_exports = 0;
    std::size_t indexed_exports = 0;
    std::size_t export_tree_nodes = 0;
    std::size_t import_modules = 0;
    std::size_t external_relocations = 0;
    std::size_t named_imports = 0;
    std::size_t indexed_imports = 0;
    std::size_t anonymous_imports = 0;
    std::size_t static_anonymous_symbols = 0;
    std::size_t internal_relocations = 0;
    std::size_t static_relocations = 0;
};

struct CroHeader {
    // A module the loader has already relocated reports FIXD rather than
    // CRO0. Hexlith reads modules straight out of the RomFS, so finding one
    // means the file came from somewhere else.
    bool fixed_up = false;

    std::string name;
    std::size_t declared_file_size = 0;
    std::size_t bss_size = 0;
    std::size_t fixed_size = 0;

    std::vector<CroSegment> segments;
    CroTableCounts counts;

    std::size_t export_strings_size = 0;
    std::size_t import_strings_size = 0;

    [[nodiscard]] bool IsStaticModule() const {
        return name == cro::StaticModuleName;
    }

    [[nodiscard]] std::size_t SegmentBytes(CroSegmentType wanted) const;
};

// Reads the header, the segment table and the module name.
//
// The segment offsets of the static module are virtual addresses and not file
// offsets, so they are reported as read and not checked against the file. On
// the reference image they match the extended header's code set addresses,
// which is a cross-check that both were read correctly.
core::Result<CroHeader> ParseCroHeader(core::ByteSpan module);

// Whether a file out of the RomFS is a module of the CRO family at all,
// decided by the magic the format puts at 0x80 rather than by the extension.
// static.crs is a CRO and does not end in .cro, and .crr/static.crr is not a
// module although it looks like one.
bool LooksLikeCroModule(core::ByteSpan file);

}  // namespace hexlith::domain::ctr

#endif  // HEXLITH_DOMAIN_CTR_CRO_H
