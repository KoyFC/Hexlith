#include "adapters/filesystem/gpu_recording_directory.h"

#include "core/sha256.h"

#include <format>

namespace hexlith::adapters::filesystem {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view IndexFile = "recording.txt";
constexpr std::string_view ListsDirectory = "lists";
constexpr std::string_view PagesDirectory = "pages";
constexpr std::string_view PartialSuffix = ".partial";
constexpr std::string_view OldSuffix = ".old";
constexpr std::string_view FormatLine = "# hexlith gpu recording, format 1";

core::Status WriteBytes(const fs::path& path, core::ByteSpan bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        return core::Error{std::format("'{}' could not be written", path.string())};
    }
    return {};
}

}  // namespace

fs::path GpuRecordingDirectory::Partial() const {
    fs::path partial = root_;
    partial += PartialSuffix;
    return partial;
}

core::Status GpuRecordingDirectory::Prepare() {
    std::error_code failure;
    fs::remove_all(Partial(), failure);
    fs::create_directories(Partial() / ListsDirectory, failure);
    fs::create_directories(Partial() / PagesDirectory, failure);
    if (failure) {
        return core::Error{
            std::format("'{}' could not be created: {}", Partial().string(), failure.message())};
    }
    index_.open(Partial() / IndexFile, std::ios::trunc);
    if (!index_) {
        return core::Error{
            std::format("'{}' could not be opened", (Partial() / IndexFile).string())};
    }
    return Line(std::string{FormatLine});
}

core::Status GpuRecordingDirectory::Line(const std::string& text) {
    index_ << text << '\n';
    if (!index_) {
        return core::Error{
            std::format("'{}' could not be written", (Partial() / IndexFile).string())};
    }
    return {};
}

core::Status GpuRecordingDirectory::Command(std::uint64_t frame,
                                            const std::array<std::uint32_t, 8>& words) {
    std::string line = std::format("{} gx", frame);
    for (const std::uint32_t word : words) {
        line += std::format(" {:08X}", word);
    }
    return Line(line);
}

core::Status
GpuRecordingDirectory::Page(std::uint64_t frame, std::uint32_t address, core::ByteSpan bytes) {
    const std::string digest = core::Sha256::Of(bytes).ToHex();
    const fs::path path = Partial() / PagesDirectory / (digest + ".bin");
    std::error_code failure;
    if (!fs::exists(path, failure)) {
        core::Status written = WriteBytes(path, bytes);
        if (!written) {
            return written;
        }
    }
    return Line(std::format("{} page {:08X} {}", frame, address, digest));
}

core::Status
GpuRecordingDirectory::List(std::uint64_t frame, std::uint32_t address, core::ByteSpan bytes) {
    const std::string name = std::format("{:06}.bin", lists_);
    ++lists_;
    core::Status written = WriteBytes(Partial() / ListsDirectory / name, bytes);
    if (!written) {
        return written;
    }
    return Line(std::format("{} list {:08X} {:08X} {}", frame, address, bytes.size(), name));
}

core::Status GpuRecordingDirectory::Finish() {
    index_.close();
    fs::path old = root_;
    old += OldSuffix;
    std::error_code failure;
    fs::remove_all(old, failure);
    if (fs::exists(root_, failure)) {
        fs::rename(root_, old, failure);
    }
    fs::rename(Partial(), root_, failure);
    if (failure) {
        return core::Error{
            std::format("'{}' could not be put in place: {}", root_.string(), failure.message())};
    }
    fs::remove_all(old, failure);
    return {};
}

std::string GpuRecordingDirectory::Describe() const {
    return root_.string();
}

}  // namespace hexlith::adapters::filesystem
