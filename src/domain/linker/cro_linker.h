#ifndef HEXLITH_DOMAIN_LINKER_CRO_LINKER_H
#define HEXLITH_DOMAIN_LINKER_CRO_LINKER_H

#include "core/byte_reader.h"
#include "core/result.h"
#include "domain/ctr/cro.h"
#include "domain/ctr/cro_link_tables.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hexlith::domain::linker {

// Linking CRO modules the way the console's ldr:ro service does, without a
// console: given the modules and the addresses chosen for them, work out the
// value every relocated word ends up holding.
//
// The source for every step is azahar/src/core/hle/service/ldr_ro/
// cro_helper.cpp, and the function names in the comments below are its. What
// the service does to guest memory, this does to a list of words. Writing the
// words into an image or into guest memory is a separate and trivial step, so
// the same answer serves the analysis, a comparison with a memory dump, and in
// time the service itself.
//
// docs/cro-linking.md has the whole procedure and what it was checked against.

// A module as the reader produced it, with everything the linker consults.
struct CroModule {
    ctr::CroHeader header;
    std::vector<ctr::CroNamedExport> named_exports;
    std::vector<ctr::CroIndexedExport> indexed_exports;
    ctr::CroLinkTables tables;
};

core::Result<CroModule> ReadCroModule(core::ByteSpan file);

// The three addresses LoadCRO is given for a module: where the file is mapped,
// and the buffers its data and BSS go to. Source: 3dbrew RO:LoadExeCRO, request
// words 2, 4 and 7. The game chooses all three.
struct Placement {
    std::uint32_t module_address = 0;
    std::uint32_t data_address = 0;
    std::uint32_t bss_address = 0;
};

// One word the loader has written.
struct LinkedWord {
    // Which word: a position in the segments of the module that holds it.
    ctr::CroSegmentTag target;

    // Where the word is once the module is in place.
    //
    // For a word in the data segment that is in the data buffer. The console's
    // loader patches internal relocations into data while the data is still in
    // the file, and the game copies it to the buffer afterwards, so the word
    // ends up here either way. ApplyInternalRelocations has the detail.
    std::uint32_t address = 0;

    // What the loader wrote, or nothing for a relocation of type Nothing,
    // which leaves the word as the file has it.
    std::optional<std::uint32_t> value;
};

// What an external relocation was last resolved against.
struct Resolution {
    // Whether an import has supplied the batch. Until one does, the batch holds
    // the module's OnUnresolved address, as ResetExternalRelocations leaves it.
    bool resolved = false;

    // The loaded module that supplied it, as an index into Linker::Modules.
    std::size_t exporter = 0;

    // The address it supplied, before the addend of each relocation.
    std::uint32_t symbol_address = 0;
};

// A module the linker has placed and linked.
struct LoadedModule {
    const CroModule* module = nullptr;
    Placement placement;

    // The address of each segment, indexed like the segment table. A segment
    // with no size and no offset is left at zero, as RebaseSegmentTable leaves
    // it. For the static module these are the segment offsets as they stand,
    // which are virtual addresses already.
    std::vector<std::uint32_t> segment_addresses;

    // One per entry of the module's internal and external relocation tables,
    // in table order.
    std::vector<LinkedWord> internal;
    std::vector<LinkedWord> external;

    // One per external relocation. Every entry of a batch carries the batch's
    // resolution, so a caller need not find the batch an entry belongs to.
    std::vector<Resolution> resolutions;

    [[nodiscard]] const std::string& Name() const {
        return module->header.name;
    }
};

// The modules linked so far, in the order the service keeps them: the static
// module first, then each loaded module in the order it was loaded. That order
// is the one a named import is searched in.
//
// Every module given to the linker must outlive it.
//
// Unloading is not here. An unload resets the imports that pointed into the
// module, so linking only the modules that are resident, in the order they were
// loaded, reaches the same state that loading and unloading reached; that is
// all the analysis and the memory comparison need. The ldr:ro service will need
// the real thing.
class Linker {
public:
    // Starts from the static module, as LoadCRS does: its relocations are
    // reset to its own OnUnresolved address, and nothing is linked yet.
    static core::Result<Linker> FromStaticModule(const CroModule& static_module);

    // Places a module and links it, as LoadCRO does: Rebase, then Link, then
    // registration at the end of the list. Answers the module's index in
    // Modules().
    //
    // Refused: a module with static anonymous symbols, which no title is known
    // to use and nothing here could be checked against, and a relocation of a
    // type Azahar does not implement. The reference title has neither.
    core::Result<std::size_t> Load(const CroModule& module, const Placement& placement);

    [[nodiscard]] const std::vector<LoadedModule>& Modules() const {
        return modules_;
    }

private:
    explicit Linker(std::vector<LoadedModule> modules) : modules_(std::move(modules)) {}

    std::vector<LoadedModule> modules_;
};

// The value a relocation of a given type writes, or why it writes nothing that
// can be known. Source: ApplyRelocation in cro_helper.cpp, which implements the
// first four types and stops on the rest.
//
// Nothing writes nothing, which is answered as an empty optional.
core::Result<std::optional<std::uint32_t>> RelocatedValue(ctr::CroRelocationType type,
                                                          bool type_known,
                                                          std::uint32_t symbol_address,
                                                          std::uint32_t addend,
                                                          std::uint32_t word_address);

// A copy of a loadable module's file with every linked word written into it, at
// the file offset of its segment. That is the module as it sits in memory once
// loaded, apart from data, which the game copies to its buffer, and BSS, which
// is not in the file.
//
// The static module has no file to write into, since its segments are the main
// executable's, and is refused.
core::Result<std::vector<std::uint8_t>> LinkedImage(const LoadedModule& loaded,
                                                    core::ByteSpan file);

}  // namespace hexlith::domain::linker

#endif  // HEXLITH_DOMAIN_LINKER_CRO_LINKER_H
