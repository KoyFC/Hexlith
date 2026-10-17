#ifndef HEXLITH_DOMAIN_LINKER_MODULE_IMAGE_H
#define HEXLITH_DOMAIN_LINKER_MODULE_IMAGE_H

#include "core/result.h"
#include "domain/linker/cro_linker.h"

#include <cstdint>
#include <span>

namespace hexlith::domain::linker {

// What the loader changes in a module's own bytes besides the words it
// relocates. The Linker works out those words; these work out the rest, on a
// copy of the module as it sits in memory, so that the ldr:ro service can
// leave the module exactly as the console's loader does.
//
// The source for every step is azahar/src/core/hle/service/ldr_ro/
// cro_helper.cpp, whose function names the comments use. docs/cro-linking.md
// has the order the steps run in.

// The sizes of the data and BSS buffers LoadCRO is given, which the segments
// they receive must fit in.
struct BufferSizes {
    std::uint32_t data = 0;
    std::uint32_t bss = 0;
};

// The header and the tables as Rebase leaves them, for a module mapped at
// module_address. Every offset in the header becomes an address, and so does
// every offset inside the tables that point at other tables: the names of
// exports and imported modules, the slices of the import tables each imported
// module owns, and the relocation batch each import resolves. The segment
// table gets the segments' addresses, data and BSS at the buffers the
// placement names, except in the static module, whose segment offsets are
// already addresses. The first entry of every batch of external relocations is
// marked unresolved, as ResetExternalRelocations leaves it.
//
// Refused, as RebaseHeader refuses it: an image without the CRO0 magic, one
// already in a module list or already fixed, offsets out of order, and a table
// that reaches past the image.
core::Status RebaseModuleImage(std::span<std::uint8_t> bytes,
                               std::uint32_t module_address,
                               const Placement& placement,
                               const BufferSizes& sizes,
                               bool is_static);

// Fix, run on an image Rebase has been through: at a fix level above zero the
// magic becomes FIXD and the tables from the level's barrier on are emptied,
// each pointed at the end of what is kept. Answers how much of the module
// stays mapped, the end of the last table kept rounded up to a page, which is
// also written into the header.
core::Result<std::uint32_t> FixModuleImage(std::span<std::uint8_t> bytes,
                                           std::uint32_t module_address,
                                           std::uint32_t fix_level);

// Unrebase, as UnloadCRO runs it on a module it is about to unmap: every
// address Rebase wrote into the header and the tables becomes an offset again,
// the module leaves its list and its fixed size goes back to zero. The magic
// stays as Fix left it.
//
// Only the tables the header still counts are touched, which after Fix is
// what the fix level kept. As in Azahar, a segment's address loses the
// module's address whatever buffer it points into, and a BSS segment's offset
// becomes zero.
void UnrebaseModuleImage(std::span<std::uint8_t> bytes,
                         std::uint32_t module_address,
                         bool is_static);

}  // namespace hexlith::domain::linker

#endif  // HEXLITH_DOMAIN_LINKER_MODULE_IMAGE_H
