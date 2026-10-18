#include "adapters/filesystem/gpu_recording_files.h"

#include <format>
#include <fstream>
#include <iterator>

namespace hexlith::adapters::filesystem {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view IndexFile = "recording.txt";
constexpr std::string_view ListsDirectory = "lists";
constexpr std::string_view PagesDirectory = "pages";
constexpr std::string_view PageExtension = ".bin";

core::Result<std::vector<std::uint8_t>> ReadAll(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return core::Error{std::format("'{}' could not be opened", path.string())};
    }
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{file}, {}};
    if (file.bad()) {
        return core::Error{std::format("'{}' could not be read", path.string())};
    }
    return bytes;
}

// A name the index gives, which must stay inside its directory.
bool IsPlainName(std::string_view name) {
    return !name.empty() && name.find('/') == std::string_view::npos && name != "." && name != "..";
}

}  // namespace

core::Result<std::string> GpuRecordingFiles::Index() {
    const core::Result<std::vector<std::uint8_t>> bytes = ReadAll(root_ / IndexFile);
    if (!bytes) {
        return bytes.Failure();
    }
    return std::string{bytes.Value().begin(), bytes.Value().end()};
}

core::Result<std::vector<std::uint8_t>> GpuRecordingFiles::Page(std::string_view digest) {
    if (!IsPlainName(digest)) {
        return core::Error{std::format("'{}' is not a page's name", digest)};
    }
    return ReadAll(root_ / PagesDirectory / (std::string{digest} + std::string{PageExtension}));
}

core::Result<std::vector<std::uint8_t>> GpuRecordingFiles::List(std::string_view file) {
    if (!IsPlainName(file)) {
        return core::Error{std::format("'{}' is not a list's name", file)};
    }
    return ReadAll(root_ / ListsDirectory / file);
}

std::string GpuRecordingFiles::Describe() const {
    return root_.string();
}

}  // namespace hexlith::adapters::filesystem
