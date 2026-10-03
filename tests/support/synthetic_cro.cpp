#include "support/synthetic_cro.h"

#include "domain/ctr/layout.h"

#include "support/synthetic_ctr_image.h"

#include <string_view>

namespace hexlith::test {
namespace {

namespace cro = domain::ctr::cro;
using domain::ctr::CroSegmentType;

constexpr std::uint64_t ByteMask = 0xFF;
constexpr std::uint8_t HashFiller = 0x77;
constexpr std::size_t FieldAlignment = 4;

// The addresses the extended header gives the main executable's segments, so
// that a test can cross-check the static module against it the way
// docs/formats/cro0.md says the reference image does.
constexpr std::uint32_t StaticTextAddress = 0x00100000;
constexpr std::uint32_t StaticReadOnlyAddress = 0x0053C000;
constexpr std::uint32_t StaticDataAddress = 0x005A6000;

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

std::vector<std::uint8_t> BuildSyntheticCro(const SyntheticCroSpec& spec) {
    const std::string name = spec.static_module ? std::string{cro::StaticModuleName} : spec.name;

    const std::size_t name_at = cro::HeaderSize;
    const std::size_t name_size = name.size() + 1;
    const std::size_t segment_table_at = AlignUp(name_at + name_size, FieldAlignment);
    const std::size_t export_table_at =
        segment_table_at + (spec.segments.size() * cro::SegmentEntrySize);
    const std::size_t export_strings_at =
        export_table_at + (spec.counts.named_exports * cro::NamedExportEntrySize);
    const std::size_t export_strings_size = spec.counts.named_exports * 8;
    const std::size_t contents_at =
        AlignUp(export_strings_at + export_strings_size, FieldAlignment);

    // A BSS segment takes up no space in the file, and neither does anything
    // in the static module, whose segments are already mapped elsewhere.
    std::size_t contents_size = 0;
    std::vector<std::size_t> offsets;
    for (const SyntheticCroSegment& segment : spec.segments) {
        if (spec.static_module) {
            offsets.push_back(segment.virtual_address);
            continue;
        }
        if (segment.type == CroSegmentType::Bss) {
            offsets.push_back(0);
            continue;
        }
        offsets.push_back(contents_at + contents_size);
        contents_size += segment.size;
    }

    std::vector<std::uint8_t> module(contents_at + contents_size, 0);
    Writer write{module};

    // The four hashes, which Hexlith never verifies.
    write.Fill(0, cro::HashesSize, HashFiller);

    write.Text(cro::FieldOffset(cro::MagicField), spec.fixed_up ? cro::FixedUpMagic : cro::Magic);
    write.Field(cro::FileSizeField, static_cast<std::uint32_t>(module.size()));
    write.Field(cro::BssSizeField, 0);
    write.Field(cro::FixedSizeField, static_cast<std::uint32_t>(cro::HeaderSize));
    write.Field(cro::ModuleNameOffsetField, static_cast<std::uint32_t>(name_at));
    write.Field(cro::ModuleNameSizeField, static_cast<std::uint32_t>(name_size));
    write.Text(name_at, name);

    write.Field(cro::SegmentTableOffsetField, static_cast<std::uint32_t>(segment_table_at));
    write.Field(cro::SegmentCountField, static_cast<std::uint32_t>(spec.segments.size()));
    for (std::size_t index = 0; index < spec.segments.size(); ++index) {
        const std::size_t entry = segment_table_at + (index * cro::SegmentEntrySize);
        write.U32(entry + cro::SegmentOffsetOffset, static_cast<std::uint32_t>(offsets[index]));
        write.U32(entry + cro::SegmentSizeOffset,
                  static_cast<std::uint32_t>(spec.segments[index].size));
        write.U32(entry + cro::SegmentTypeOffset,
                  static_cast<std::uint32_t>(spec.segments[index].type));
    }

    write.Field(cro::NamedExportTableOffsetField, static_cast<std::uint32_t>(export_table_at));
    write.Field(cro::NamedExportCountField, static_cast<std::uint32_t>(spec.counts.named_exports));
    write.Field(cro::ExportStringsOffsetField, static_cast<std::uint32_t>(export_strings_at));
    write.Field(cro::ExportStringsSizeField, static_cast<std::uint32_t>(export_strings_size));

    // The remaining tables are declared but not laid out, because Phase 1
    // records their counts and does not walk them.
    write.Field(cro::IndexedExportCountField,
                static_cast<std::uint32_t>(spec.counts.indexed_exports));
    write.Field(cro::ExportTreeNodeCountField,
                static_cast<std::uint32_t>(spec.counts.export_tree_nodes));
    write.Field(cro::ImportModuleCountField,
                static_cast<std::uint32_t>(spec.counts.import_modules));
    write.Field(cro::ExternalRelocationCountField,
                static_cast<std::uint32_t>(spec.counts.external_relocations));
    write.Field(cro::NamedImportCountField, static_cast<std::uint32_t>(spec.counts.named_imports));
    write.Field(cro::IndexedImportCountField,
                static_cast<std::uint32_t>(spec.counts.indexed_imports));
    write.Field(cro::AnonymousImportCountField,
                static_cast<std::uint32_t>(spec.counts.anonymous_imports));
    write.Field(cro::StaticAnonymousSymbolCountField,
                static_cast<std::uint32_t>(spec.counts.static_anonymous_symbols));
    write.Field(cro::InternalRelocationCountField,
                static_cast<std::uint32_t>(spec.counts.internal_relocations));
    write.Field(cro::StaticRelocationCountField,
                static_cast<std::uint32_t>(spec.counts.static_relocations));

    for (std::size_t index = 0; index < spec.segments.size(); ++index) {
        if (spec.static_module || spec.segments[index].type == CroSegmentType::Bss) {
            continue;
        }
        const std::vector<std::uint8_t> contents =
            PatternBytes(spec.segments[index].size, static_cast<std::uint8_t>(0x30 + index));
        for (std::size_t byte = 0; byte < contents.size(); ++byte) {
            module[offsets[index] + byte] = contents[byte];
        }
    }

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
    spec.counts.named_exports = StaticNamedExports;
    spec.counts.import_modules = StaticImportModules;
    spec.counts.anonymous_imports = StaticAnonymousImports;
    spec.counts.external_relocations = StaticExternalRelocations;
    return spec;
}

}  // namespace hexlith::test
