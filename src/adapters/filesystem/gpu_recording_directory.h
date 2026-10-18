#ifndef HEXLITH_ADAPTERS_FILESYSTEM_GPU_RECORDING_DIRECTORY_H
#define HEXLITH_ADAPTERS_FILESYSTEM_GPU_RECORDING_DIRECTORY_H

#include "ports/gpu_recorder.h"

#include <cstddef>
#include <filesystem>
#include <fstream>

namespace hexlith::adapters::filesystem {

// Writes a GPU recording as docs/formats/gpu-recording.md lays it out: an
// index of lines in recording.txt, each command list in lists/, and each page
// in pages/ under its SHA-256, so that a page that comes back to a content it
// had before costs nothing more.
//
// It writes beside the old recording, under a name ending in .partial, and
// puts the new one in place at Finish, so that a run that stops partway leaves
// the old recording whole.
class GpuRecordingDirectory : public ports::GpuRecorder {
public:
    explicit GpuRecordingDirectory(std::filesystem::path root) : root_(std::move(root)) {}

    // Creates the directory written into, emptying one left by a run that
    // stopped.
    core::Status Prepare();

    core::Status Command(std::uint64_t frame, const std::array<std::uint32_t, 8>& words) override;
    core::Status Page(std::uint64_t frame, std::uint32_t address, core::ByteSpan bytes) override;
    core::Status List(std::uint64_t frame, std::uint32_t address, core::ByteSpan bytes) override;
    core::Status Framebuffer(std::uint64_t frame,
                             std::uint32_t screen,
                             const std::array<std::uint32_t, FramebufferWords>& words) override;
    core::Status Finish() override;

    [[nodiscard]] std::string Describe() const override;

private:
    [[nodiscard]] std::filesystem::path Partial() const;
    core::Status Line(const std::string& text);

    std::filesystem::path root_;
    std::ofstream index_;
    std::size_t lists_ = 0;
};

}  // namespace hexlith::adapters::filesystem

#endif  // HEXLITH_ADAPTERS_FILESYSTEM_GPU_RECORDING_DIRECTORY_H
