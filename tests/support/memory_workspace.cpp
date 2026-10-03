#include "support/memory_workspace.h"

#include <format>

namespace hexlith::test {

core::Result<core::ByteSpan> MemoryRomSource::Contents() {
    if (failure_.has_value()) {
        return core::Error{*failure_};
    }
    return core::ByteSpan{bytes_};
}

std::string MemoryRomSource::Describe() const {
    return std::format("an image of {} bytes held in memory", bytes_.size());
}

core::Status MemoryBlobSink::Write(std::string_view path, core::ByteSpan data) {
    if (fail_on_.has_value() && path == *fail_on_) {
        return core::Error{std::format("writing '{}' was made to fail", path)};
    }

    const std::string name{path};
    if (files_.contains(name)) {
        return core::Error{std::format("'{}' was written twice", path)};
    }

    files_[name].assign(data.begin(), data.end());
    write_order_.push_back(name);
    return {};
}

core::Status MemoryBlobSink::WriteText(std::string_view path, std::string_view text) {
    return Write(path,
                 core::ByteSpan{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

core::Status MemoryBlobSink::MakeDirectory(std::string_view path) {
    directories_.emplace_back(path);
    return {};
}

std::string MemoryBlobSink::Describe() const {
    return "a workspace held in memory";
}

bool MemoryBlobSink::Holds(std::string_view path) const {
    return files_.contains(std::string{path});
}

}  // namespace hexlith::test
