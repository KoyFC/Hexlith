#ifndef HEXLITH_PORTS_GPU_RECORDER_H
#define HEXLITH_PORTS_GPU_RECORDER_H

#include "core/byte_reader.h"
#include "core/result.h"

#include <array>
#include <cstdint>
#include <string>

namespace hexlith::ports {

// Where the GPU's work is kept as the game hands it over, for a later phase
// to draw without running the game: every GX command, every command list's
// bytes, and the pages of GPU-visible memory as they stood before each list.
// docs/formats/gpu-recording.md is the format and ADR 0016 the reasons.
class GpuRecorder {
public:
    GpuRecorder() = default;
    virtual ~GpuRecorder() = default;

    GpuRecorder(const GpuRecorder&) = delete;
    GpuRecorder& operator=(const GpuRecorder&) = delete;
    GpuRecorder(GpuRecorder&&) = delete;
    GpuRecorder& operator=(GpuRecorder&&) = delete;

    virtual core::Status Command(std::uint64_t frame,
                                 const std::array<std::uint32_t, 8>& words) = 0;

    // A page that differs from what it held at the last page recorded for it.
    virtual core::Status Page(std::uint64_t frame, std::uint32_t address, core::ByteSpan bytes) = 0;

    virtual core::Status List(std::uint64_t frame, std::uint32_t address, core::ByteSpan bytes) = 0;

    // Everything written is in place once this answers.
    virtual core::Status Finish() = 0;

    [[nodiscard]] virtual std::string Describe() const = 0;
};

}  // namespace hexlith::ports

#endif  // HEXLITH_PORTS_GPU_RECORDER_H
