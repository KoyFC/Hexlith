#ifndef HEXLITH_ADAPTERS_FILESYSTEM_FILE_ROM_SOURCE_H
#define HEXLITH_ADAPTERS_FILESYSTEM_FILE_ROM_SOURCE_H

#include "ports/rom_source.h"

#include <cstdint>
#include <filesystem>
#include <span>

namespace hexlith::adapters::filesystem {

// Reads a cartridge image from a file.
//
// The file is opened read-only and never written, renamed or moved, and a
// symbolic link is refused so that a path cannot be redirected after it was
// checked. ADR 0008 states both.
//
// The file is mapped into memory rather than read. The image is two
// gigabytes, a run touches its ExeFS and the part of its RomFS the title
// opens, and reading all of it first cost most of a second of every run. A
// page is read from disk the first time something looks at it. The mapping is
// POSIX, as the dlopen the lifted library is opened with is.
//
// The file must not shrink while it is mapped: touching a page past its new
// end stops the process. The reference image is read-only input that nothing
// writes to, so this costs nothing in practice.
class FileRomSource : public ports::RomSource {
public:
    explicit FileRomSource(std::filesystem::path path) : path_(std::move(path)) {}

    ~FileRomSource() override;

    // The mapping belongs to one source, and the spans handed out point into
    // it, so a source is neither copied nor moved.
    FileRomSource(const FileRomSource&) = delete;
    FileRomSource& operator=(const FileRomSource&) = delete;
    FileRomSource(FileRomSource&&) = delete;
    FileRomSource& operator=(FileRomSource&&) = delete;

    [[nodiscard]] core::Result<core::ByteSpan> Contents() override;
    [[nodiscard]] std::string Describe() const override;

private:
    std::filesystem::path path_;
    std::span<const std::uint8_t> mapped_;
};

}  // namespace hexlith::adapters::filesystem

#endif  // HEXLITH_ADAPTERS_FILESYSTEM_FILE_ROM_SOURCE_H
