#include "support/synthetic_cro.h"

#include "domain/ctr/layout.h"

#include "support/synthetic_ctr_image.h"

#include <string_view>

namespace hexlith::test {
namespace {

namespace cro = domain::ctr::cro;
using domain::ctr::CroSegmentType;
using domain::ctr::CroTableCounts;

constexpr std::uint64_t ByteMask = 0xFF;
constexpr std::uint8_t HashFiller = 0x77;
constexpr std::size_t FieldAlignment = 4;

// The addresses the extended header gives the main executable's segments, so
// that a test can cross-check the static module against it the way
// docs/formats/cro0.md says the reference image does.
constexpr std::uint32_t StaticTextAddress = 0x00100000;
constexpr std::uint32_t StaticReadOnlyAddress = 0x0053C000;
constexpr std::uint32_t StaticDataAddress = 0x005A6000;

// A table the builder declares without writing still needs the string table to
// be reachable, and this is the room each unwritten entry is given in it.
constexpr std::size_t UnwrittenNameSize = 8;

// ExportTreeEntry in cro_helper.h. The builder only ever reserves room for it.
constexpr std::size_t ExportTreeEntrySize = 8;

std::uint32_t SegmentTag(const SyntheticCroExport& entry) {
    return (entry.offset_in_segment << cro::SegmentTagOffsetShift) |
           (entry.segment_index & cro::SegmentTagIndexMask);
}

std::size_t AlignUp(std::size_t value, std::size_t alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

class Writer {
public:
    explicit Writer(std::vector<std::uint8_t>& bytes) : bytes_(&bytes) {}

    void Field(std::size_t index, std::uint32_t value) {
        Integer(cro::FieldOffset(index), value);
    }

    void U32(std::size_t offset, std::uint32_t value) {
        Integer(offset, value);
    }

    void Text(std::size_t offset, std::string_view text) {
        for (std::size_t index = 0; index < text.size(); ++index) {
            bytes_->at(offset + index) = static_cast<std::uint8_t>(text[index]);
        }
    }

    void Byte(std::size_t offset, std::uint8_t value) {
        bytes_->at(offset) = value;
    }

    void Fill(std::size_t offset, std::size_t size, std::uint8_t value) {
        for (std::size_t index = 0; index < size; ++index) {
            bytes_->at(offset + index) = value;
        }
    }

private:
    void Integer(std::size_t offset, std::uint32_t value) {
        for (std::size_t index = 0; index < 4; ++index) {
            bytes_->at(offset + index) =
                static_cast<std::uint8_t>((value >> (8 * index)) & ByteMask);
        }
    }

    std::vector<std::uint8_t>* bytes_;
};

}  // namespace

namespace {

std::uint32_t PackTag(std::uint32_t segment_index, std::uint32_t offset_in_segment) {
    return (offset_in_segment << cro::SegmentTagOffsetShift) |
           (segment_index & cro::SegmentTagIndexMask);
}

// A relocation table built from batches laid end to end, with the index of
// the first entry of each batch so the imports can point at it.
struct BatchTable {
    std::vector<SyntheticCroRelocation> entries;
    std::vector<bool> batch_end;
    std::vector<std::size_t> batch_first;

    void Add(const std::vector<SyntheticCroRelocation>& batch) {
        batch_first.push_back(entries.size());
        for (std::size_t index = 0; index < batch.size(); ++index) {
            entries.push_back(batch[index]);
            batch_end.push_back(index + 1 == batch.size());
        }
    }
};

// A string table being built, with the offset of each string in it.
struct StringTable {
    std::string text;

    std::size_t Add(std::string_view value) {
        const std::size_t offset = text.size();
        text += value;
        text.push_back('\0');
        return offset;
    }
};

struct Placement {
    std::size_t at = 0;
    std::size_t count = 0;
};

class Layout {
public:
    explicit Layout(std::size_t start) : next_(start) {}

    Placement Place(std::size_t count, std::size_t entry_size) {
        next_ = AlignUp(next_, FieldAlignment);
        const Placement placement{.at = next_, .count = count};
        next_ += count * entry_size;
        return placement;
    }

    [[nodiscard]] std::size_t Next() const {
        return next_;
    }

private:
    std::size_t next_;
};

std::size_t CountOr(std::size_t listed, std::size_t declared) {
    return listed != 0 ? listed : declared;
}

// Where everything goes, in the order RebaseHeader in cro_helper.cpp requires:
// code and read-only data first, then the name and the tables, then data.
// The title's own modules are laid out the same way.
struct Plan {
    std::string name;
    std::vector<std::size_t> segment_offsets;
    Placement code;
    Placement data;

    Placement name_at;
    Placement segment_table;
    Placement named_exports;
    Placement export_tree;
    Placement indexed_exports;
    Placement export_strings;
    Placement import_modules;
    Placement external_relocations;
    Placement named_imports;
    Placement indexed_imports;
    Placement anonymous_imports;
    Placement import_strings;
    Placement static_symbols;
    Placement internal_relocations;
    Placement static_relocations;

    StringTable export_text;
    std::vector<std::size_t> export_name_offsets;
    StringTable import_text;
    std::vector<std::size_t> module_name_offsets;
    std::vector<std::size_t> import_name_offsets;

    BatchTable external;
    BatchTable statics;

    std::size_t file_size = 0;
};

// The external relocation table is the batches of the imports in the order
// the import tables list them, and the static one is built the same way.
void PlanBatches(const SyntheticCroSpec& spec, Plan& plan) {
    for (const SyntheticCroImport& entry : spec.named_imports) {
        plan.external.Add(entry.batch);
    }
    for (const SyntheticCroImportModule& module : spec.import_modules) {
        for (const SyntheticCroImport& entry : module.indexed) {
            plan.external.Add(entry.batch);
        }
        for (const SyntheticCroImport& entry : module.anonymous) {
            plan.external.Add(entry.batch);
        }
    }
    for (const SyntheticCroImport& entry : spec.static_anonymous_symbols) {
        plan.statics.Add(entry.batch);
    }
}

void PlanStrings(const SyntheticCroSpec& spec, Plan& plan) {
    for (const SyntheticCroExport& entry : spec.named_exports) {
        plan.export_name_offsets.push_back(plan.export_text.Add(entry.name));
    }
    if (spec.named_exports.empty()) {
        // Room for a string table nothing points into, so that the declared
        // size is reachable.
        plan.export_text.text.assign(spec.counts.named_exports * UnwrittenNameSize, '\0');
    }
    for (const SyntheticCroImportModule& module : spec.import_modules) {
        plan.module_name_offsets.push_back(plan.import_text.Add(module.name));
    }
    for (const SyntheticCroImport& entry : spec.named_imports) {
        plan.import_name_offsets.push_back(plan.import_text.Add(entry.name));
    }
}

// Code and read-only data come straight after the header. A BSS segment takes
// up no space in the file, and neither does anything in the static module,
// whose segments are already mapped elsewhere.
void PlanCode(const SyntheticCroSpec& spec, Layout& layout, Plan& plan) {
    plan.segment_offsets.assign(spec.segments.size(), 0);
    plan.code = layout.Place(0, 1);
    for (std::size_t index = 0; index < spec.segments.size(); ++index) {
        const SyntheticCroSegment& segment = spec.segments[index];
        if (spec.static_module) {
            plan.segment_offsets[index] = segment.virtual_address;
        } else if (segment.type == CroSegmentType::Code ||
                   segment.type == CroSegmentType::ReadOnlyData) {
            plan.segment_offsets[index] = layout.Place(segment.size, 1).at;
        }
    }
    plan.code.count = layout.Next() - plan.code.at;
}

void PlanData(const SyntheticCroSpec& spec, Layout& layout, Plan& plan) {
    plan.data = layout.Place(0, 1);
    for (std::size_t index = 0; index < spec.segments.size(); ++index) {
        if (!spec.static_module && spec.segments[index].type == CroSegmentType::Data) {
            plan.segment_offsets[index] = layout.Place(spec.segments[index].size, 1).at;
        }
    }
    plan.data.count = layout.Next() - plan.data.at;
}

void PlanTables(const SyntheticCroSpec& spec, Layout& layout, Plan& plan) {
    std::size_t indexed_imports = 0;
    std::size_t anonymous_imports = 0;
    for (const SyntheticCroImportModule& module : spec.import_modules) {
        indexed_imports += module.indexed.size();
        anonymous_imports += module.anonymous.size();
    }
    const CroTableCounts& counts = spec.counts;

    plan.name_at = layout.Place(plan.name.size() + 1, 1);
    plan.segment_table = layout.Place(spec.segments.size(), cro::SegmentEntrySize);
    plan.named_exports = layout.Place(CountOr(spec.named_exports.size(), counts.named_exports),
                                      cro::NamedExportEntrySize);
    plan.export_tree = layout.Place(counts.export_tree_nodes, ExportTreeEntrySize);
    plan.indexed_exports = layout.Place(
        CountOr(spec.indexed_exports.size(), counts.indexed_exports), cro::IndexedExportEntrySize);
    plan.export_strings = layout.Place(plan.export_text.text.size(), 1);
    plan.import_modules = layout.Place(CountOr(spec.import_modules.size(), counts.import_modules),
                                       cro::ImportModuleEntrySize);
    plan.external_relocations =
        layout.Place(CountOr(plan.external.entries.size(), counts.external_relocations),
                     cro::RelocationEntrySize);
    plan.named_imports = layout.Place(CountOr(spec.named_imports.size(), counts.named_imports),
                                      cro::ImportEntrySize);
    plan.indexed_imports =
        layout.Place(CountOr(indexed_imports, counts.indexed_imports), cro::ImportEntrySize);
    plan.anonymous_imports =
        layout.Place(CountOr(anonymous_imports, counts.anonymous_imports), cro::ImportEntrySize);
    plan.import_strings = layout.Place(plan.import_text.text.size(), 1);
    plan.static_symbols =
        layout.Place(CountOr(spec.static_anonymous_symbols.size(), counts.static_anonymous_symbols),
                     cro::ImportEntrySize);
    plan.internal_relocations =
        layout.Place(CountOr(spec.internal_relocations.size(), counts.internal_relocations),
                     cro::RelocationEntrySize);
    plan.static_relocations = layout.Place(
        CountOr(plan.statics.entries.size(), counts.static_relocations), cro::RelocationEntrySize);
}

Plan PlanModule(const SyntheticCroSpec& spec) {
    Plan plan;
    plan.name = spec.static_module ? std::string{cro::StaticModuleName} : spec.name;
    PlanBatches(spec, plan);
    PlanStrings(spec, plan);

    Layout layout{cro::HeaderSize};
    PlanCode(spec, layout, plan);
    PlanTables(spec, layout, plan);
    PlanData(spec, layout, plan);
    plan.file_size = AlignUp(layout.Next(), FieldAlignment);
    return plan;
}

void WriteHeader(const SyntheticCroSpec& spec, const Plan& plan, Writer& write) {
    // The four hashes, which Hexlith never verifies.
    write.Fill(0, cro::HashesSize, HashFiller);

    write.Text(cro::FieldOffset(cro::MagicField), spec.fixed_up ? cro::FixedUpMagic : cro::Magic);
    write.Field(cro::NameOffsetField, static_cast<std::uint32_t>(plan.name_at.at));
    write.Field(cro::FileSizeField, static_cast<std::uint32_t>(plan.file_size));
    write.Field(cro::BssSizeField, 0);
    write.Field(cro::FixedSizeField, static_cast<std::uint32_t>(cro::HeaderSize));
    write.Field(cro::UnknownSegmentTagField, spec.unknown_hook);
    write.Field(cro::OnLoadSegmentTagField, spec.on_load_hook);
    write.Field(cro::OnExitSegmentTagField, spec.on_exit_hook);
    write.Field(cro::OnUnresolvedSegmentTagField, spec.on_unresolved_hook);

    const auto declare = [&](std::size_t offset_field, const Placement& placement) {
        write.Field(offset_field, static_cast<std::uint32_t>(placement.at));
        write.Field(offset_field + 1, static_cast<std::uint32_t>(placement.count));
    };
    declare(cro::CodeOffsetField, plan.code);
    declare(cro::DataOffsetField, plan.data);
    declare(cro::ModuleNameOffsetField, plan.name_at);
    declare(cro::SegmentTableOffsetField, plan.segment_table);
    declare(cro::NamedExportTableOffsetField, plan.named_exports);
    declare(cro::IndexedExportTableOffsetField, plan.indexed_exports);
    declare(cro::ExportStringsOffsetField, plan.export_strings);
    declare(cro::ExportTreeTableOffsetField, plan.export_tree);
    declare(cro::ImportModuleTableOffsetField, plan.import_modules);
    declare(cro::ExternalRelocationTableOffsetField, plan.external_relocations);
    declare(cro::NamedImportTableOffsetField, plan.named_imports);
    declare(cro::IndexedImportTableOffsetField, plan.indexed_imports);
    declare(cro::AnonymousImportTableOffsetField, plan.anonymous_imports);
    declare(cro::ImportStringsOffsetField, plan.import_strings);
    declare(cro::StaticAnonymousSymbolTableOffsetField, plan.static_symbols);
    declare(cro::InternalRelocationTableOffsetField, plan.internal_relocations);
    declare(cro::StaticRelocationTableOffsetField, plan.static_relocations);

    write.Text(plan.name_at.at, plan.name);
}

void WriteSegments(const SyntheticCroSpec& spec, const Plan& plan, Writer& write) {
    for (std::size_t index = 0; index < spec.segments.size(); ++index) {
        const std::size_t entry = plan.segment_table.at + (index * cro::SegmentEntrySize);
        write.U32(entry + cro::SegmentOffsetOffset,
                  static_cast<std::uint32_t>(plan.segment_offsets[index]));
        write.U32(entry + cro::SegmentSizeOffset,
                  static_cast<std::uint32_t>(spec.segments[index].size));
        write.U32(entry + cro::SegmentTypeOffset,
                  static_cast<std::uint32_t>(spec.segments[index].type));
    }
}

void WriteExports(const SyntheticCroSpec& spec, const Plan& plan, Writer& write) {
    write.Text(plan.export_strings.at, plan.export_text.text);
    for (std::size_t index = 0; index < spec.named_exports.size(); ++index) {
        const std::size_t at = plan.named_exports.at + (index * cro::NamedExportEntrySize);
        write.U32(
            at + cro::NamedExportNameOffsetOffset,
            static_cast<std::uint32_t>(plan.export_strings.at + plan.export_name_offsets[index]));
        write.U32(at + cro::NamedExportPositionOffset, SegmentTag(spec.named_exports[index]));
    }
    for (std::size_t index = 0; index < spec.indexed_exports.size(); ++index) {
        write.U32(plan.indexed_exports.at + (index * cro::IndexedExportEntrySize),
                  SegmentTag(spec.indexed_exports[index]));
    }
}

void WriteBatches(const BatchTable& table, const Placement& placement, Writer& write) {
    for (std::size_t index = 0; index < table.entries.size(); ++index) {
        const SyntheticCroRelocation& entry = table.entries[index];
        const std::size_t at = placement.at + (index * cro::RelocationEntrySize);
        write.U32(at + cro::RelocationTargetOffset,
                  PackTag(entry.segment_index, entry.offset_in_segment));
        write.Byte(at + cro::RelocationTypeOffset, entry.type);
        write.Byte(at + cro::RelocationBatchEndOffset, table.batch_end[index] ? 1 : 0);
        write.U32(at + cro::RelocationAddendOffset, entry.addend);
    }
}

// Writes the imports in the order PlanBatches laid their batches out, so that
// the nth import written names the nth batch.
class ImportWriter {
public:
    ImportWriter(const Plan& plan, Writer& write) : plan_(&plan), write_(&write) {}

    void Write(std::size_t table_at, std::uint32_t what) {
        const std::size_t batch_at =
            plan_->external_relocations.at +
            (plan_->external.batch_first.at(batch_++) * cro::RelocationEntrySize);
        write_->U32(table_at + cro::ImportWhatOffset, what);
        write_->U32(table_at + cro::ImportBatchOffset, static_cast<std::uint32_t>(batch_at));
    }

private:
    const Plan* plan_;
    Writer* write_;
    std::size_t batch_ = 0;
};

void WriteImportModule(const SyntheticCroImportModule& imported,
                       std::size_t index,
                       const Plan& plan,
                       std::size_t& indexed_written,
                       std::size_t& anonymous_written,
                       ImportWriter& imports,
                       Writer& write) {
    const std::size_t at = plan.import_modules.at + (index * cro::ImportModuleEntrySize);
    const std::size_t indexed_at =
        plan.indexed_imports.at + (indexed_written * cro::ImportEntrySize);
    const std::size_t anonymous_at =
        plan.anonymous_imports.at + (anonymous_written * cro::ImportEntrySize);

    write.U32(at + cro::ImportModuleNameOffsetOffset,
              static_cast<std::uint32_t>(plan.import_strings.at + plan.module_name_offsets[index]));
    write.U32(at + cro::ImportModuleIndexedTableOffset, static_cast<std::uint32_t>(indexed_at));
    write.U32(at + cro::ImportModuleIndexedCountOffset,
              static_cast<std::uint32_t>(imported.indexed.size()));
    write.U32(at + cro::ImportModuleAnonymousTableOffset, static_cast<std::uint32_t>(anonymous_at));
    write.U32(at + cro::ImportModuleAnonymousCountOffset,
              static_cast<std::uint32_t>(imported.anonymous.size()));

    for (const SyntheticCroImport& entry : imported.indexed) {
        imports.Write(plan.indexed_imports.at + (indexed_written++ * cro::ImportEntrySize),
                      entry.export_index);
    }
    for (const SyntheticCroImport& entry : imported.anonymous) {
        imports.Write(plan.anonymous_imports.at + (anonymous_written++ * cro::ImportEntrySize),
                      PackTag(entry.segment_index, entry.offset_in_segment));
    }
}

void WriteImports(const SyntheticCroSpec& spec, const Plan& plan, Writer& write) {
    write.Text(plan.import_strings.at, plan.import_text.text);

    ImportWriter imports{plan, write};
    for (std::size_t index = 0; index < spec.named_imports.size(); ++index) {
        imports.Write(
            plan.named_imports.at + (index * cro::ImportEntrySize),
            static_cast<std::uint32_t>(plan.import_strings.at + plan.import_name_offsets[index]));
    }

    std::size_t indexed_written = 0;
    std::size_t anonymous_written = 0;
    for (std::size_t index = 0; index < spec.import_modules.size(); ++index) {
        WriteImportModule(spec.import_modules[index],
                          index,
                          plan,
                          indexed_written,
                          anonymous_written,
                          imports,
                          write);
    }

    for (std::size_t index = 0; index < spec.static_anonymous_symbols.size(); ++index) {
        const SyntheticCroImport& entry = spec.static_anonymous_symbols[index];
        const std::size_t at = plan.static_symbols.at + (index * cro::ImportEntrySize);
        const std::size_t batch_at =
            plan.static_relocations.at +
            (plan.statics.batch_first.at(index) * cro::RelocationEntrySize);
        write.U32(at + cro::ImportWhatOffset,
                  PackTag(entry.segment_index, entry.offset_in_segment));
        write.U32(at + cro::ImportBatchOffset, static_cast<std::uint32_t>(batch_at));
    }
}

void WriteInternalRelocations(const SyntheticCroSpec& spec, const Plan& plan, Writer& write) {
    for (std::size_t index = 0; index < spec.internal_relocations.size(); ++index) {
        const SyntheticCroRelocation& entry = spec.internal_relocations[index];
        const std::size_t at = plan.internal_relocations.at + (index * cro::RelocationEntrySize);
        write.U32(at + cro::RelocationTargetOffset,
                  PackTag(entry.segment_index, entry.offset_in_segment));
        write.Byte(at + cro::RelocationTypeOffset, entry.type);
        write.Byte(at + cro::RelocationSymbolSegmentOffset, entry.symbol_segment);
        write.U32(at + cro::RelocationAddendOffset, entry.addend);
    }
}

void WriteContents(const SyntheticCroSpec& spec, const Plan& plan, Writer& write) {
    for (std::size_t index = 0; index < spec.segments.size(); ++index) {
        if (spec.static_module || spec.segments[index].type == CroSegmentType::Bss) {
            continue;
        }
        const std::vector<std::uint8_t> contents =
            PatternBytes(spec.segments[index].size, static_cast<std::uint8_t>(0x30 + index));
        for (std::size_t byte = 0; byte < contents.size(); ++byte) {
            write.Byte(plan.segment_offsets[index] + byte, contents[byte]);
        }
    }
}

}  // namespace

std::vector<std::uint8_t> BuildSyntheticCro(const SyntheticCroSpec& spec) {
    const Plan plan = PlanModule(spec);

    std::vector<std::uint8_t> module(plan.file_size, 0);
    Writer write{module};
    WriteHeader(spec, plan, write);
    WriteSegments(spec, plan, write);
    WriteExports(spec, plan, write);
    WriteBatches(plan.external, plan.external_relocations, write);
    WriteBatches(plan.statics, plan.static_relocations, write);
    WriteImports(spec, plan, write);
    WriteInternalRelocations(spec, plan, write);
    WriteContents(spec, plan, write);
    return module;
}

// The counts docs/formats/cro0.md measured on DllField.cro, the largest
// module in the title. A module exports almost nothing and imports almost
// everything, and using the real numbers keeps a test from being written
// against a shape the cartridge does not have.
constexpr std::size_t DllFieldNamedExports = 2;
constexpr std::size_t DllFieldImportModules = 30;
constexpr std::size_t DllFieldExternalRelocations = 3951;
constexpr std::size_t DllFieldAnonymousImports = 2976;
constexpr std::size_t DllFieldInternalRelocations = 10237;

// The CRS, which is the only source of symbol names the cartridge gives
// away. The same document measured these.
constexpr std::size_t StaticNamedExports = 461;
constexpr std::size_t StaticImportModules = 78;
constexpr std::size_t StaticAnonymousImports = 328;
constexpr std::size_t StaticExternalRelocations = 331;

namespace {

// An odd position in a code segment, which is how a Thumb entry point is
// spelt: the low bit is the interworking flag and not part of the address.
// Measured on the reference image, where 338 of the 520 code exports are odd
// and none is merely even, which is what the convention predicts.
constexpr std::uint32_t ThumbExportOffset = 0x101;

// The 461 names of the static module, generated rather than taken from the
// cartridge. Mangled C++ names are what the real table holds, and the mix of
// ARM and Thumb positions is the mix the reference image has.
std::vector<SyntheticCroExport> BuildStaticModuleExports() {
    constexpr std::uint32_t Spacing = 0x40;
    constexpr std::size_t ThumbEvery = 3;

    std::vector<SyntheticCroExport> exports;
    exports.reserve(StaticNamedExports);
    for (std::size_t index = 0; index < StaticNamedExports; ++index) {
        const bool thumb = index % ThumbEvery != 0;
        exports.push_back(SyntheticCroExport{
            .name = "_ZN7hexlith6symbol" + std::to_string(index) + "Ev",
            .segment_index = 0,
            .offset_in_segment = static_cast<std::uint32_t>(index * Spacing) + (thumb ? 1U : 0U)});
    }
    return exports;
}

}  // namespace

SyntheticCroSpec DefaultCroSpec() {
    // A module exports almost nothing and imports almost everything, which
    // docs/formats/cro0.md records as the shape of the problem.
    constexpr std::size_t CodeSize = 0x400;
    constexpr std::size_t ReadOnlySize = 0x180;
    constexpr std::size_t DataSize = 0x80;
    constexpr std::size_t BssSize = 0x200;

    SyntheticCroSpec spec;
    spec.name = "DllField";
    spec.segments = {
        SyntheticCroSegment{.type = CroSegmentType::Code, .size = CodeSize},
        SyntheticCroSegment{.type = CroSegmentType::ReadOnlyData, .size = ReadOnlySize},
        SyntheticCroSegment{.type = CroSegmentType::Data, .size = DataSize},
        SyntheticCroSegment{.type = CroSegmentType::Bss, .size = BssSize},
    };
    // Two exports, which is what the real module publishes, one in ARM and one
    // in Thumb so that both spellings of a code position appear.
    spec.named_exports = {
        SyntheticCroExport{.name = "DllFieldMain", .segment_index = 0, .offset_in_segment = 0},
        SyntheticCroExport{
            .name = "DllFieldOnLoad", .segment_index = 0, .offset_in_segment = ThumbExportOffset},
    };
    spec.counts.named_exports = DllFieldNamedExports;
    spec.counts.import_modules = DllFieldImportModules;
    spec.counts.external_relocations = DllFieldExternalRelocations;
    spec.counts.anonymous_imports = DllFieldAnonymousImports;
    spec.counts.internal_relocations = DllFieldInternalRelocations;
    return spec;
}

SyntheticCroSpec StaticModuleSpec() {
    constexpr std::size_t TextSize = 4439772;
    constexpr std::size_t ReadOnlySize = 432708;
    constexpr std::size_t DataSize = 278720;

    SyntheticCroSpec spec;
    spec.static_module = true;
    spec.segments = {
        SyntheticCroSegment{
            .type = CroSegmentType::Code, .size = TextSize, .virtual_address = StaticTextAddress},
        SyntheticCroSegment{.type = CroSegmentType::ReadOnlyData,
                            .size = ReadOnlySize,
                            .virtual_address = StaticReadOnlyAddress},
        SyntheticCroSegment{
            .type = CroSegmentType::Data, .size = DataSize, .virtual_address = StaticDataAddress},
    };
    spec.named_exports = BuildStaticModuleExports();
    spec.counts.named_exports = StaticNamedExports;
    spec.counts.import_modules = StaticImportModules;
    spec.counts.anonymous_imports = StaticAnonymousImports;
    spec.counts.external_relocations = StaticExternalRelocations;
    return spec;
}

}  // namespace hexlith::test
