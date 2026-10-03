#ifndef HEXLITH_TESTS_SUPPORT_MEMORY_WORKSPACE_H
#define HEXLITH_TESTS_SUPPORT_MEMORY_WORKSPACE_H

#include "ports/blob_sink.h"
#include "ports/rom_source.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

// The two ports, implemented in memory.
//
// A test can then run the whole extraction and look at what it produced,
// without a temporary directory, without the host filesystem's opinions about
// names, and without anything left behind when a test fails.
namespace hexlith::test {

class MemoryRomSource : public ports::RomSource {
public:
    explicit MemoryRomSource(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

    [[nodiscard]] core::Result<core::ByteSpan> Contents() override;

    [[nodiscard]] std::string Describe() const override;

    // Makes the source fail, so that a use case's handling of an unreadable
    // image is covered too.
    void FailWith(std::string message) {
        failure_ = std::move(message);
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::optional<std::string> failure_;
};

class MemoryBlobSink : public ports::BlobSink {
public:
    core::Status Write(std::string_view path, core::ByteSpan data) override;

    core::Status WriteText(std::string_view path, std::string_view text) override;

    core::Status MakeDirectory(std::string_view path) override;

    [[nodiscard]] std::string Describe() const override;

    // Sorted by path, since that is what a test wants to compare against.
    [[nodiscard]] const std::map<std::string, std::vector<std::uint8_t>>& Files() const {
        return files_;
    }

    [[nodiscard]] const std::vector<std::string>& Directories() const {
        return directories_;
    }

    // The order the use case wrote in, which is what a test checks when the
    // question is whether validation happened before anything was written.
    [[nodiscard]] const std::vector<std::string>& WriteOrder() const {
        return write_order_;
    }

    [[nodiscard]] bool Holds(std::string_view path) const;

    void FailOn(std::string path) {
        fail_on_ = std::move(path);
    }

private:
    std::map<std::string, std::vector<std::uint8_t>> files_;
    std::vector<std::string> directories_;
    std::vector<std::string> write_order_;
    std::optional<std::string> fail_on_;
};

}  // namespace hexlith::test

#endif  // HEXLITH_TESTS_SUPPORT_MEMORY_WORKSPACE_H
