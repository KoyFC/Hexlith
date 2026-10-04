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

// How many entries each table of the module declares, as plain header fields.
// The entries of the tables the loader links with are read by
// ReadCroLinkTables in cro_link_tables.h.
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

// A position inside a module, as the format packs it: the segment index in the
// low four bits and the byte offset within that segment in the other 28.
//
// This is how a CRO refers to its own contents, and it is the reason a module
// can be loaded anywhere. Nothing in it names an address; everything names a
// segment and a distance into it, and the loader turns that into an address
// once it has decided where the segment goes.
struct CroSegmentTag {
    std::uint32_t raw = 0;

    [[nodiscard]] std::uint32_t SegmentIndex() const {
        return raw & cro::SegmentTagIndexMask;
    }

    [[nodiscard]] std::uint32_t OffsetInSegment() const {
        return raw >> cro::SegmentTagOffsetShift;
    }
};

// A symbol a module publishes under a name.
struct CroNamedExport {
    std::string name;
    CroSegmentTag tag;

    // Where the tag lands: the segment's own offset plus the distance into it.
    // In a loadable module that is an offset into the file, and in the static
    // module it is a virtual address, for the same reason the segment offsets
    // are. CroSegment says why.
    std::size_t position = 0;
};

// A symbol a module publishes without a name, found by its place in the table.
struct CroIndexedExport {
    std::size_t index = 0;
    CroSegmentTag tag;
    std::size_t position = 0;
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

// The named export table, which is the only source of symbol names this
// project has. The static module publishes 461 of them.
//
// A tag whose segment index is out of range, or whose offset reaches past the
// end of its segment, is a failure. That is the same test the console's loader
// applies: SegmentTagToAddress in cro_helper.cpp returns address zero for
// either, and a symbol at address zero is not a symbol.
//
// The names come out in table order rather than sorted. The export tree beside
// the table exists to make lookup fast on the console and is not read: a tool
// that reads every entry once gains nothing from an index.
core::Result<std::vector<CroNamedExport>> ReadCroNamedExports(core::ByteSpan module,
                                                              const CroHeader& header);

// The indexed export table. These have no names, so they are no use as symbols,
// but each one is an address the module publishes for something to call, which
// makes it an entry point worth knowing about.
core::Result<std::vector<CroIndexedExport>> ReadCroIndexedExports(core::ByteSpan module,
                                                                  const CroHeader& header);

// Where a segment tag lands: the segment's own offset plus the distance into
// it, in whatever space the segment offsets are in.
//
// The two refusals are the two the console's loader makes: SegmentTagToAddress
// in cro_helper.cpp answers address zero for a segment index past the end of
// the table and for an offset that reaches the end of its segment, and the
// loader treats address zero as nothing there. The description names the tag
// in the failure.
core::Result<std::size_t>
ResolveCroSegmentTag(const CroHeader& header, CroSegmentTag tag, std::string_view what);

// Whether a file out of the RomFS is a module of the CRO family at all,
// decided by the magic the format puts at 0x80 rather than by the extension.
// static.crs is a CRO and does not end in .cro, and .crr/static.crr is not a
// module although it looks like one.
bool LooksLikeCroModule(core::ByteSpan file);

}  // namespace hexlith::domain::ctr

#endif  // HEXLITH_DOMAIN_CTR_CRO_H
