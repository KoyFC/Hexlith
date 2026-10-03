#ifndef HEXLITH_ADAPTERS_FILESYSTEM_FILE_ROM_SOURCE_H
#define HEXLITH_ADAPTERS_FILESYSTEM_FILE_ROM_SOURCE_H

#include "ports/rom_source.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace hexlith::adapters::filesystem {

// Reads a cartridge image from a file.
//
// The file is opened read-only and never written, renamed or moved, and a
// symbolic link is refused so that a path cannot be redirected after it was
// checked. ADR 0008 states both.
//
// The whole image is read into memory on the first call. At two gigabytes
// that is a real allocation, and a memory map would avoid the copy; the
// trade is a platform-specific implementation against about half a second
// once per run, and the port hands over a span either way, so swapping this
// for a mapping later changes nothing above it.
class FileRomSource : public ports::RomSource {
public:
    explicit FileRomSource(std::filesystem::path path) : path_(std::move(path)) {}

    [[nodiscard]] core::Result<core::ByteSpan> Contents() override;

    [[nodiscard]] std::string Describe() const override;

private:
    std::filesystem::path path_;
    std::optional<std::vector<std::uint8_t>> bytes_;
};

}  // namespace hexlith::adapters::filesystem

#endif  // HEXLITH_ADAPTERS_FILESYSTEM_FILE_ROM_SOURCE_H
