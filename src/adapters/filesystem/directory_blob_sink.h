#ifndef HEXLITH_ADAPTERS_FILESYSTEM_DIRECTORY_BLOB_SINK_H
#define HEXLITH_ADAPTERS_FILESYSTEM_DIRECTORY_BLOB_SINK_H

#include "ports/blob_sink.h"

#include <filesystem>

namespace hexlith::adapters::filesystem {

// Writes extracted content under one directory.
//
// Every path is checked against the root before anything is created, even
// though the RomFS parser already refuses a name that could escape. The
// parser is where a bad name is understood and this is where the consequence
// would land, and a filesystem image is untrusted input, so both ends check.
class DirectoryBlobSink : public ports::BlobSink {
public:
    explicit DirectoryBlobSink(std::filesystem::path root) : root_(std::move(root)) {}

    // Creates the root if it is not there. Kept out of the constructor so
    // that a failure is a Result rather than an exception from a member
    // initialiser.
    core::Status Prepare();

    core::Status Write(std::string_view path, core::ByteSpan data) override;

    core::Status WriteText(std::string_view path, std::string_view text) override;

    core::Status MakeDirectory(std::string_view path) override;

    [[nodiscard]] std::string Describe() const override;

private:
    [[nodiscard]] core::Result<std::filesystem::path> Resolve(std::string_view path) const;

    std::filesystem::path root_;
};

}  // namespace hexlith::adapters::filesystem

#endif  // HEXLITH_ADAPTERS_FILESYSTEM_DIRECTORY_BLOB_SINK_H
