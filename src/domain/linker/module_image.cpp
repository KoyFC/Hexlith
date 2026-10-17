#include "domain/linker/module_image.h"

#include "core/byte_reader.h"
#include "domain/ctr/layout.h"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

namespace hexlith::domain::linker {
namespace {

namespace cro = ctr::cro;

// The size of an entry of each table, in the order the header lists the
// tables from the code segment on. Source: ENTRY_SIZE in cro_helper.cpp; the
// sizes are the structures in cro_helper.h, and docs/formats/cro0.md has the
// layout of each.
constexpr std::size_t ExportTreeEntrySize = 8;
constexpr std::array<std::uint32_t, 17> EntrySizes = {
    1,  // code
    1,  // data
    1,  // module name
    cro::SegmentEntrySize,
    cro::NamedExportEntrySize,
    cro::IndexedExportEntrySize,
    1,  // export strings
    ExportTreeEntrySize,
    cro::ImportModuleEntrySize,
    cro::RelocationEntrySize,
    cro::ImportEntrySize,
    cro::ImportEntrySize,
    cro::ImportEntrySize,
    1,  // import strings
    cro::ImportEntrySize,
    cro::RelocationEntrySize,
    cro::RelocationEntrySize,
};

// The offsets RebaseHeader requires to be in ascending order, which is not
// quite the order of the header.
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

// The header field each fix level crops from: nothing at level 0, then the
// static anonymous symbols on, the import tables on, and everything from the
// export tables on. Source: FIX_BARRIERS in cro_helper.cpp.
constexpr std::array<std::size_t, 4> FixBarriers = {
    cro::FixLevel0BarrierField,
    cro::StaticAnonymousSymbolTableOffsetField,
    cro::ImportModuleTableOffsetField,
    cro::NamedExportTableOffsetField,
};

// RebaseHeader's limit on the file and BSS sizes, which Azahar notes the
// console's loader imposes.
constexpr std::uint32_t SizeLimit = 0x10000000;

constexpr std::string_view LoadableMagic = "CRO0";

// What stays mapped is a whole number of pages.
constexpr std::uint32_t PageSize = 0x1000;

core::Error FormatError(std::string_view what) {
    return core::Error{std::format("the module is malformed: {}", what)};
}

// A view of the image that reads and writes its words and bytes by address,
// as the loader sees them once the header holds addresses.
class Image {
public:
    Image(std::span<std::uint8_t> bytes, std::uint32_t address) : bytes_(bytes), base_(address) {}

    [[nodiscard]] std::uint32_t Field(std::size_t index) const {
        return Word(cro::FieldOffset(index));
    }

    void SetField(std::size_t index, std::uint32_t value) {
        SetWord(cro::FieldOffset(index), value);
    }

    [[nodiscard]] std::uint32_t Word(std::size_t offset) const {
        return core::ReadU32At(bytes_, offset).ValueOr(0);
    }

    void SetWord(std::size_t offset, std::uint32_t value) {
        for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
            bytes_[offset + byte] = static_cast<std::uint8_t>(value >> (8U * byte));
        }
    }

    void SetByte(std::size_t offset, std::uint8_t value) {
        bytes_[offset] = value;
    }

    [[nodiscard]] std::uint8_t Byte(std::size_t offset) const {
        return bytes_[offset];
    }

    // Where an entry of a table sits in the image, the table's field holding
    // its address.
    [[nodiscard]] std::size_t
    Entry(std::size_t table_field, std::size_t entry_size, std::size_t index) const {
        return Field(table_field) - base_ + (index * entry_size);
    }

    [[nodiscard]] std::uint32_t Base() const {
        return base_;
    }

    [[nodiscard]] std::uint32_t End() const {
        return base_ + static_cast<std::uint32_t>(bytes_.size());
    }

    [[nodiscard]] bool MatchesMagic(std::string_view magic) const {
        return core::MatchesAt(bytes_, cro::FieldOffset(cro::MagicField), magic);
    }

private:
    std::span<std::uint8_t> bytes_;
    std::uint32_t base_;
};

// Adds the module's address to an offset held at a place in the image,
// unless it is zero, and answers the address.
std::uint32_t RebaseWord(Image& image, std::size_t offset) {
    const std::uint32_t value = image.Word(offset);
    if (value == 0) {
        return 0;
    }
    image.SetWord(offset, value + image.Base());
    return value + image.Base();
}

bool Within(std::uint32_t address, std::uint32_t begin, std::uint32_t end) {
    return address >= begin && address <= end;
}

// The end of a table: its address plus its entries.
std::uint32_t TableEnd(const Image& image, std::size_t table_field, std::size_t entry_size) {
    return image.Field(table_field) +
           (image.Field(table_field + 1) * static_cast<std::uint32_t>(entry_size));
}

core::Status RebaseHeader(Image& image) {
    if (!image.MatchesMagic(LoadableMagic)) {
        return FormatError("its magic is not CRO0");
    }
    if (image.Field(cro::NextModuleField) != 0 || image.Field(cro::PreviousModuleField) != 0) {
        return FormatError("it is already in a module list");
    }
    if (image.Field(cro::FileSizeField) > SizeLimit || image.Field(cro::BssSizeField) > SizeLimit) {
        return FormatError("it is larger than the loader allows");
    }
    if (image.Field(cro::FixedSizeField) != 0) {
        return FormatError("it is already fixed");
    }
    if (image.Field(cro::CodeOffsetField) < cro::HeaderSize) {
        return FormatError("its code starts inside its header");
    }
    for (std::size_t index = 1; index < OffsetOrder.size(); ++index) {
        if (image.Field(OffsetOrder.at(index)) < image.Field(OffsetOrder.at(index - 1))) {
            return FormatError("its tables are out of order");
        }
    }

    static_cast<void>(RebaseWord(image, cro::FieldOffset(cro::NameOffsetField)));
    for (std::size_t field = cro::CodeOffsetField; field < cro::FixLevel0BarrierField; field += 2) {
        static_cast<void>(RebaseWord(image, cro::FieldOffset(field)));
    }
    for (std::size_t table = 0; table < EntrySizes.size(); ++table) {
        const std::size_t field = cro::CodeOffsetField + (table * 2);
        if (TableEnd(image, field, EntrySizes.at(table)) > image.End()) {
            return FormatError("a table reaches past its end");
        }
    }
    return {};
}

core::Status
RebaseSegmentTable(Image& image, const Placement& placement, const BufferSizes& sizes) {
    const std::uint32_t count = image.Field(cro::SegmentCountField);
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t entry =
            image.Entry(cro::SegmentTableOffsetField, cro::SegmentEntrySize, index);
        const std::size_t offset = entry + cro::SegmentOffsetOffset;
        const std::uint32_t size = image.Word(entry + cro::SegmentSizeOffset);
        const auto type =
            static_cast<ctr::CroSegmentType>(image.Word(entry + cro::SegmentTypeOffset));
        if (type == ctr::CroSegmentType::Data || type == ctr::CroSegmentType::Bss) {
            if (size == 0) {
                continue;
            }
            const bool data = type == ctr::CroSegmentType::Data;
            if (size > (data ? sizes.data : sizes.bss)) {
                return core::Error{std::format("the {} buffer is smaller than the segment",
                                               data ? "data" : "BSS")};
            }
            image.SetWord(offset, data ? placement.data_address : placement.bss_address);
        } else if (RebaseWord(image, offset) > image.End()) {
            return FormatError("a segment reaches past its end");
        }
    }
    return {};
}

// Rebases one word of every entry of a table, each of which has to land
// between two addresses.
core::Status RebaseEntries(Image& image,
                           std::size_t table_field,
                           std::size_t entry_size,
                           std::size_t word,
                           std::uint32_t begin,
                           std::uint32_t end,
                           std::string_view what) {
    const std::uint32_t count = image.Field(table_field + 1);
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t address =
            RebaseWord(image, image.Entry(table_field, entry_size, index) + word);
        if (address != 0 && !Within(address, begin, end)) {
            return FormatError(std::format("{} points outside its table", what));
        }
    }
    return {};
}

core::Status RebaseExportTables(Image& image) {
    const std::uint32_t strings = image.Field(cro::ExportStringsOffsetField);
    const std::uint32_t strings_end = strings + image.Field(cro::ExportStringsSizeField);
    return RebaseEntries(image,
                         cro::NamedExportTableOffsetField,
                         cro::NamedExportEntrySize,
                         cro::NamedExportNameOffsetOffset,
                         strings,
                         strings_end,
                         "an export's name");
}

core::Status RebaseImportModuleTable(Image& image) {
    const std::uint32_t strings = image.Field(cro::ImportStringsOffsetField);
    const std::uint32_t strings_end = strings + image.Field(cro::ImportStringsSizeField);
    core::Status names = RebaseEntries(image,
                                       cro::ImportModuleTableOffsetField,
                                       cro::ImportModuleEntrySize,
                                       cro::ImportModuleNameOffsetOffset,
                                       strings,
                                       strings_end,
                                       "an imported module's name");
    if (!names) {
        return names;
    }
    core::Status indexed =
        RebaseEntries(image,
                      cro::ImportModuleTableOffsetField,
                      cro::ImportModuleEntrySize,
                      cro::ImportModuleIndexedTableOffset,
                      image.Field(cro::IndexedImportTableOffsetField),
                      TableEnd(image, cro::IndexedImportTableOffsetField, cro::ImportEntrySize),
                      "an imported module's indexed imports");
    if (!indexed) {
        return indexed;
    }
    return RebaseEntries(
        image,
        cro::ImportModuleTableOffsetField,
        cro::ImportModuleEntrySize,
        cro::ImportModuleAnonymousTableOffset,
        image.Field(cro::AnonymousImportTableOffsetField),
        TableEnd(image, cro::AnonymousImportTableOffsetField, cro::ImportEntrySize),
        "an imported module's anonymous imports");
}

// ResetExternalRelocations, as far as it touches the table itself: the first
// entry of each batch is marked unresolved. The words the relocations patch
// are the Linker's.
core::Status ResetBatches(Image& image) {
    const std::uint32_t count = image.Field(cro::ExternalRelocationCountField);
    bool batch_begins = true;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t entry =
            image.Entry(cro::ExternalRelocationTableOffsetField, cro::RelocationEntrySize, index);
        if (batch_begins) {
            image.SetByte(entry + cro::RelocationBatchResolvedOffset, 0);
        }
        batch_begins = image.Byte(entry + cro::RelocationBatchEndOffset) != 0;
    }
    if (!batch_begins) {
        return FormatError("its last external relocation does not end a batch");
    }
    return {};
}

core::Status RebaseImportTables(Image& image) {
    const std::uint32_t strings = image.Field(cro::ImportStringsOffsetField);
    const std::uint32_t strings_end = strings + image.Field(cro::ImportStringsSizeField);
    const std::uint32_t batches = image.Field(cro::ExternalRelocationTableOffsetField);
    const std::uint32_t batches_end =
        TableEnd(image, cro::ExternalRelocationTableOffsetField, cro::RelocationEntrySize);

    core::Status names = RebaseEntries(image,
                                       cro::NamedImportTableOffsetField,
                                       cro::ImportEntrySize,
                                       cro::ImportWhatOffset,
                                       strings,
                                       strings_end,
                                       "a named import's name");
    if (!names) {
        return names;
    }
    for (const std::size_t table : {cro::NamedImportTableOffsetField,
                                    cro::IndexedImportTableOffsetField,
                                    cro::AnonymousImportTableOffsetField}) {
        core::Status rebased = RebaseEntries(image,
                                             table,
                                             cro::ImportEntrySize,
                                             cro::ImportBatchOffset,
                                             batches,
                                             batches_end,
                                             "an import's relocation batch");
        if (!rebased) {
            return rebased;
        }
    }
    return {};
}

// GetFixEnd: the end of the code and of every table up to the level's
// barrier.
std::uint32_t FixEnd(const Image& image, std::size_t barrier) {
    std::uint32_t end = image.Base() + static_cast<std::uint32_t>(cro::HeaderSize);
    end = std::max(end, TableEnd(image, cro::CodeOffsetField, 1));
    std::size_t table = 2;
    for (std::size_t field = cro::ModuleNameOffsetField; field < barrier; field += 2) {
        end = std::max(end, TableEnd(image, field, EntrySizes.at(table)));
        ++table;
    }
    return end;
}

// Takes the module's address off an address held at a place in the image,
// unless it is zero.
void UnrebaseWord(Image& image, std::size_t offset) {
    const std::uint32_t value = image.Word(offset);
    if (value != 0) {
        image.SetWord(offset, value - image.Base());
    }
}

// Unrebases one word of every entry of a table the header still counts.
void UnrebaseEntries(Image& image,
                     std::size_t table_field,
                     std::size_t entry_size,
                     std::size_t word) {
    const std::uint32_t count = image.Field(table_field + 1);
    for (std::uint32_t index = 0; index < count; ++index) {
        UnrebaseWord(image, image.Entry(table_field, entry_size, index) + word);
    }
}

// UnrebaseSegmentTable.
void UnrebaseSegments(Image& image) {
    const std::uint32_t count = image.Field(cro::SegmentCountField);
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t entry =
            image.Entry(cro::SegmentTableOffsetField, cro::SegmentEntrySize, index);
        const auto type =
            static_cast<ctr::CroSegmentType>(image.Word(entry + cro::SegmentTypeOffset));
        if (type == ctr::CroSegmentType::Bss) {
            image.SetWord(entry + cro::SegmentOffsetOffset, 0);
        } else {
            UnrebaseWord(image, entry + cro::SegmentOffsetOffset);
        }
    }
}

}  // namespace

void UnrebaseModuleImage(std::span<std::uint8_t> bytes,
                         std::uint32_t module_address,
                         bool is_static) {
    Image image(bytes, module_address);

    // The tables first, while the header still holds where they are. The
    // order is Unrebase's.
    UnrebaseEntries(
        image, cro::AnonymousImportTableOffsetField, cro::ImportEntrySize, cro::ImportBatchOffset);
    UnrebaseEntries(
        image, cro::IndexedImportTableOffsetField, cro::ImportEntrySize, cro::ImportBatchOffset);
    UnrebaseEntries(
        image, cro::NamedImportTableOffsetField, cro::ImportEntrySize, cro::ImportWhatOffset);
    UnrebaseEntries(
        image, cro::NamedImportTableOffsetField, cro::ImportEntrySize, cro::ImportBatchOffset);
    for (const std::size_t word : {cro::ImportModuleNameOffsetOffset,
                                   cro::ImportModuleIndexedTableOffset,
                                   cro::ImportModuleAnonymousTableOffset}) {
        UnrebaseEntries(image, cro::ImportModuleTableOffsetField, cro::ImportModuleEntrySize, word);
    }
    UnrebaseEntries(image,
                    cro::NamedExportTableOffsetField,
                    cro::NamedExportEntrySize,
                    cro::NamedExportNameOffsetOffset);
    if (!is_static) {
        UnrebaseSegments(image);
    }

    image.SetField(cro::NextModuleField, 0);
    image.SetField(cro::PreviousModuleField, 0);
    image.SetField(cro::FixedSizeField, 0);

    // UnrebaseHeader.
    UnrebaseWord(image, cro::FieldOffset(cro::NameOffsetField));
    for (std::size_t field = cro::CodeOffsetField; field < cro::FixLevel0BarrierField; field += 2) {
        UnrebaseWord(image, cro::FieldOffset(field));
    }
}

core::Status RebaseModuleImage(std::span<std::uint8_t> bytes,
                               std::uint32_t module_address,
                               const Placement& placement,
                               const BufferSizes& sizes,
                               bool is_static) {
    if (bytes.size() < cro::HeaderSize) {
        return FormatError("it is smaller than its header");
    }
    Image image(bytes, module_address);
    core::Status header = RebaseHeader(image);
    if (!header) {
        return header;
    }
    if (!is_static) {
        core::Status segments = RebaseSegmentTable(image, placement, sizes);
        if (!segments) {
            return segments;
        }
    }
    for (const auto& step :
         {RebaseExportTables, RebaseImportModuleTable, ResetBatches, RebaseImportTables}) {
        core::Status done = step(image);
        if (!done) {
            return done;
        }
    }
    return {};
}

core::Result<std::uint32_t> FixModuleImage(std::span<std::uint8_t> bytes,
                                           std::uint32_t module_address,
                                           std::uint32_t fix_level) {
    if (fix_level >= FixBarriers.size()) {
        return core::Error{std::format("there is no fix level {}", fix_level)};
    }
    Image image(bytes, module_address);
    const std::size_t barrier = FixBarriers.at(fix_level);
    const std::uint32_t end = FixEnd(image, barrier);
    if (fix_level != 0) {
        for (std::size_t byte = 0; byte < cro::FixedUpMagic.size(); ++byte) {
            image.SetByte(cro::FieldOffset(cro::MagicField) + byte,
                          static_cast<std::uint8_t>(cro::FixedUpMagic[byte]));
        }
        for (std::size_t field = barrier; field < cro::FixLevel0BarrierField; field += 2) {
            image.SetField(field, end);
            image.SetField(field + 1, 0);
        }
    }
    const std::uint32_t fixed_size = ((end + PageSize - 1) / PageSize * PageSize) - module_address;
    image.SetField(cro::FixedSizeField, fixed_size);
    return fixed_size;
}

}  // namespace hexlith::domain::linker
