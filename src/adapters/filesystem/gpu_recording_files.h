#ifndef HEXLITH_ADAPTERS_FILESYSTEM_GPU_RECORDING_FILES_H
#define HEXLITH_ADAPTERS_FILESYSTEM_GPU_RECORDING_FILES_H

#include "ports/gpu_recording_source.h"

#include <filesystem>

namespace hexlith::adapters::filesystem {

// A GPU recording read from the directory GpuRecordingDirectory wrote:
// recording.txt, pages/<sha256>.bin and lists/<file>.
class GpuRecordingFiles : public ports::GpuRecordingSource {
public:
    explicit GpuRecordingFiles(std::filesystem::path root) : root_(std::move(root)) {}

    [[nodiscard]] core::Result<std::string> Index() override;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> Page(std::string_view digest) override;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> List(std::string_view file) override;
    [[nodiscard]] std::string Describe() const override;

private:
    std::filesystem::path root_;
};

}  // namespace hexlith::adapters::filesystem

#endif  // HEXLITH_ADAPTERS_FILESYSTEM_GPU_RECORDING_FILES_H
