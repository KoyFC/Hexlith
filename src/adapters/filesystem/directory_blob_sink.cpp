#include "adapters/filesystem/directory_blob_sink.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <ranges>

namespace hexlith::adapters::filesystem {
namespace {

namespace fs = std::filesystem;

}  // namespace

core::Status DirectoryBlobSink::Prepare() {
    std::error_code failure;
    fs::create_directories(root_, failure);
    if (failure) {
        return core::Error{
            std::format("'{}' could not be created: {}", root_.string(), failure.message())};
    }
    if (!fs::is_directory(root_, failure)) {
        return core::Error{std::format("'{}' is not a directory", root_.string())};
    }
    return {};
}

core::Result<fs::path> DirectoryBlobSink::Resolve(std::string_view path) const {
    if (path.empty()) {
        return core::Error{"an empty path was given"};
    }

    const fs::path relative{path};
    if (relative.is_absolute()) {
        return core::Error{std::format("'{}' is absolute, and only paths under the extraction "
                                       "directory can be written",
                                       path)};
    }

    for (const fs::path& part : relative) {
        if (part == ".." || part == ".") {
            return core::Error{
                std::format("'{}' holds '{}', which would reach outside the extraction directory",
                            path,
                            part.string())};
        }
    }

    const fs::path resolved = (root_ / relative).lexically_normal();
    const fs::path normal_root = root_.lexically_normal();

    // Belt and braces: the component check above already rules this out, but
    // the comparison costs nothing and this is the last place before a file
    // is created.
    const auto within = std::ranges::mismatch(normal_root, resolved);
    if (within.in1 != normal_root.end()) {
        return core::Error{std::format("'{}' resolves outside '{}'", path, normal_root.string())};
    }

    return resolved;
}

core::Status DirectoryBlobSink::Write(std::string_view path, core::ByteSpan data) {
    const core::Result<fs::path> resolved = Resolve(path);
    if (!resolved) {
        return resolved.Failure();
    }

    std::error_code failure;
    fs::create_directories(resolved.Value().parent_path(), failure);
    if (failure) {
        return core::Error{std::format(
            "the directory for '{}' could not be created: {}", path, failure.message())};
    }

    std::ofstream file(resolved.Value(), std::ios::binary | std::ios::trunc);
    if (!file) {
        return core::Error{std::format("'{}' could not be opened for writing", path)};
    }

    if (!data.empty()) {
        file.write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
    }

    // Closed explicitly, because a write that failed on flush is one that
    // left a file shorter than the manifest says it is.
    file.close();
    if (!file) {
        return core::Error{
            std::format("'{}' could not be written in full, so it may be incomplete", path)};
    }
    return {};
}

core::Status DirectoryBlobSink::WriteText(std::string_view path, std::string_view text) {
    return Write(path,
                 core::ByteSpan{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

core::Status DirectoryBlobSink::MakeDirectory(std::string_view path) {
    const core::Result<fs::path> resolved = Resolve(path);
    if (!resolved) {
        return resolved.Failure();
    }

    std::error_code failure;
    fs::create_directories(resolved.Value(), failure);
    if (failure) {
        return core::Error{std::format("'{}' could not be created: {}", path, failure.message())};
    }
    return {};
}

std::string DirectoryBlobSink::Describe() const {
    return root_.string();
}

}  // namespace hexlith::adapters::filesystem
