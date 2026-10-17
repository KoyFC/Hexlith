#include "domain/linker/cro_linker.h"

#include <format>
#include <string_view>
#include <utility>

namespace hexlith::domain::linker {
namespace {

using ctr::CroRelocationType;
using ctr::CroSegmentTag;
using ctr::CroSegmentType;

// The named import ApplyExitRelocations treats specially, and the export it is
// resolved against instead. A module's static destructors are registered
// through it, and the static module provides the registry.
constexpr std::string_view AtexitImport = "__aeabi_atexit";
constexpr std::string_view AtexitExport = "nnroAeabiAtexit_";

// Where a tag lands in a placed module, or nothing, which is the answer
// SegmentTagToAddress gives as address zero: an index past the segment table,
// or an offset that reaches the end of its segment.
std::optional<std::uint32_t> TagAddress(const LoadedModule& loaded, CroSegmentTag tag) {
    const std::vector<ctr::CroSegment>& segments = loaded.module->header.segments;
    const std::uint32_t index = tag.SegmentIndex();
    if (index >= segments.size() || tag.OffsetInSegment() >= segments.at(index).size) {
        return std::nullopt;
    }
    return loaded.segment_addresses.at(index) + tag.OffsetInSegment();
}

// RebaseSegmentTable: data and BSS go to the buffers the caller gave, the
// other segments move with the module, and a segment at offset zero stays at
// zero. A data or BSS segment of size zero is left as it is in the file.
std::vector<std::uint32_t> RebaseSegments(const ctr::CroHeader& header,
                                          const Placement& placement) {
    std::vector<std::uint32_t> addresses;
    addresses.reserve(header.segments.size());
    for (const ctr::CroSegment& segment : header.segments) {
        const auto offset = static_cast<std::uint32_t>(segment.offset);
        const bool data = segment.type_known && segment.type == CroSegmentType::Data;
        const bool bss = segment.type_known && segment.type == CroSegmentType::Bss;
        if (data || bss) {
            const std::uint32_t buffer = data ? placement.data_address : placement.bss_address;
            addresses.push_back(segment.size != 0 ? buffer : offset);
        } else {
            addresses.push_back(offset != 0 ? offset + placement.module_address : 0);
        }
    }
    return addresses;
}

// The address of a named export, or nothing. FindExportNamedSymbol searches
// the export tree, and a module with no tree finds nothing whatever its export
// table holds; the same holds here. Hexlith searches the table itself, which
// answers the same for a tree that indexes it correctly.
std::optional<std::uint32_t> FindNamedExport(const LoadedModule& loaded, std::string_view name) {
    const CroModule& module = *loaded.module;
    if (module.header.counts.export_tree_nodes == 0) {
        return std::nullopt;
    }
    for (const ctr::CroNamedExport& entry : module.named_exports) {
        if (entry.name == name) {
            return TagAddress(loaded, entry.tag);
        }
    }
    return std::nullopt;
}

// Writes one external relocation of a module with a symbol, as ApplyRelocation
// does from ApplyRelocationBatch and ResetExternalRelocations.
core::Status WriteExternal(LoadedModule& loaded, std::size_t index, std::uint32_t symbol) {
    const ctr::CroBatchRelocation& entry = loaded.module->tables.external_relocations.at(index);
    const std::optional<std::uint32_t> address = TagAddress(loaded, entry.target);
    if (!address) {
        return core::Error{
            std::format("external relocation {} of {} patches nowhere", index, loaded.Name())};
    }

    const core::Result<std::optional<std::uint32_t>> value =
        RelocatedValue(entry.type, entry.type_known, symbol, entry.addend, *address);
    if (!value) {
        return value.Failure().WithContext(
            std::format("external relocation {} of {}", index, loaded.Name()));
    }

    LinkedWord& word = loaded.external.at(index);
    word.target = entry.target;
    word.address = *address;
    word.value = value.Value();
    return {};
}

// ApplyRelocationBatch: every entry of the batch receives the symbol, and the
// batch is marked resolved. A symbol at address zero is refused, because it is
// what SegmentTagToAddress answers for a position that is not there.
core::Status ApplyBatch(LoadedModule& loaded,
                        const ctr::CroBatch& batch,
                        std::uint32_t symbol,
                        std::size_t exporter) {
    if (symbol == 0) {
        return core::Error{std::format("{} imports a symbol at address zero, which the loader "
                                       "refuses",
                                       loaded.Name())};
    }
    for (std::size_t index = batch.first; index < batch.first + batch.count; ++index) {
        const core::Status written = WriteExternal(loaded, index, symbol);
        if (!written) {
            return written;
        }
        loaded.resolutions.at(index) =
            Resolution{.resolved = true, .exporter = exporter, .symbol_address = symbol};
    }
    return {};
}

bool BatchResolved(const LoadedModule& loaded, const ctr::CroBatch& batch) {
    return loaded.resolutions.at(batch.first).resolved;
}

std::uint32_t UnresolvedAddress(const LoadedModule& loaded) {
    return TagAddress(loaded, loaded.module->tables.hooks.on_unresolved).value_or(0);
}

// The fix levels from which a module's import tables, and its export tables,
// are emptied. Source: FIX_BARRIERS in cro_helper.cpp.
constexpr std::uint32_t ImportsEmptiedFrom = 2;
constexpr std::uint32_t ExportsEmptiedFrom = 3;

bool KeepsImports(const LoadedModule& loaded) {
    return loaded.options.fix_level < ImportsEmptiedFrom;
}

// ApplyRelocationBatch with reset set: every entry of the batch receives the
// module's OnUnresolved address, and the batch is marked unresolved. Address
// zero is allowed here.
core::Status ResetBatch(LoadedModule& loaded, const ctr::CroBatch& batch) {
    const std::uint32_t unresolved = UnresolvedAddress(loaded);
    for (std::size_t index = batch.first; index < batch.first + batch.count; ++index) {
        const core::Status written = WriteExternal(loaded, index, unresolved);
        if (!written) {
            return written;
        }
        loaded.resolutions.at(index) = Resolution{};
    }
    return {};
}

// Resets the batch of every entry, an import of any of the three kinds.
template<typename Entries>
core::Status ResetEach(LoadedModule& loaded, const Entries& entries) {
    for (const auto& entry : entries) {
        const core::Status reset = ResetBatch(loaded, entry.batch);
        if (!reset) {
            return reset;
        }
    }
    return {};
}

// What a module imports by index and by position from one module it names.
core::Status ResetImportsFrom(LoadedModule& loaded, const ctr::CroImportModule& imported) {
    core::Status indexed = ResetEach(loaded, imported.indexed);
    if (!indexed) {
        return indexed;
    }
    return ResetEach(loaded, imported.anonymous);
}

// ResetImportNamedSymbol, ResetImportIndexedSymbol and
// ResetImportAnonymousSymbol: every import batch of the module, resolved or
// not.
core::Status ResetOwnImports(LoadedModule& loaded) {
    if (!KeepsImports(loaded)) {
        return {};
    }
    core::Status named = ResetEach(loaded, loaded.module->tables.named_imports);
    if (!named) {
        return named;
    }
    for (const ctr::CroImportModule& imported : loaded.module->tables.import_modules) {
        const core::Status reset = ResetImportsFrom(loaded, imported);
        if (!reset) {
            return reset;
        }
    }
    return {};
}

// ResetExportNamedSymbol: a resolved named import of the target is reset if
// the departing module exports the name.
core::Status ResetNamesExportedBy(LoadedModule& target, const LoadedModule& departing) {
    for (const ctr::CroNamedImport& entry : target.module->tables.named_imports) {
        if (!BatchResolved(target, entry.batch) ||
            !FindNamedExport(departing, entry.name).has_value()) {
            continue;
        }
        const core::Status reset = ResetBatch(target, entry.batch);
        if (!reset) {
            return reset;
        }
    }
    return {};
}

// ResetModuleExport: whatever the target imports from a module of the
// departing module's name.
core::Status ResetModuleImports(LoadedModule& target, const LoadedModule& departing) {
    for (const ctr::CroImportModule& imported : target.module->tables.import_modules) {
        if (imported.name != departing.Name()) {
            continue;
        }
        const core::Status reset = ResetImportsFrom(target, imported);
        if (!reset) {
            return reset;
        }
    }
    return {};
}

// ResetExternalRelocations: before anything is imported, every external
// relocation is written as though it had resolved to the module's
// OnUnresolved hook, which is address zero for a module without one.
core::Status ResetExternals(LoadedModule& loaded) {
    const std::uint32_t unresolved =
        TagAddress(loaded, loaded.module->tables.hooks.on_unresolved).value_or(0);

    const std::size_t count = loaded.module->tables.external_relocations.size();
    loaded.external.assign(count, LinkedWord{});
    loaded.resolutions.assign(count, Resolution{});
    for (std::size_t index = 0; index < count; ++index) {
        const core::Status written = WriteExternal(loaded, index, unresolved);
        if (!written) {
            return written;
        }
    }
    return {};
}

// ApplyInternalRelocations: each word receives the address of a segment of its
// own module plus the addend.
core::Status ApplyInternals(LoadedModule& loaded) {
    const std::vector<ctr::CroInternalRelocation>& relocations =
        loaded.module->tables.internal_relocations;
    loaded.internal.assign(relocations.size(), LinkedWord{});

    for (std::size_t index = 0; index < relocations.size(); ++index) {
        const ctr::CroInternalRelocation& entry = relocations.at(index);
        const std::optional<std::uint32_t> address = TagAddress(loaded, entry.target);
        if (!address || entry.symbol_segment >= loaded.segment_addresses.size()) {
            return core::Error{
                std::format("internal relocation {} of {} is out of range", index, loaded.Name())};
        }

        const core::Result<std::optional<std::uint32_t>> value =
            RelocatedValue(entry.type,
                           entry.type_known,
                           loaded.segment_addresses.at(entry.symbol_segment),
                           entry.addend,
                           *address);
        if (!value) {
            return value.Failure().WithContext(
                std::format("internal relocation {} of {}", index, loaded.Name()));
        }

        LinkedWord& word = loaded.internal.at(index);
        word.target = entry.target;
        word.address = *address;
        word.value = value.Value();
    }
    return {};
}

// Everything a newly loaded module does against the modules already linked.
// The new module is not among them yet: Register comes after Link.
class Linking {
public:
    Linking(std::vector<LoadedModule>& linked, LoadedModule& loading)
        : linked_(&linked), loading_(&loading), loading_index_(linked.size()) {}

    // ApplyExitRelocations, which Rebase runs before Link.
    core::Status ExitRelocations() {
        for (const ctr::CroNamedImport& entry : Tables(*loading_).named_imports) {
            if (entry.name != AtexitImport) {
                continue;
            }
            const core::Status applied = ImportByName(entry, AtexitExport, true);
            if (!applied) {
                return applied;
            }
        }
        return {};
    }

    // ApplyImportNamedSymbol and ApplyModuleImport.
    core::Status Imports() {
        for (const ctr::CroNamedImport& entry : Tables(*loading_).named_imports) {
            const core::Status applied = ImportByName(entry, entry.name, false);
            if (!applied) {
                return applied;
            }
        }
        for (const ctr::CroImportModule& imported : Tables(*loading_).import_modules) {
            const std::optional<std::size_t> exporter = FindLinked(imported.name);
            if (!exporter) {
                continue;
            }
            const core::Status applied = ImportFromModule(*loading_, imported, *exporter);
            if (!applied) {
                return applied;
            }
        }
        return {};
    }

    // ApplyExportNamedSymbol and ApplyModuleExport, for every module linked so
    // far, the static one first.
    core::Status Exports() {
        for (LoadedModule& target : *linked_) {
            core::Status named = ExportNamesTo(target);
            if (!named) {
                return named;
            }
            for (const ctr::CroImportModule& imported : Tables(target).import_modules) {
                if (imported.name != loading_->Name()) {
                    continue;
                }
                const core::Status applied = ImportFromModule(target, imported, loading_index_);
                if (!applied) {
                    return applied;
                }
            }
        }
        return {};
    }

private:
    static const ctr::CroLinkTables& Tables(const LoadedModule& loaded) {
        return loaded.module->tables;
    }

    // The first linked module of a name, which is the one ForEachAutoLinkCRO
    // reaches first.
    [[nodiscard]] std::optional<std::size_t> FindLinked(std::string_view name) const {
        for (std::size_t index = 0; index < linked_->size(); ++index) {
            if (linked_->at(index).Name() == name) {
                return index;
            }
        }
        return std::nullopt;
    }

    // Searches the linked modules in order for an export of a name and applies
    // the first one found. ApplyExitRelocations does not look at whether the
    // batch is resolved already, and ApplyImportNamedSymbol does.
    core::Status ImportByName(const ctr::CroNamedImport& entry,
                              std::string_view export_name,
                              bool even_if_resolved) {
        if (!even_if_resolved && BatchResolved(*loading_, entry.batch)) {
            return {};
        }
        for (std::size_t index = 0; index < linked_->size(); ++index) {
            const std::optional<std::uint32_t> symbol =
                FindNamedExport(linked_->at(index), export_name);
            if (symbol) {
                return ApplyBatch(*loading_, entry.batch, *symbol, index);
            }
        }
        return {};
    }

    // What an importer takes from one exporter by index and by position. The
    // symbol is found in the exporter and written into the importer.
    core::Status ImportFromModule(LoadedModule& importer,
                                  const ctr::CroImportModule& imported,
                                  std::size_t exporter_index) {
        const LoadedModule& exporter = Module(exporter_index);
        for (const ctr::CroIndexedImport& entry : imported.indexed) {
            const std::vector<ctr::CroIndexedExport>& exports = exporter.module->indexed_exports;
            if (entry.export_index >= exports.size()) {
                return core::Error{
                    std::format("{} imports indexed export {} of {}, which declares {}",
                                importer.Name(),
                                entry.export_index,
                                exporter.Name(),
                                exports.size())};
            }
            const std::uint32_t symbol =
                TagAddress(exporter, exports.at(entry.export_index).tag).value_or(0);
            const core::Status applied = ApplyBatch(importer, entry.batch, symbol, exporter_index);
            if (!applied) {
                return applied;
            }
        }
        for (const ctr::CroAnonymousImport& entry : imported.anonymous) {
            const std::uint32_t symbol = TagAddress(exporter, entry.position).value_or(0);
            const core::Status applied = ApplyBatch(importer, entry.batch, symbol, exporter_index);
            if (!applied) {
                return applied;
            }
        }
        return {};
    }

    core::Status ExportNamesTo(LoadedModule& target) {
        for (const ctr::CroNamedImport& entry : Tables(target).named_imports) {
            if (BatchResolved(target, entry.batch)) {
                continue;
            }
            const std::optional<std::uint32_t> symbol = FindNamedExport(*loading_, entry.name);
            if (!symbol) {
                continue;
            }
            const core::Status applied = ApplyBatch(target, entry.batch, *symbol, loading_index_);
            if (!applied) {
                return applied;
            }
        }
        return {};
    }

    [[nodiscard]] const LoadedModule& Module(std::size_t index) const {
        return index == loading_index_ ? *loading_ : linked_->at(index);
    }

    std::vector<LoadedModule>* linked_;
    LoadedModule* loading_;
    std::size_t loading_index_;
};

}  // namespace

core::Result<CroModule> ReadCroModule(core::ByteSpan file) {
    CroModule module;

    const core::Result<ctr::CroHeader> header = ctr::ParseCroHeader(file);
    if (!header) {
        return header.Failure();
    }
    module.header = header.Value();

    const core::Result<std::vector<ctr::CroNamedExport>> named =
        ctr::ReadCroNamedExports(file, module.header);
    if (!named) {
        return named.Failure().WithContext(module.header.name);
    }
    module.named_exports = named.Value();

    const core::Result<std::vector<ctr::CroIndexedExport>> indexed =
        ctr::ReadCroIndexedExports(file, module.header);
    if (!indexed) {
        return indexed.Failure().WithContext(module.header.name);
    }
    module.indexed_exports = indexed.Value();

    const core::Result<ctr::CroLinkTables> tables = ctr::ReadCroLinkTables(file, module.header);
    if (!tables) {
        return tables.Failure().WithContext(module.header.name);
    }
    module.tables = tables.Value();
    return module;
}

core::Result<std::optional<std::uint32_t>> RelocatedValue(CroRelocationType type,
                                                          bool type_known,
                                                          std::uint32_t symbol_address,
                                                          std::uint32_t addend,
                                                          std::uint32_t word_address) {
    if (!type_known) {
        return core::Error{"a relocation of a type the format does not define"};
    }
    switch (type) {
    case CroRelocationType::Nothing:
        return std::optional<std::uint32_t>{};
    case CroRelocationType::AbsoluteAddress:
    case CroRelocationType::AbsoluteAddress2:
        return std::optional<std::uint32_t>{symbol_address + addend};
    case CroRelocationType::RelativeAddress:
        return std::optional<std::uint32_t>{symbol_address + addend - word_address};
    case CroRelocationType::ThumbBranch:
    case CroRelocationType::ArmBranch:
    case CroRelocationType::ModifyArmBranch:
    case CroRelocationType::AlignedRelativeAddress:
        break;
    }
    return core::Error{std::format("a relocation of type {}, which Azahar does not implement and "
                                   "no source this project has read describes",
                                   ctr::Describe(type))};
}

core::Result<Linker> Linker::FromStaticModule(const CroModule& static_module) {
    if (!static_module.header.IsStaticModule()) {
        return core::Error{std::format("{} is not the static module", static_module.header.name)};
    }

    LoadedModule loaded;
    loaded.module = &static_module;
    for (const ctr::CroSegment& segment : static_module.header.segments) {
        loaded.segment_addresses.push_back(static_cast<std::uint32_t>(segment.offset));
    }

    const core::Status reset = ResetExternals(loaded);
    if (!reset) {
        return reset.Failure();
    }
    const core::Status internal = ApplyInternals(loaded);
    if (!internal) {
        return internal.Failure();
    }

    std::vector<LoadedModule> modules;
    modules.push_back(std::move(loaded));
    return Linker{std::move(modules)};
}

core::Result<std::size_t>
Linker::Load(const CroModule& module, const Placement& placement, const LoadOptions& options) {
    if (module.header.IsStaticModule()) {
        return core::Error{"the static module is linked once, when the linker is made"};
    }
    if (!module.tables.static_anonymous_symbols.empty()) {
        return core::Error{std::format("{} hands symbols to the static module, which no title is "
                                       "known to do and Hexlith does not link",
                                       module.header.name)};
    }

    LoadedModule loaded;
    loaded.module = &module;
    loaded.placement = placement;
    loaded.options = options;
    loaded.segment_addresses = RebaseSegments(module.header, placement);

    // Rebase.
    const core::Status reset = ResetExternals(loaded);
    if (!reset) {
        return reset.Failure();
    }
    const core::Status internal = ApplyInternals(loaded);
    if (!internal) {
        return internal.Failure();
    }

    Linking linking{modules_, loaded};
    const core::Status exits = linking.ExitRelocations();
    if (!exits) {
        return exits.Failure();
    }

    // Link.
    const core::Status imports = linking.Imports();
    if (!imports) {
        return imports.Failure();
    }
    const core::Status exports = linking.Exports();
    if (!exports) {
        return exports.Failure();
    }

    // Register.
    modules_.push_back(std::move(loaded));
    return modules_.size() - 1;
}

core::Result<LoadedModule> Linker::Unload(std::size_t index) {
    if (index == 0 || index >= modules_.size()) {
        return core::Error{std::format("there is no loaded module {} to unload", index)};
    }

    // Unregister: the module leaves the list before anything is reset, so the
    // walk below does not reach it.
    LoadedModule unloaded = std::move(modules_.at(index));
    modules_.erase(modules_.begin() + static_cast<std::ptrdiff_t>(index));

    const core::Status own = ResetOwnImports(unloaded);
    if (!own) {
        return own.Failure();
    }

    // ResetExportNamedSymbol and ResetModuleExport, for the static module and
    // every module linked automatically, which is what ForEachAutoLinkCRO
    // walks.
    for (std::size_t target_index = 0; target_index < modules_.size(); ++target_index) {
        LoadedModule& target = modules_.at(target_index);
        if ((target_index != 0 && !target.options.auto_link) || !KeepsImports(target)) {
            continue;
        }
        if (unloaded.options.fix_level < ExportsEmptiedFrom) {
            const core::Status named = ResetNamesExportedBy(target, unloaded);
            if (!named) {
                return named.Failure();
            }
        }
        const core::Status imports = ResetModuleImports(target, unloaded);
        if (!imports) {
            return imports.Failure();
        }
    }

    // What is still resolved against a module after it keeps its index, one
    // place lower for the modules that came after the one unloaded.
    for (LoadedModule& remaining : modules_) {
        for (Resolution& resolution : remaining.resolutions) {
            if (resolution.resolved && resolution.exporter > index) {
                --resolution.exporter;
            }
        }
    }
    return unloaded;
}

core::Result<std::vector<std::uint8_t>> LinkedImage(const LoadedModule& loaded,
                                                    core::ByteSpan file) {
    const ctr::CroHeader& header = loaded.module->header;
    if (header.IsStaticModule()) {
        return core::Error{"the static module's segments are the main executable's, not its own"};
    }

    std::vector<std::uint8_t> image(file.begin(), file.end());
    for (const std::vector<LinkedWord>* words : {&loaded.internal, &loaded.external}) {
        for (const LinkedWord& word : *words) {
            if (!word.value.has_value()) {
                continue;
            }
            const std::uint32_t value = word.value.value();
            const ctr::CroSegment& segment = header.segments.at(word.target.SegmentIndex());
            const std::size_t at = segment.offset + word.target.OffsetInSegment();
            if (segment.type == CroSegmentType::Bss || at + sizeof(std::uint32_t) > image.size()) {
                return core::Error{std::format(
                    "{} has a linked word at {:#x}, outside its file", loaded.Name(), at)};
            }
            for (std::size_t byte = 0; byte < sizeof(std::uint32_t); ++byte) {
                image.at(at + byte) = static_cast<std::uint8_t>(value >> (8 * byte));
            }
        }
    }
    return image;
}

}  // namespace hexlith::domain::linker
