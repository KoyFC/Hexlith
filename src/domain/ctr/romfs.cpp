#include "domain/ctr/romfs.h"

#include "core/text.h"

#include <algorithm>
#include <array>
#include <format>
#include <unordered_set>

namespace hexlith::domain::ctr {
namespace {

// The level 3 header, with every offset already made relative to the start of
// the RomFS region.
struct Level3Header {
    std::size_t directory_metadata = 0;
    std::size_t directory_metadata_length = 0;
    std::size_t file_metadata = 0;
    std::size_t file_metadata_length = 0;
    std::size_t file_data = 0;
};

core::Status
RequireWithin(core::ByteSpan region, std::size_t offset, std::size_t size, std::string_view what) {
    if (offset > region.size() || size > region.size() - offset) {
        return core::Error{
            std::format("{} runs from {:#x} for {:#x} bytes, past the end of a RomFS of {:#x}",
                        what,
                        offset,
                        size,
                        region.size())};
    }
    return {};
}

core::Result<Level3Header> ReadLevel3Header(core::ByteSpan region) {
    if (!core::MatchesAt(region, romfs::IvfcMagicOffset, romfs::IvfcMagic)) {
        return core::Error{"there is no IVFC magic at the start of the RomFS region"};
    }

    const std::size_t base = romfs::Level3Offset;
    const core::Status reachable =
        RequireWithin(region, base, romfs::Level3HeaderLength, "the level 3 header");
    if (!reachable) {
        return reachable.Failure();
    }

    const std::uint32_t length = core::ReadU32At(region, base + romfs::HeaderLengthOffset).Value();
    if (length != romfs::Level3HeaderLength) {
        return core::Error{std::format(
            "the level 3 header length reads {:#x} rather than {:#x}, so level 3 is not {:#x} "
            "bytes into the region",
            length,
            romfs::Level3HeaderLength,
            romfs::Level3Offset)};
    }

    Level3Header header;
    header.directory_metadata =
        base + core::ReadU32At(region, base + romfs::DirectoryMetadataOffsetOffset).Value();
    header.directory_metadata_length =
        core::ReadU32At(region, base + romfs::DirectoryMetadataLengthOffset).Value();
    header.file_metadata =
        base + core::ReadU32At(region, base + romfs::FileMetadataOffsetOffset).Value();
    header.file_metadata_length =
        core::ReadU32At(region, base + romfs::FileMetadataLengthOffset).Value();
    header.file_data = base + core::ReadU32At(region, base + romfs::FileDataOffsetOffset).Value();

    const std::array<core::Status, 3> tables = {{
        RequireWithin(region,
                      header.directory_metadata,
                      header.directory_metadata_length,
                      "the directory metadata table"),
        RequireWithin(
            region, header.file_metadata, header.file_metadata_length, "the file metadata table"),
        RequireWithin(region, header.file_data, 0, "the file data area"),
    }};
    for (const core::Status& status : tables) {
        if (!status) {
            return status.Failure();
        }
    }

    return header;
}

// A name a filesystem image asks an extractor to create on the host.
//
// The image is untrusted input, so anything that could reach outside the
// output directory is refused. So is a control character, which no legitimate
// name in this title carries and which would make a file hard to name in a
// report or a manifest.
core::Status RequireUsableName(std::string_view name, std::size_t entry) {
    if (name.empty()) {
        return core::Error{std::format("the entry at {:#x} has no name", entry)};
    }
    if (name == "." || name == "..") {
        return core::Error{
            std::format("the entry at {:#x} is named '{}', which names a directory rather than "
                        "an entry in one",
                        entry,
                        name)};
    }
    for (const char character : name) {
        if (character == '/' || character == '\\') {
            return core::Error{std::format(
                "the entry at {:#x} is named '{}', which holds a path separator", entry, name)};
        }
        if (static_cast<unsigned char>(character) < ' ') {
            return core::Error{
                std::format("the name of the entry at {:#x} holds a control character", entry)};
        }
    }
    return {};
}

core::Result<std::string> ReadName(core::ByteSpan region,
                                   std::size_t entry,
                                   std::size_t length_field,
                                   std::size_t name_offset) {
    const std::uint32_t length = core::ReadU32At(region, entry + length_field).Value();

    const core::Status reachable = RequireWithin(region, entry + name_offset, length, "a name");
    if (!reachable) {
        return reachable.Failure();
    }

    const core::Result<std::string> name =
        core::Utf16LeToUtf8(core::ReadBytesAt(region, entry + name_offset, length).Value());
    if (!name) {
        return name.Failure().WithContext(std::format("the name of the entry at {:#x}", entry));
    }

    const core::Status usable = RequireUsableName(name.Value(), entry);
    if (!usable) {
        return usable.Failure();
    }
    return name.Value();
}

std::string Join(std::string_view directory, std::string_view name) {
    if (directory.empty()) {
        return std::string{name};
    }
    return std::string{directory} + "/" + std::string{name};
}

// One directory still to be walked.
struct PendingDirectory {
    std::size_t entry = 0;
    std::string path;
};

core::Status ReadFilesOf(core::ByteSpan region,
                         const Level3Header& header,
                         const PendingDirectory& directory,
                         std::unordered_set<std::size_t>& seen,
                         RomFs& filesystem) {
    std::uint32_t relative =
        core::ReadU32At(region, directory.entry + romfs::DirectoryFirstFileOffset).Value();

    while (relative != romfs::NoEntry) {
        const std::size_t entry = header.file_metadata + relative;
        const core::Status reachable =
            RequireWithin(region, entry, romfs::FileEntrySize, "a file metadata entry");
        if (!reachable) {
            return reachable.Failure();
        }

        // A sibling chain that comes back to an entry it already visited
        // would otherwise be walked for ever.
        if (!seen.insert(entry).second) {
            return core::Error{
                std::format("the file metadata entry at {:#x} appears twice in the tree", entry)};
        }

        const core::Result<std::string> name =
            ReadName(region, entry, romfs::FileNameLengthOffset, romfs::FileEntrySize);
        if (!name) {
            return name.Failure();
        }

        RomFsFile file;
        file.path = Join(directory.path, name.Value());
        file.offset = header.file_data +
                      static_cast<std::size_t>(
                          core::ReadU64At(region, entry + romfs::FileDataOffsetField).Value());
        file.size = static_cast<std::size_t>(
            core::ReadU64At(region, entry + romfs::FileDataLengthField).Value());

        const core::Status within =
            RequireWithin(region, file.offset, file.size, std::format("'{}'", file.path));
        if (!within) {
            return within.Failure();
        }

        filesystem.files.push_back(std::move(file));
        relative = core::ReadU32At(region, entry + romfs::FileNextSiblingOffset).Value();
    }

    return {};
}

core::Status ReadChildrenOf(core::ByteSpan region,
                            const Level3Header& header,
                            const PendingDirectory& directory,
                            std::unordered_set<std::size_t>& seen,
                            RomFs& filesystem,
                            std::vector<PendingDirectory>& pending) {
    std::uint32_t relative =
        core::ReadU32At(region, directory.entry + romfs::DirectoryFirstChildOffset).Value();

    while (relative != romfs::NoEntry) {
        const std::size_t entry = header.directory_metadata + relative;
        const core::Status reachable =
            RequireWithin(region, entry, romfs::DirectoryEntrySize, "a directory metadata entry");
        if (!reachable) {
            return reachable.Failure();
        }

        if (!seen.insert(entry).second) {
            return core::Error{std::format(
                "the directory metadata entry at {:#x} appears twice in the tree", entry)};
        }

        const core::Result<std::string> name =
            ReadName(region, entry, romfs::DirectoryNameLengthOffset, romfs::DirectoryEntrySize);
        if (!name) {
            return name.Failure();
        }

        PendingDirectory child;
        child.entry = entry;
        child.path = Join(directory.path, name.Value());
        filesystem.directories.push_back(child.path);
        pending.push_back(std::move(child));

        relative = core::ReadU32At(region, entry + romfs::DirectoryNextSiblingOffset).Value();
    }

    return {};
}

}  // namespace

const RomFsFile* RomFs::Find(std::string_view path) const {
    for (const RomFsFile& file : files) {
        if (file.path == path) {
            return &file;
        }
    }
    return nullptr;
}

std::size_t RomFs::TotalFileBytes() const {
    std::size_t total = 0;
    for (const RomFsFile& file : files) {
        total += file.size;
    }
    return total;
}

core::Result<RomFs> ParseRomFs(core::ByteSpan region) {
    const core::Result<Level3Header> header = ReadLevel3Header(region);
    if (!header) {
        return header.Failure();
    }

    RomFs filesystem;
    filesystem.level3_offset = romfs::Level3Offset;
    filesystem.file_data_offset = header.Value().file_data;

    std::unordered_set<std::size_t> seen_directories;
    std::unordered_set<std::size_t> seen_files;

    const std::size_t root = header.Value().directory_metadata + romfs::RootDirectory;
    const core::Status reachable =
        RequireWithin(region, root, romfs::DirectoryEntrySize, "the root directory entry");
    if (!reachable) {
        return reachable.Failure();
    }
    seen_directories.insert(root);

    std::vector<PendingDirectory> pending{PendingDirectory{.entry = root, .path = ""}};
    while (!pending.empty()) {
        const PendingDirectory directory = std::move(pending.back());
        pending.pop_back();

        const core::Status files =
            ReadFilesOf(region, header.Value(), directory, seen_files, filesystem);
        if (!files) {
            return files.Failure();
        }

        const core::Status children = ReadChildrenOf(
            region, header.Value(), directory, seen_directories, filesystem, pending);
        if (!children) {
            return children.Failure();
        }
    }

    std::ranges::sort(filesystem.directories);
    std::ranges::sort(filesystem.files, [](const RomFsFile& left, const RomFsFile& right) {
        return left.path < right.path;
    });

    return filesystem;
}

core::Result<core::ByteSpan> RomFsFileContents(core::ByteSpan region, const RomFsFile& file) {
    const core::Result<core::ByteSpan> contents = core::ReadBytesAt(region, file.offset, file.size);
    if (!contents) {
        return contents.Failure().WithContext(std::format("'{}'", file.path));
    }
    return contents.Value();
}

}  // namespace hexlith::domain::ctr
