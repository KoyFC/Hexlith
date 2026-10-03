#include "domain/ctr/exefs.h"

#include <format>

namespace hexlith::domain::ctr {
namespace {

core::Status CheckAgainstEarlierFiles(const std::vector<ExeFsFile>& files,
                                      const ExeFsFile& candidate) {
    for (const ExeFsFile& earlier : files) {
        if (earlier.name == candidate.name) {
            return core::Error{std::format("entries {} and {} are both named '{}'",
                                           earlier.index,
                                           candidate.index,
                                           candidate.name)};
        }

        // Two files sharing bytes is not something the format expresses, so
        // it means the offsets were misread.
        const bool overlaps = candidate.offset < earlier.offset + earlier.size &&
                              earlier.offset < candidate.offset + candidate.size;
        if (overlaps) {
            return core::Error{
                std::format("'{}' at {:#x} for {:#x} bytes overlaps '{}' at {:#x} for {:#x}",
                            candidate.name,
                            candidate.offset,
                            candidate.size,
                            earlier.name,
                            earlier.offset,
                            earlier.size)};
        }
    }
    return {};
}

}  // namespace

const ExeFsFile* ExeFs::Find(std::string_view name) const {
    for (const ExeFsFile& file : files) {
        if (file.name == name) {
            return &file;
        }
    }
    return nullptr;
}

core::Result<ExeFs> ParseExeFs(core::ByteSpan region) {
    if (region.size() < exefs::HeaderSize) {
        return core::Error{std::format("an ExeFS header is {:#x} bytes and the region is {:#x}",
                                       exefs::HeaderSize,
                                       region.size())};
    }

    ExeFs filesystem;

    for (std::size_t index = 0; index < exefs::MaxFileCount; ++index) {
        const std::size_t entry = exefs::FileHeaderOffset + (index * exefs::FileHeaderSize);

        const core::Result<std::string> name =
            core::ReadFixedAsciiAt(region, entry, exefs::FileNameSize);
        if (!name) {
            return name.Failure().WithContext(std::format("ExeFS entry {}", index));
        }

        ExeFsFile file;
        file.index = index;
        file.name = name.Value();
        file.offset = core::ReadU32At(region, entry + exefs::FileOffsetOffset).Value();
        file.size = core::ReadU32At(region, entry + exefs::FileSizeOffset).Value();

        // An unused entry is zeroed throughout. Anything half filled in means
        // the header was misread, and guessing which half to trust is how an
        // extractor writes out a file of the wrong length.
        const bool named = !file.name.empty();
        const bool sized = file.size > 0;
        if (!named && !sized) {
            continue;
        }
        if (named != sized) {
            return core::Error{
                std::format("ExeFS entry {} is named '{}' and {:#x} bytes long, so it is neither "
                            "a file nor an unused entry",
                            index,
                            file.name,
                            file.size)};
        }

        if (exefs::HeaderSize + file.offset + file.size > region.size()) {
            return core::Error{std::format(
                "'{}' runs from {:#x} for {:#x} bytes past the end of an ExeFS of {:#x} bytes",
                file.name,
                exefs::HeaderSize + file.offset,
                file.size,
                region.size())};
        }

        // The hash table is stored in reverse order against the file
        // headers, so the first file's hash is the last entry.
        const std::size_t hash_entry =
            exefs::HashTableOffset + ((exefs::MaxFileCount - 1 - index) * Sha256Size);
        const core::Result<core::Digest> hash = core::ReadDigestAt(region, hash_entry);
        if (!hash) {
            return hash.Failure().WithContext(std::format("the hash of '{}'", file.name));
        }
        file.hash = hash.Value();

        const core::Status consistent = CheckAgainstEarlierFiles(filesystem.files, file);
        if (!consistent) {
            return consistent.Failure();
        }

        filesystem.files.push_back(file);
    }

    if (filesystem.files.empty()) {
        return core::Error{"the ExeFS declares no files"};
    }

    return filesystem;
}

core::Result<core::ByteSpan> ExeFsFileContents(core::ByteSpan region, const ExeFsFile& file) {
    const core::Result<core::ByteSpan> contents =
        core::ReadBytesAt(region, exefs::HeaderSize + file.offset, file.size);
    if (!contents) {
        return contents.Failure().WithContext(std::format("'{}'", file.name));
    }
    return contents.Value();
}

}  // namespace hexlith::domain::ctr
