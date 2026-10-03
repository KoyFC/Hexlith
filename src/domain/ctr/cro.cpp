#include "domain/ctr/cro.h"

#include <array>
#include <format>

namespace hexlith::domain::ctr {
namespace {

std::uint32_t Field(core::ByteSpan module, std::size_t index) {
    return core::ReadU32At(module, cro::FieldOffset(index)).Value();
}

core::Status
RequireWithin(core::ByteSpan module, std::size_t offset, std::size_t size, std::string_view what) {
    if (offset > module.size() || size > module.size() - offset) {
        return core::Error{
            std::format("{} runs from {:#x} for {:#x} bytes, past the end of a module of {:#x}",
                        what,
                        offset,
                        size,
                        module.size())};
    }
    return {};
}

core::Result<std::string> ReadModuleName(core::ByteSpan module) {
    const std::size_t offset = Field(module, cro::ModuleNameOffsetField);
    const std::size_t size = Field(module, cro::ModuleNameSizeField);

    if (size == 0) {
        return core::Error{"the module declares no name"};
    }

    const core::Status reachable = RequireWithin(module, offset, size, "the module name");
    if (!reachable) {
        return reachable.Failure();
    }

    // The declared size counts the terminator, which a fixed-width read stops
    // at anyway.
    const core::Result<std::string> name = core::ReadFixedAsciiAt(module, offset, size);
    if (!name) {
        return name.Failure().WithContext("the module name");
    }
    if (name.Value().empty()) {
        return core::Error{std::format("the module name at {:#x} is empty", offset)};
    }
    return name.Value();
}

core::Result<std::vector<CroSegment>> ReadSegments(core::ByteSpan module, bool static_module) {
    const std::size_t table = Field(module, cro::SegmentTableOffsetField);
    const std::size_t count = Field(module, cro::SegmentCountField);

    const core::Status reachable =
        RequireWithin(module, table, count * cro::SegmentEntrySize, "the segment table");
    if (!reachable) {
        return reachable.Failure();
    }

    std::vector<CroSegment> segments;
    segments.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t entry = table + (index * cro::SegmentEntrySize);

        CroSegment segment;
        segment.index = index;
        segment.offset = core::ReadU32At(module, entry + cro::SegmentOffsetOffset).Value();
        segment.size = core::ReadU32At(module, entry + cro::SegmentSizeOffset).Value();
        segment.raw_type = core::ReadU32At(module, entry + cro::SegmentTypeOffset).Value();

        static constexpr std::array<CroSegmentType, 4> KnownTypes = {CroSegmentType::Code,
                                                                     CroSegmentType::ReadOnlyData,
                                                                     CroSegmentType::Data,
                                                                     CroSegmentType::Bss};
        if (segment.raw_type < KnownTypes.size()) {
            segment.type = KnownTypes.at(segment.raw_type);
            segment.type_known = true;
        }

        // A BSS segment occupies no space in the file, so its offset
        // describes where it will be rather than where it is. Neither does
        // any segment of the static module, whose offsets are the virtual
        // addresses of code that is already mapped.
        const bool in_the_file =
            !static_module && segment.type_known && segment.type != CroSegmentType::Bss;
        if (in_the_file) {
            const core::Status within = RequireWithin(
                module, segment.offset, segment.size, std::format("segment {}", index));
            if (!within) {
                return within.Failure();
            }
        }

        segments.push_back(segment);
    }

    return segments;
}

CroTableCounts ReadCounts(core::ByteSpan module) {
    CroTableCounts counts;
    counts.segments = Field(module, cro::SegmentCountField);
    counts.named_exports = Field(module, cro::NamedExportCountField);
    counts.indexed_exports = Field(module, cro::IndexedExportCountField);
    counts.export_tree_nodes = Field(module, cro::ExportTreeNodeCountField);
    counts.import_modules = Field(module, cro::ImportModuleCountField);
    counts.external_relocations = Field(module, cro::ExternalRelocationCountField);
    counts.named_imports = Field(module, cro::NamedImportCountField);
    counts.indexed_imports = Field(module, cro::IndexedImportCountField);
    counts.anonymous_imports = Field(module, cro::AnonymousImportCountField);
    counts.static_anonymous_symbols = Field(module, cro::StaticAnonymousSymbolCountField);
    counts.internal_relocations = Field(module, cro::InternalRelocationCountField);
    counts.static_relocations = Field(module, cro::StaticRelocationCountField);
    return counts;
}

}  // namespace

std::string_view Describe(CroSegmentType type) {
    switch (type) {
    case CroSegmentType::Code:
        return "code";
    case CroSegmentType::ReadOnlyData:
        return "rodata";
    case CroSegmentType::Data:
        return "data";
    case CroSegmentType::Bss:
        return "bss";
    }
    return "unknown";
}

std::size_t CroHeader::SegmentBytes(CroSegmentType wanted) const {
    std::size_t total = 0;
    for (const CroSegment& segment : segments) {
        if (segment.type_known && segment.type == wanted) {
            total += segment.size;
        }
    }
    return total;
}

bool LooksLikeCroModule(core::ByteSpan file) {
    return core::MatchesAt(file, cro::FieldOffset(cro::MagicField), cro::Magic) ||
           core::MatchesAt(file, cro::FieldOffset(cro::MagicField), cro::FixedUpMagic);
}

core::Result<CroHeader> ParseCroHeader(core::ByteSpan module) {
    if (module.size() < cro::HeaderSize) {
        return core::Error{std::format(
            "a CRO header is {:#x} bytes and the file is {:#x}", cro::HeaderSize, module.size())};
    }

    const std::size_t magic_at = cro::FieldOffset(cro::MagicField);
    const bool fixed_up = core::MatchesAt(module, magic_at, cro::FixedUpMagic);
    if (!core::MatchesAt(module, magic_at, cro::Magic) && !fixed_up) {
        return core::Error{std::format(
            "there is no CRO0 or FIXD magic at offset {:#x}, so this is not a module", magic_at)};
    }

    CroHeader header;
    header.fixed_up = fixed_up;
    header.declared_file_size = Field(module, cro::FileSizeField);
    header.bss_size = Field(module, cro::BssSizeField);
    header.fixed_size = Field(module, cro::FixedSizeField);
    header.export_strings_size = Field(module, cro::ExportStringsSizeField);
    header.import_strings_size = Field(module, cro::ImportStringsSizeField);

    const core::Result<std::string> name = ReadModuleName(module);
    if (!name) {
        return name.Failure();
    }
    header.name = name.Value();

    // The static module is the exception to almost every bounds check here,
    // so it has to be identified before any of them run.
    const core::Result<std::vector<CroSegment>> segments =
        ReadSegments(module, header.IsStaticModule());
    if (!segments) {
        return segments.Failure();
    }
    header.segments = segments.Value();

    header.counts = ReadCounts(module);

    const core::Status exports =
        RequireWithin(module,
                      Field(module, cro::NamedExportTableOffsetField),
                      header.counts.named_exports * cro::NamedExportEntrySize,
                      "the named export table");
    if (!exports) {
        return exports.Failure();
    }

    const core::Status strings = RequireWithin(module,
                                               Field(module, cro::ExportStringsOffsetField),
                                               header.export_strings_size,
                                               "the export string table");
    if (!strings) {
        return strings.Failure();
    }

    if (header.declared_file_size > module.size()) {
        return core::Error{std::format("the module declares {:#x} bytes and the file holds {:#x}",
                                       header.declared_file_size,
                                       module.size())};
    }

    return header;
}

}  // namespace hexlith::domain::ctr
