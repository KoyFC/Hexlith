#include "adapters/filesystem/file_rom_source.h"

#include <format>
#include <fstream>

namespace hexlith::adapters::filesystem {
namespace {

namespace fs = std::filesystem;

}  // namespace

core::Result<core::ByteSpan> FileRomSource::Contents() {
    if (bytes_.has_value()) {
        return core::ByteSpan{*bytes_};
    }

    std::error_code failure;

    // Checked on the path as given rather than after resolution, which is the
    // point: a link is refused instead of followed.
    if (fs::is_symlink(path_, failure)) {
        return core::Error{std::format(
            "'{}' is a symbolic link. Hexlith refuses one so that a path cannot be redirected "
            "after it was checked",
            path_.string())};
    }
    if (!fs::is_regular_file(path_, failure)) {
        return core::Error{std::format("'{}' is not a file", path_.string())};
    }

    const std::uintmax_t size = fs::file_size(path_, failure);
    if (failure) {
        return core::Error{std::format(
            "the size of '{}' could not be read: {}", path_.string(), failure.message())};
    }
    if (size == 0) {
        return core::Error{std::format("'{}' is empty", path_.string())};
    }

    std::ifstream file(path_, std::ios::binary);
    if (!file) {
        return core::Error{std::format("'{}' could not be opened for reading", path_.string())};
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(file.gcount()) != bytes.size()) {
        return core::Error{std::format(
            "'{}' ended after {} of {} bytes", path_.string(), file.gcount(), bytes.size())};
    }

    bytes_ = std::move(bytes);
    return core::ByteSpan{*bytes_};
}

std::string FileRomSource::Describe() const {
    return path_.string();
}

}  // namespace hexlith::adapters::filesystem
