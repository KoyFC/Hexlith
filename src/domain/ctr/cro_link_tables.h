#ifndef HEXLITH_DOMAIN_CTR_CRO_LINK_TABLES_H
#define HEXLITH_DOMAIN_CTR_CRO_LINK_TABLES_H

#include "core/byte_reader.h"
#include "core/result.h"
#include "domain/ctr/cro.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hexlith::domain::ctr {

// The tables the loader links a module with: what the module patches inside
// itself, what it asks of other modules, and where each answer is written.
//
// Everything here is read as the file states it. Positions stay segment tags
// and nothing is turned into an address, because an address only exists once
// somebody has decided where the module goes. That is the linker's business.
//
// docs/formats/cro0.md gives the layout of every entry with its source.

// What a relocation does to the word it patches. Source: RelocationType in
// azahar/src/core/hle/service/ldr_ro/cro_helper.h.
//
// Azahar implements Nothing and the three address forms and stops on the
// others. Every one of the 66,019 relocations in the reference title is an
// AbsoluteAddress, so the branch forms have never been needed here either.
enum class CroRelocationType : std::uint8_t {
    Nothing = 0,
    AbsoluteAddress = 2,
    RelativeAddress = 3,
    ThumbBranch = 10,
    ArmBranch = 28,
    ModifyArmBranch = 29,
    AbsoluteAddress2 = 38,
    AlignedRelativeAddress = 42,
};

std::string_view Describe(CroRelocationType type);

// One entry of the external or the static relocation table.
//
// These come in batches: consecutive entries that all receive the same symbol,
// the last one marked. An import names the first entry of its batch, and the
// loader patches every entry up to the mark with whatever the import resolved
// to.
struct CroBatchRelocation {
    CroSegmentTag target;

    // The raw byte is kept beside the decoded one, for the same reason a
    // segment's is: a type the format gains later is reported rather than
    // mistaken for a known one.
    std::uint8_t raw_type = 0;
    CroRelocationType type = CroRelocationType::Nothing;
    bool type_known = false;

    bool batch_end = false;

    // Set by the loader once it has applied the batch. A module read from the
    // RomFS has never been through a loader, and on the reference image the
    // flag is clear everywhere.
    bool batch_resolved = false;

    std::uint32_t addend = 0;
};

// One entry of the internal relocation table: a word in this module that has
// to hold the address of something else in this module.
//
// The thing pointed at is named by a segment and nothing finer. The loader
// writes the segment's address plus the addend, so the addend carries the
// distance into the segment.
struct CroInternalRelocation {
    CroSegmentTag target;
    std::uint8_t raw_type = 0;
    CroRelocationType type = CroRelocationType::Nothing;
    bool type_known = false;
    std::uint32_t symbol_segment = 0;
    std::uint32_t addend = 0;
};

// A run of consecutive entries in a relocation table, as indices.
struct CroBatch {
    std::size_t first = 0;
    std::size_t count = 0;
};

// An import by name, searched for in every module the loader can link with.
struct CroNamedImport {
    std::string name;
    CroBatch batch;
};

// An import by the exporting module's indexed export number.
struct CroIndexedImport {
    std::uint32_t export_index = 0;
    CroBatch batch;
};

// An import by position. The tag is a position in the exporting module, in
// that module's segments, which is why it cannot be checked here.
struct CroAnonymousImport {
    CroSegmentTag position;
    CroBatch batch;
};

// A module this one imports from, and what it imports by index and by
// position. Named imports do not belong to a module: the loader looks for the
// name everywhere.
struct CroImportModule {
    std::string name;
    std::vector<CroIndexedImport> indexed;
    std::vector<CroAnonymousImport> anonymous;
};

// A position this module hands to the static module, with the batch of the
// static relocation table that receives it. Azahar notes that no game is known
// to use these, and the reference title declares none.
struct CroStaticAnonymousSymbol {
    CroSegmentTag position;
    CroBatch batch;
};

// The four segment tags in the header that name places to call rather than
// tables. A tag that resolves nowhere means there is none; the title writes
// 0xFFFFFFFF for that, which has a segment index no module declares.
struct CroHooks {
    CroSegmentTag unknown;
    CroSegmentTag on_load;
    CroSegmentTag on_exit;
    CroSegmentTag on_unresolved;
};

struct CroLinkTables {
    std::vector<CroImportModule> import_modules;
    std::vector<CroNamedImport> named_imports;
    std::vector<CroBatchRelocation> external_relocations;
    std::vector<CroInternalRelocation> internal_relocations;
    std::vector<CroStaticAnonymousSymbol> static_anonymous_symbols;
    std::vector<CroBatchRelocation> static_relocations;
    CroHooks hooks;
};

// Reads every table the loader links with.
//
// The checks are the ones RebaseHeader and the Rebase* functions in
// cro_helper.cpp make before the console's loader touches anything: the table
// offsets appear in a fixed order, each table lies inside the file, each name
// lies inside its string table, each import module's slices lie inside the
// table they slice, and every relocation patches a position inside its own
// segment.
//
// One check is stricter than the loader's. A batch offset has to land on the
// first entry of a batch, where the loader only asks that it land somewhere
// in the table. An offset into the middle of a batch would patch half of it,
// and every batch offset in the reference title is the first entry of one.
core::Result<CroLinkTables> ReadCroLinkTables(core::ByteSpan module, const CroHeader& header);

}  // namespace hexlith::domain::ctr

#endif  // HEXLITH_DOMAIN_CTR_CRO_LINK_TABLES_H
