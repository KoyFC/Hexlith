#include "adapters/filesystem/file_rom_source.h"

#include <cerrno>
#include <cstring>
#include <format>
#include <sys/mman.h>

#include <fcntl.h>
#include <unistd.h>

namespace hexlith::adapters::filesystem {
namespace {

namespace fs = std::filesystem;

// Closes a descriptor when it goes out of scope. The mapping outlives it.
class Descriptor {
public:
    explicit Descriptor(int descriptor) : descriptor_(descriptor) {}

    ~Descriptor() {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }

    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&&) = delete;
    Descriptor& operator=(Descriptor&&) = delete;

    [[nodiscard]] int Get() const {
        return descriptor_;
    }

private:
    int descriptor_;
};

}  // namespace

FileRomSource::~FileRomSource() {
    if (!mapped_.empty()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): munmap takes the address mmap gave
        munmap(const_cast<std::uint8_t*>(mapped_.data()), mapped_.size());
    }
}

core::Result<core::ByteSpan> FileRomSource::Contents() {
    if (!mapped_.empty()) {
        return mapped_;
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

    // O_NOFOLLOW refuses a link that took the file's place after the check
    // above.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is the POSIX call
    const Descriptor file{open(path_.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
    if (file.Get() < 0) {
        return core::Error{std::format(
            "'{}' could not be opened for reading: {}", path_.string(), std::strerror(errno))};
    }

    const auto length = static_cast<std::size_t>(size);
    const void* const address = mmap(nullptr, length, PROT_READ, MAP_PRIVATE, file.Get(), 0);
    if (address == MAP_FAILED) {
        return core::Error{
            std::format("'{}' could not be mapped: {}", path_.string(), std::strerror(errno))};
    }

    mapped_ = std::span<const std::uint8_t>{static_cast<const std::uint8_t*>(address), length};
    return mapped_;
}

std::string FileRomSource::Describe() const {
    return path_.string();
}

}  // namespace hexlith::adapters::filesystem
