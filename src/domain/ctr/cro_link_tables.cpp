#include "domain/ctr/cro_link_tables.h"

#include "domain/ctr/layout.h"

#include <algorithm>
#include <array>
#include <format>
#include <set>

namespace hexlith::domain::ctr {
namespace {

std::uint32_t Field(core::ByteSpan module, std::size_t index) {
    return core::ReadU32At(module, cro::FieldOffset(index)).Value();
}

// The table offsets in the order RebaseHeader in cro_helper.cpp requires them
// to appear in the file, closed by the end of the data and the file size. A
// module whose offsets go backwards is refused by the console's loader before
// anything is read from it.
constexpr std::array<std::size_t, 18> OffsetOrder = {
    cro::CodeOffsetField,
    cro::ModuleNameOffsetField,
    cro::SegmentTableOffsetField,
    cro::NamedExportTableOffsetField,
    cro::ExportTreeTableOffsetField,
    cro::IndexedExportTableOffsetField,
    cro::ExportStringsOffsetField,
    cro::ImportModuleTableOffsetField,
    cro::ExternalRelocationTableOffsetField,
    cro::NamedImportTableOffsetField,
    cro::IndexedImportTableOffsetField,
    cro::AnonymousImportTableOffsetField,
    cro::ImportStringsOffsetField,
    cro::StaticAnonymousSymbolTableOffsetField,
    cro::InternalRelocationTableOffsetField,
    cro::StaticRelocationTableOffsetField,
    cro::DataOffsetField,
    cro::FileSizeField,
};

core::Status RequireOffsetsInOrder(core::ByteSpan module) {
    for (std::size_t index = 1; index < OffsetOrder.size(); ++index) {
        const std::uint32_t before = Field(module, OffsetOrder.at(index - 1));
        const std::uint32_t after = Field(module, OffsetOrder.at(index));
        if (after < before) {
            return core::Error{std::format(
                "header field {} holds {:#x}, which comes before field {} at {:#x}, and the "
                "loader requires the tables in order",
                OffsetOrder.at(index),
                after,
                OffsetOrder.at(index - 1),
                before)};
        }
    }
    return {};
}

// A table as the header declares it: where it starts and how many entries it
// holds.
struct Table {
    std::size_t offset = 0;
    std::size_t count = 0;
    std::size_t entry_size = 0;

    [[nodiscard]] std::size_t End() const {
        return offset + (count * entry_size);
    }

    [[nodiscard]] std::size_t EntryAt(std::size_t index) const {
        return offset + (index * entry_size);
    }
};

core::Result<Table> DeclaredTable(core::ByteSpan module,
                                  std::size_t offset_field,
                                  std::size_t entry_size,
                                  std::string_view what) {
    const Table table{.offset = Field(module, offset_field),
                      .count = Field(module, offset_field + 1),
                      .entry_size = entry_size};
    if (table.offset > module.size() || table.count > (module.size() - table.offset) / entry_size) {
        return core::Error{
            std::format("{} runs from {:#x} for {} entries of {} bytes, past the end of a module "
                        "of {:#x}",
                        what,
                        table.offset,
                        table.count,
                        entry_size,
                        module.size())};
    }
    return table;
}

// A string named by its file offset, which has to land inside the string
// table it belongs to. The string runs to the first NUL or to the end of the
// table, whichever is first.
core::Result<std::string> ReadTableString(core::ByteSpan module,
                                          const Table& strings,
                                          std::size_t offset,
                                          std::string_view what) {
    if (offset < strings.offset || offset >= strings.End()) {
        return core::Error{std::format("{} names a string at {:#x}, outside the string table of "
                                       "{:#x} bytes at {:#x}",
                                       what,
                                       offset,
                                       strings.count,
                                       strings.offset)};
    }
    const core::Result<std::string> text =
        core::ReadFixedAsciiAt(module, offset, strings.End() - offset);
    if (!text) {
        return text.Failure().WithContext(what);
    }
    if (text.Value().empty()) {
        return core::Error{std::format("{} has an empty name at {:#x}", what, offset)};
    }
    return text.Value();
}

std::uint8_t ByteAt(core::ByteSpan module, std::size_t offset) {
    return core::ReadU8At(module, offset).Value();
}

void DecodeType(std::uint8_t raw, CroRelocationType& type, bool& known) {
    static constexpr std::array<CroRelocationType, 8> KnownTypes = {
        CroRelocationType::Nothing,
        CroRelocationType::AbsoluteAddress,
        CroRelocationType::RelativeAddress,
        CroRelocationType::ThumbBranch,
        CroRelocationType::ArmBranch,
        CroRelocationType::ModifyArmBranch,
        CroRelocationType::AbsoluteAddress2,
        CroRelocationType::AlignedRelativeAddress,
    };
    for (const CroRelocationType candidate : KnownTypes) {
        if (static_cast<std::uint8_t>(candidate) == raw) {
            type = candidate;
            known = true;
            return;
        }
    }
    type = CroRelocationType::Nothing;
    known = false;
}

// A relocation table made of batches, with the first entry of every batch
// recorded so that an import naming a batch can be checked against it.
struct BatchedTable {
    Table table;
    std::vector<CroBatchRelocation> entries;
    std::set<std::size_t> batch_starts;
};

core::Result<BatchedTable> ReadBatchedTable(core::ByteSpan module,
                                            const CroHeader& header,
                                            std::size_t offset_field,
                                            bool targets_this_module,
                                            std::string_view what) {
    const core::Result<Table> declared =
        DeclaredTable(module, offset_field, cro::RelocationEntrySize, what);
    if (!declared) {
        return declared.Failure();
    }

    BatchedTable batched;
    batched.table = declared.Value();
    batched.entries.reserve(batched.table.count);
    batched.batch_starts.insert(0);

    for (std::size_t index = 0; index < batched.table.count; ++index) {
        const std::size_t at = batched.table.EntryAt(index);

        CroBatchRelocation entry;
        entry.target.raw = core::ReadU32At(module, at + cro::RelocationTargetOffset).Value();
        entry.raw_type = ByteAt(module, at + cro::RelocationTypeOffset);
        DecodeType(entry.raw_type, entry.type, entry.type_known);
        entry.batch_end = ByteAt(module, at + cro::RelocationBatchEndOffset) != 0;
        entry.batch_resolved = ByteAt(module, at + cro::RelocationBatchResolvedOffset) != 0;
        entry.addend = core::ReadU32At(module, at + cro::RelocationAddendOffset).Value();

        // A static relocation patches the static module, whose segments this
        // module does not describe, so only the external ones can be checked.
        if (targets_this_module) {
            const core::Result<std::size_t> target =
                ResolveCroSegmentTag(header, entry.target, std::format("{} entry {}", what, index));
            if (!target) {
                return target.Failure();
            }
        }

        if (entry.batch_end) {
            batched.batch_starts.insert(index + 1);
        }
        batched.entries.push_back(entry);
    }

    // ResetExternalRelocations refuses a table whose last entry does not close
    // a batch, because the loader would run off the end of it.
    if (!batched.entries.empty() && !batched.entries.back().batch_end) {
        return core::Error{std::format("the last entry of {} does not end a batch", what)};
    }
    return batched;
}

// Turns the file offset an import gives for its batch into the indices of the
// entries the batch holds.
core::Result<CroBatch>
BatchAt(const BatchedTable& batched, std::size_t offset, std::string_view what) {
    const Table& table = batched.table;
    const bool inside = offset >= table.offset && offset < table.End() &&
                        (offset - table.offset) % table.entry_size == 0;
    if (!inside) {
        return core::Error{
            std::format("{} names a relocation batch at {:#x}, which is not an entry of the "
                        "table of {} at {:#x}",
                        what,
                        offset,
                        table.count,
                        table.offset)};
    }

    const std::size_t first = (offset - table.offset) / table.entry_size;
    if (!batched.batch_starts.contains(first)) {
        return core::Error{
            std::format("{} names relocation {}, which is in the middle of a batch", what, first)};
    }

    std::size_t last = first;
    while (!batched.entries.at(last).batch_end) {
        ++last;
    }
    return CroBatch{.first = first, .count = last - first + 1};
}

core::Result<std::vector<CroInternalRelocation>> ReadInternalRelocations(core::ByteSpan module,
                                                                         const CroHeader& header) {
    const core::Result<Table> declared = DeclaredTable(module,
                                                       cro::InternalRelocationTableOffsetField,
                                                       cro::RelocationEntrySize,
                                                       "the internal relocation table");
    if (!declared) {
        return declared.Failure();
    }

    std::vector<CroInternalRelocation> relocations;
    relocations.reserve(declared.Value().count);
    for (std::size_t index = 0; index < declared.Value().count; ++index) {
        const std::size_t at = declared.Value().EntryAt(index);

        CroInternalRelocation entry;
        entry.target.raw = core::ReadU32At(module, at + cro::RelocationTargetOffset).Value();
        entry.raw_type = ByteAt(module, at + cro::RelocationTypeOffset);
        DecodeType(entry.raw_type, entry.type, entry.type_known);
        entry.symbol_segment = ByteAt(module, at + cro::RelocationSymbolSegmentOffset);
        entry.addend = core::ReadU32At(module, at + cro::RelocationAddendOffset).Value();

        const std::string what = std::format("internal relocation {}", index);
        const core::Result<std::size_t> target = ResolveCroSegmentTag(header, entry.target, what);
        if (!target) {
            return target.Failure();
        }
        if (entry.symbol_segment >= header.segments.size()) {
            return core::Error{std::format("{} points into segment {}, and the module declares {}",
                                           what,
                                           entry.symbol_segment,
                                           header.segments.size())};
        }
        relocations.push_back(entry);
    }
    return relocations;
}

// One slice of the indexed or anonymous import table, as an import module
// entry gives it: a file offset and a count. An empty slice may carry any
// offset, because the loader never reads it.
core::Result<Table>
SliceOf(const Table& whole, std::size_t offset, std::size_t count, std::string_view what) {
    if (count == 0) {
        return Table{.offset = whole.offset, .count = 0, .entry_size = whole.entry_size};
    }
    const bool inside = offset >= whole.offset && (offset - whole.offset) % whole.entry_size == 0 &&
                        count <= (whole.End() - std::min(offset, whole.End())) / whole.entry_size;
    if (!inside) {
        return core::Error{std::format("{} runs from {:#x} for {} entries, outside the table of {} "
                                       "at {:#x}",
                                       what,
                                       offset,
                                       count,
                                       whole.count,
                                       whole.offset)};
    }
    return Table{.offset = offset, .count = count, .entry_size = whole.entry_size};
}

// The context the import tables are read in: the string table their names
// are in, and the relocation table their batches are in.
struct ImportContext {
    core::ByteSpan module;
    Table strings;
    Table indexed;
    Table anonymous;
    const BatchedTable* external = nullptr;
};

core::Result<CroImportModule>
ReadImportModule(const ImportContext& context, const Table& modules, std::size_t index) {
    const std::size_t at = modules.EntryAt(index);
    const auto word = [&](std::size_t offset) {
        return core::ReadU32At(context.module, at + offset).Value();
    };

    const std::string what = std::format("import module {}", index);
    const core::Result<std::string> name = ReadTableString(
        context.module, context.strings, word(cro::ImportModuleNameOffsetOffset), what);
    if (!name) {
        return name.Failure();
    }
    CroImportModule imported;
    imported.name = name.Value();

    const core::Result<Table> indexed =
        SliceOf(context.indexed,
                word(cro::ImportModuleIndexedTableOffset),
                word(cro::ImportModuleIndexedCountOffset),
                std::format("the indexed imports of '{}'", imported.name));
    if (!indexed) {
        return indexed.Failure();
    }
    for (std::size_t entry = 0; entry < indexed.Value().count; ++entry) {
        const std::size_t entry_at = indexed.Value().EntryAt(entry);
        const core::Result<CroBatch> batch =
            BatchAt(*context.external,
                    core::ReadU32At(context.module, entry_at + cro::ImportBatchOffset).Value(),
                    std::format("an indexed import from '{}'", imported.name));
        if (!batch) {
            return batch.Failure();
        }
        imported.indexed.push_back(CroIndexedImport{
            .export_index =
                core::ReadU32At(context.module, entry_at + cro::ImportWhatOffset).Value(),
            .batch = batch.Value()});
    }

    const core::Result<Table> anonymous =
        SliceOf(context.anonymous,
                word(cro::ImportModuleAnonymousTableOffset),
                word(cro::ImportModuleAnonymousCountOffset),
                std::format("the anonymous imports of '{}'", imported.name));
    if (!anonymous) {
        return anonymous.Failure();
    }
    for (std::size_t entry = 0; entry < anonymous.Value().count; ++entry) {
        const std::size_t entry_at = anonymous.Value().EntryAt(entry);
        const core::Result<CroBatch> batch =
            BatchAt(*context.external,
                    core::ReadU32At(context.module, entry_at + cro::ImportBatchOffset).Value(),
                    std::format("an anonymous import from '{}'", imported.name));
        if (!batch) {
            return batch.Failure();
        }
        imported.anonymous.push_back(CroAnonymousImport{
            .position =
                CroSegmentTag{
                    .raw =
                        core::ReadU32At(context.module, entry_at + cro::ImportWhatOffset).Value()},
            .batch = batch.Value()});
    }
    return imported;
}

core::Result<std::vector<CroNamedImport>> ReadNamedImports(const ImportContext& context) {
    const core::Result<Table> declared = DeclaredTable(context.module,
                                                       cro::NamedImportTableOffsetField,
                                                       cro::ImportEntrySize,
                                                       "the named import table");
    if (!declared) {
        return declared.Failure();
    }

    std::vector<CroNamedImport> imports;
    for (std::size_t index = 0; index < declared.Value().count; ++index) {
        const std::size_t at = declared.Value().EntryAt(index);
        const std::string what = std::format("named import {}", index);

        const core::Result<std::string> name =
            ReadTableString(context.module,
                            context.strings,
                            core::ReadU32At(context.module, at + cro::ImportWhatOffset).Value(),
                            what);
        if (!name) {
            return name.Failure();
        }
        const core::Result<CroBatch> batch =
            BatchAt(*context.external,
                    core::ReadU32At(context.module, at + cro::ImportBatchOffset).Value(),
                    what);
        if (!batch) {
            return batch.Failure();
        }
        imports.push_back(CroNamedImport{.name = name.Value(), .batch = batch.Value()});
    }
    return imports;
}

core::Result<std::vector<CroStaticAnonymousSymbol>> ReadStaticAnonymousSymbols(
    core::ByteSpan module, const CroHeader& header, const BatchedTable& static_relocations) {
    const core::Result<Table> declared = DeclaredTable(module,
                                                       cro::StaticAnonymousSymbolTableOffsetField,
                                                       cro::ImportEntrySize,
                                                       "the static anonymous symbol table");
    if (!declared) {
        return declared.Failure();
    }

    std::vector<CroStaticAnonymousSymbol> symbols;
    for (std::size_t index = 0; index < declared.Value().count; ++index) {
        const std::size_t at = declared.Value().EntryAt(index);
        const std::string what = std::format("static anonymous symbol {}", index);

        const CroSegmentTag position{
            .raw = core::ReadU32At(module, at + cro::ImportWhatOffset).Value()};
        const core::Result<std::size_t> resolved = ResolveCroSegmentTag(header, position, what);
        if (!resolved) {
            return resolved.Failure();
        }
        const core::Result<CroBatch> batch = BatchAt(
            static_relocations, core::ReadU32At(module, at + cro::ImportBatchOffset).Value(), what);
        if (!batch) {
            return batch.Failure();
        }
        symbols.push_back(CroStaticAnonymousSymbol{.position = position, .batch = batch.Value()});
    }
    return symbols;
}

CroHooks ReadHooks(core::ByteSpan module) {
    const auto tag = [&](std::size_t field) { return CroSegmentTag{.raw = Field(module, field)}; };
    return CroHooks{.unknown = tag(cro::UnknownSegmentTagField),
                    .on_load = tag(cro::OnLoadSegmentTagField),
                    .on_exit = tag(cro::OnExitSegmentTagField),
                    .on_unresolved = tag(cro::OnUnresolvedSegmentTagField)};
}

}  // namespace

std::string_view Describe(CroRelocationType type) {
    switch (type) {
    case CroRelocationType::Nothing:
        return "nothing";
    case CroRelocationType::AbsoluteAddress:
        return "absolute";
    case CroRelocationType::RelativeAddress:
        return "relative";
    case CroRelocationType::ThumbBranch:
        return "thumb-branch";
    case CroRelocationType::ArmBranch:
        return "arm-branch";
    case CroRelocationType::ModifyArmBranch:
        return "modify-arm-branch";
    case CroRelocationType::AbsoluteAddress2:
        return "absolute-2";
    case CroRelocationType::AlignedRelativeAddress:
        return "aligned-relative";
    }
    return "unknown";
}

core::Result<CroLinkTables> ReadCroLinkTables(core::ByteSpan module, const CroHeader& header) {
    const core::Status ordered = RequireOffsetsInOrder(module);
    if (!ordered) {
        return ordered.Failure();
    }

    const core::Result<BatchedTable> external =
        ReadBatchedTable(module,
                         header,
                         cro::ExternalRelocationTableOffsetField,
                         true,
                         "the external relocation table");
    if (!external) {
        return external.Failure();
    }
    const core::Result<BatchedTable> statics =
        ReadBatchedTable(module,
                         header,
                         cro::StaticRelocationTableOffsetField,
                         false,
                         "the static relocation table");
    if (!statics) {
        return statics.Failure();
    }

    const core::Result<Table> strings =
        DeclaredTable(module, cro::ImportStringsOffsetField, 1, "the import string table");
    const core::Result<Table> indexed = DeclaredTable(
        module, cro::IndexedImportTableOffsetField, cro::ImportEntrySize, "the indexed imports");
    const core::Result<Table> anonymous = DeclaredTable(module,
                                                        cro::AnonymousImportTableOffsetField,
                                                        cro::ImportEntrySize,
                                                        "the anonymous imports");
    const core::Result<Table> modules = DeclaredTable(module,
                                                      cro::ImportModuleTableOffsetField,
                                                      cro::ImportModuleEntrySize,
                                                      "the import module table");
    for (const core::Result<Table>* table : {&strings, &indexed, &anonymous, &modules}) {
        if (!*table) {
            return table->Failure();
        }
    }

    const ImportContext context{.module = module,
                                .strings = strings.Value(),
                                .indexed = indexed.Value(),
                                .anonymous = anonymous.Value(),
                                .external = &external.Value()};

    CroLinkTables tables;
    for (std::size_t index = 0; index < modules.Value().count; ++index) {
        const core::Result<CroImportModule> imported =
            ReadImportModule(context, modules.Value(), index);
        if (!imported) {
            return imported.Failure();
        }
        tables.import_modules.push_back(imported.Value());
    }

    const core::Result<std::vector<CroNamedImport>> named = ReadNamedImports(context);
    if (!named) {
        return named.Failure();
    }
    tables.named_imports = named.Value();

    const core::Result<std::vector<CroInternalRelocation>> internal =
        ReadInternalRelocations(module, header);
    if (!internal) {
        return internal.Failure();
    }
    tables.internal_relocations = internal.Value();

    const core::Result<std::vector<CroStaticAnonymousSymbol>> static_symbols =
        ReadStaticAnonymousSymbols(module, header, statics.Value());
    if (!static_symbols) {
        return static_symbols.Failure();
    }
    tables.static_anonymous_symbols = static_symbols.Value();

    tables.external_relocations = external.Value().entries;
    tables.static_relocations = statics.Value().entries;
    tables.hooks = ReadHooks(module);
    return tables;
}

}  // namespace hexlith::domain::ctr
