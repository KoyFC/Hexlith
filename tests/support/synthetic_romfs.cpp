#include "support/synthetic_romfs.h"

#include "core/text.h"
#include "domain/ctr/layout.h"

#include "support/synthetic_ctr_image.h"

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <string_view>

namespace hexlith::test {
namespace {

namespace romfs = domain::ctr::romfs;

constexpr std::size_t FileDataAlignment = 0x10;

// Not a file offset, and not the one the parser is allowed to use. The
// reference image reports 0xD66000 here.
constexpr std::uint64_t MisleadingLevel3LogicalOffset = 0xD66000;

constexpr std::uint32_t MasterHashSize = 0x20;

constexpr std::uint64_t ByteMask = 0xFF;
constexpr std::size_t HashBucketSize = 4;

std::size_t AlignUp(std::size_t value, std::size_t alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

std::vector<std::uint8_t> EncodeName(std::string_view name) {
    const core::Result<std::vector<std::uint8_t>> units = core::Utf8ToUtf16Le(name);
    if (!units) {
        throw std::invalid_argument("a synthetic RomFS name is not UTF-8: " + std::string{name});
    }
    return units.Value();
}

struct Directory {
    std::vector<std::uint8_t> name;
    std::size_t parent = 0;
    std::vector<std::size_t> children;
    std::vector<std::size_t> files;
    std::size_t entry = 0;
};

struct File {
    std::vector<std::uint8_t> name;
    std::size_t directory = 0;
    std::size_t entry = 0;
    std::size_t data_offset = 0;
    const std::vector<std::uint8_t>* data = nullptr;
};

std::vector<std::string_view> SplitPath(std::string_view path) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t separator = path.find('/', start);
        const std::size_t end = separator == std::string_view::npos ? path.size() : separator;
        parts.push_back(path.substr(start, end - start));
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    return parts;
}

class Tree {
public:
    Tree() {
        directories_.emplace_back();
        by_path_[""] = 0;
    }

    // Walks down from the root creating what is missing. Written as a walk
    // rather than as a recursion on the parent path, since a path is a list
    // of names and treating it as one reads better.
    std::size_t EnsureDirectory(std::string_view path) {
        std::size_t current = 0;
        if (path.empty()) {
            return current;
        }

        std::string walked;
        for (const std::string_view name : SplitPath(path)) {
            if (!walked.empty()) {
                walked.append("/");
            }
            walked.append(name);

            const auto existing = by_path_.find(walked);
            if (existing != by_path_.end()) {
                current = existing->second;
                continue;
            }

            const std::size_t index = directories_.size();
            Directory directory;
            directory.name = EncodeName(name);
            directory.parent = current;
            directories_.push_back(std::move(directory));
            directories_[current].children.push_back(index);
            by_path_[walked] = index;
            current = index;
        }
        return current;
    }

    void AddFile(const SyntheticRomFsFile& file) {
        const std::vector<std::string_view> parts = SplitPath(file.path);
        if (parts.empty() || parts.back().empty()) {
            throw std::invalid_argument("a synthetic RomFS path names no file: " + file.path);
        }

        const std::size_t separator = file.path.rfind('/');
        const std::string_view directory_path =
            separator == std::string::npos ? std::string_view{}
                                           : std::string_view{file.path}.substr(0, separator);
        const std::size_t directory = EnsureDirectory(directory_path);

        const std::size_t index = files_.size();
        File entry;
        entry.name = EncodeName(parts.back());
        entry.directory = directory;
        entry.data = &file.data;
        files_.push_back(std::move(entry));
        directories_[directory].files.push_back(index);
    }

    std::vector<Directory>& Directories() {
        return directories_;
    }

    std::vector<File>& Files() {
        return files_;
    }

private:
    std::vector<Directory> directories_;
    std::vector<File> files_;
    std::map<std::string, std::size_t> by_path_;
};

// Enough buckets to hold the entries, rounded up to a power of two, which is
// the shape the format expects even though Hexlith never reads them.
std::size_t BucketCount(std::size_t entries) {
    std::size_t buckets = 1;
    while (buckets < entries) {
        buckets *= 2;
    }
    return buckets;
}

class RegionWriter {
public:
    explicit RegionWriter(std::vector<std::uint8_t>& bytes) : bytes_(&bytes) {}

    void U32(std::size_t offset, std::uint32_t value) {
        Integer(offset, value, 4);
    }

    void U64(std::size_t offset, std::uint64_t value) {
        Integer(offset, value, 8);
    }

    void Bytes(std::size_t offset, const std::vector<std::uint8_t>& data) {
        for (std::size_t index = 0; index < data.size(); ++index) {
            bytes_->at(offset + index) = data[index];
        }
    }

    void Text(std::size_t offset, std::string_view text) {
        for (std::size_t index = 0; index < text.size(); ++index) {
            bytes_->at(offset + index) = static_cast<std::uint8_t>(text[index]);
        }
    }

private:
    void Integer(std::size_t offset, std::uint64_t value, std::size_t width) {
        for (std::size_t index = 0; index < width; ++index) {
            bytes_->at(offset + index) =
                static_cast<std::uint8_t>((value >> (8 * index)) & ByteMask);
        }
    }

    std::vector<std::uint8_t>* bytes_;
};

// Where each table of level 3 lands, relative to the start of level 3.
struct TableLayout {
    std::size_t directory_hash_at = 0;
    std::size_t directory_hash_length = 0;
    std::size_t directory_table_at = 0;
    std::size_t directory_table_length = 0;
    std::size_t file_hash_at = 0;
    std::size_t file_hash_length = 0;
    std::size_t file_table_at = 0;
    std::size_t file_table_length = 0;
    std::size_t data_at = 0;
    std::size_t data_length = 0;
};

// The IVFC wrapper. Hexlith reads nothing from it but the magic, so the level
// fields describe a plausible tree rather than a real one, and the level 3
// logical offset is deliberately not a file offset.
void WriteIvfcHeader(RegionWriter& write) {
    write.Text(romfs::IvfcMagicOffset, romfs::IvfcMagic);
    write.U32(romfs::IvfcMagicNumberOffset, romfs::IvfcMagicNumber);
    write.U32(romfs::MasterHashSizeOffset, MasterHashSize);
    write.U64(romfs::Level3LogicalOffsetOffset, MisleadingLevel3LogicalOffset);
}

void WriteLevel3Header(RegionWriter& write, const TableLayout& layout) {
    const auto field = [&write](std::size_t offset, std::size_t value) {
        write.U32(romfs::Level3Offset + offset, static_cast<std::uint32_t>(value));
    };

    field(romfs::HeaderLengthOffset, romfs::Level3HeaderLength);
    field(romfs::DirectoryHashTableOffsetOffset, layout.directory_hash_at);
    field(romfs::DirectoryHashTableLengthOffset, layout.directory_hash_length);
    field(romfs::DirectoryMetadataOffsetOffset, layout.directory_table_at);
    field(romfs::DirectoryMetadataLengthOffset, layout.directory_table_length);
    field(romfs::FileHashTableOffsetOffset, layout.file_hash_at);
    field(romfs::FileHashTableLengthOffset, layout.file_hash_length);
    field(romfs::FileMetadataOffsetOffset, layout.file_table_at);
    field(romfs::FileMetadataLengthOffset, layout.file_table_length);
    field(romfs::FileDataOffsetOffset, layout.data_at);
}

void WriteEmptyHashTables(RegionWriter& write, const TableLayout& layout) {
    const std::size_t level3 = romfs::Level3Offset;
    for (std::size_t bucket = 0; bucket < layout.directory_hash_length / HashBucketSize; ++bucket) {
        write.U32(level3 + layout.directory_hash_at + (bucket * HashBucketSize), romfs::NoEntry);
    }
    for (std::size_t bucket = 0; bucket < layout.file_hash_length / HashBucketSize; ++bucket) {
        write.U32(level3 + layout.file_hash_at + (bucket * HashBucketSize), romfs::NoEntry);
    }
}

// The entry of whichever sibling follows position in chain, or the chain
// terminator when nothing does.
template<typename Entries>
std::uint32_t
NextSibling(const std::vector<std::size_t>& chain, std::size_t index, const Entries& entries) {
    for (std::size_t position = 0; position < chain.size(); ++position) {
        if (chain[position] != index) {
            continue;
        }
        if (position + 1 >= chain.size()) {
            return romfs::NoEntry;
        }
        return static_cast<std::uint32_t>(entries[chain[position + 1]].entry);
    }
    return romfs::NoEntry;
}

void WriteDirectoryEntries(RegionWriter& write,
                           const TableLayout& layout,
                           const std::vector<Directory>& directories,
                           const std::vector<File>& files) {
    const std::size_t table = romfs::Level3Offset + layout.directory_table_at;

    for (std::size_t index = 0; index < directories.size(); ++index) {
        const Directory& directory = directories[index];
        const std::size_t at = table + directory.entry;

        write.U32(at + romfs::DirectoryParentOffset,
                  static_cast<std::uint32_t>(directories[directory.parent].entry));

        // The root has no siblings, and the chain it would belong to does
        // not exist, so its chain is terminated rather than searched for.
        const std::uint32_t next =
            index == 0 ? romfs::NoEntry
                       : NextSibling(directories[directory.parent].children, index, directories);
        write.U32(at + romfs::DirectoryNextSiblingOffset, next);

        write.U32(at + romfs::DirectoryFirstChildOffset,
                  directory.children.empty()
                      ? romfs::NoEntry
                      : static_cast<std::uint32_t>(directories[directory.children.front()].entry));
        write.U32(at + romfs::DirectoryFirstFileOffset,
                  directory.files.empty()
                      ? romfs::NoEntry
                      : static_cast<std::uint32_t>(files[directory.files.front()].entry));
        write.U32(at + romfs::DirectoryNextInBucketOffset, romfs::NoEntry);
        write.U32(at + romfs::DirectoryNameLengthOffset,
                  static_cast<std::uint32_t>(directory.name.size()));
        write.Bytes(at + romfs::DirectoryEntrySize, directory.name);
    }
}

void WriteFileEntries(RegionWriter& write,
                      const TableLayout& layout,
                      const std::vector<Directory>& directories,
                      const std::vector<File>& files) {
    const std::size_t table = romfs::Level3Offset + layout.file_table_at;
    const std::size_t data = romfs::Level3Offset + layout.data_at;

    for (std::size_t index = 0; index < files.size(); ++index) {
        const File& file = files[index];
        const std::size_t at = table + file.entry;

        write.U32(at + romfs::FileParentOffset,
                  static_cast<std::uint32_t>(directories[file.directory].entry));
        write.U32(at + romfs::FileNextSiblingOffset,
                  NextSibling(directories[file.directory].files, index, files));
        write.U64(at + romfs::FileDataOffsetField, file.data_offset);
        write.U64(at + romfs::FileDataLengthField, file.data->size());
        write.U32(at + romfs::FileNextInBucketOffset, romfs::NoEntry);
        write.U32(at + romfs::FileNameLengthOffset, static_cast<std::uint32_t>(file.name.size()));
        write.Bytes(at + romfs::FileEntrySize, file.name);

        write.Bytes(data + file.data_offset, *file.data);
    }
}

// Assigns every entry and every file its place. Entry sizes vary with the
// length of the name that follows them, so the tables have to be laid out
// before anything can be written.
TableLayout PlanTables(std::vector<Directory>& directories, std::vector<File>& files) {
    TableLayout layout;

    for (Directory& directory : directories) {
        directory.entry = layout.directory_table_length;
        layout.directory_table_length +=
            romfs::DirectoryEntrySize + AlignUp(directory.name.size(), romfs::NameAlignment);
    }

    for (File& file : files) {
        file.entry = layout.file_table_length;
        layout.file_table_length +=
            romfs::FileEntrySize + AlignUp(file.name.size(), romfs::NameAlignment);
    }

    for (File& file : files) {
        file.data_offset = layout.data_length;
        layout.data_length = AlignUp(layout.data_length + file.data->size(), FileDataAlignment);
    }

    layout.directory_hash_length = BucketCount(directories.size()) * HashBucketSize;
    layout.file_hash_length = BucketCount(std::max<std::size_t>(files.size(), 1)) * HashBucketSize;

    layout.directory_hash_at = romfs::Level3HeaderLength;
    layout.directory_table_at = layout.directory_hash_at + layout.directory_hash_length;
    layout.file_hash_at = layout.directory_table_at + layout.directory_table_length;
    layout.file_table_at = layout.file_hash_at + layout.file_hash_length;
    layout.data_at = AlignUp(layout.file_table_at + layout.file_table_length, FileDataAlignment);
    return layout;
}

}  // namespace

std::vector<std::uint8_t> BuildSyntheticRomFs(const SyntheticRomFsSpec& spec) {
    Tree tree;
    for (const std::string& directory : spec.empty_directories) {
        tree.EnsureDirectory(directory);
    }
    for (const SyntheticRomFsFile& file : spec.files) {
        tree.AddFile(file);
    }

    std::vector<Directory>& directories = tree.Directories();
    std::vector<File>& files = tree.Files();
    const TableLayout layout = PlanTables(directories, files);

    std::vector<std::uint8_t> region(romfs::Level3Offset + layout.data_at + layout.data_length, 0);
    RegionWriter write{region};

    WriteIvfcHeader(write);
    WriteLevel3Header(write, layout);
    WriteEmptyHashTables(write, layout);
    WriteDirectoryEntries(write, layout, directories, files);
    WriteFileEntries(write, layout, directories, files);

    return region;
}

SyntheticRomFsSpec DefaultRomFsSpec() {
    // Shaped like the filesystem on the cartridge: the static module and a
    // couple of CRO modules in the root, the hash container in its own
    // directory, bulk data under a numbered hierarchy, and one name that is
    // not ASCII so that the UTF-16 the format stores is not silently assumed
    // to be one byte per character.
    static constexpr std::array<std::string_view, 9> Paths = {
        ".crr/static.crr",
        "DllBattle.cro",
        "DllField.cro",
        "a/0/0",
        "a/0/1",
        "a/1/0",
        "m/\xC3\xB1\xE6\x97\xA5.moflex",
        "sound/stream/bgm.bcstm",
        "static.crs",
    };

    // Each file gets a different length and a different filler, so that a
    // parser reading one file's bytes for another produces something visibly
    // wrong rather than something plausible.
    constexpr std::size_t SmallestFile = 0x40;
    constexpr std::size_t FileSizeStep = 0x30;

    SyntheticRomFsSpec spec;
    for (std::size_t index = 0; index < Paths.size(); ++index) {
        spec.files.push_back(
            SyntheticRomFsFile{.path = std::string{Paths[index]},
                               .data = PatternBytes(SmallestFile + (index * FileSizeStep),
                                                    static_cast<std::uint8_t>(index + 1))});
    }

    spec.empty_directories = {"shaders"};
    return spec;
}

}  // namespace hexlith::test
