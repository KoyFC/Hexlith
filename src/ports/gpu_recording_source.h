#ifndef HEXLITH_PORTS_GPU_RECORDING_SOURCE_H
#define HEXLITH_PORTS_GPU_RECORDING_SOURCE_H

#include "core/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::ports {

// Where a GPU recording is read back from: its index, and the pages and lists
// the index names. docs/formats/gpu-recording.md is the format. A recording is
// read-only here; GpuRecorder is the side that writes one.
class GpuRecordingSource {
public:
    GpuRecordingSource() = default;
    virtual ~GpuRecordingSource() = default;

    GpuRecordingSource(const GpuRecordingSource&) = delete;
    GpuRecordingSource& operator=(const GpuRecordingSource&) = delete;
    GpuRecordingSource(GpuRecordingSource&&) = delete;
    GpuRecordingSource& operator=(GpuRecordingSource&&) = delete;

    // recording.txt, whole.
    [[nodiscard]] virtual core::Result<std::string> Index() = 0;

    // A page's bytes, by the SHA-256 the index names it with.
    [[nodiscard]] virtual core::Result<std::vector<std::uint8_t>> Page(std::string_view digest) = 0;

    // A command list's bytes, by the file name the index gives.
    [[nodiscard]] virtual core::Result<std::vector<std::uint8_t>> List(std::string_view file) = 0;

    [[nodiscard]] virtual std::string Describe() const = 0;
};

}  // namespace hexlith::ports

#endif  // HEXLITH_PORTS_GPU_RECORDING_SOURCE_H
