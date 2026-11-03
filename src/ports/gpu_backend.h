#ifndef HEXLITH_PORTS_GPU_BACKEND_H
#define HEXLITH_PORTS_GPU_BACKEND_H

#include "core/result.h"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>

namespace hexlith::ports {

// Where a backend spent its time in RunCommandList. Whatever is left of
// `total` once these are taken out is the command processor and the vertices
// the CPU shades. A backend that does not split its time reports only `total`.
struct GpuBackendTimes {
    std::chrono::nanoseconds total{0};
    // Decoding textures from GPU memory for the shader to sample.
    std::chrono::nanoseconds textures{0};
    // Colour and depth buffers from memory into the backend's images, and back.
    std::chrono::nanoseconds upload{0};
    std::chrono::nanoseconds download{0};
    // Waiting for the device to finish a submission.
    std::chrono::nanoseconds wait{0};
    // Making the buffers a submission reads.
    std::chrono::nanoseconds allocate{0};
};

struct GpuBackendStatistics {
    std::uint64_t lists = 0;
    std::uint64_t draws = 0;
    // Runs of draws to one target, each a round trip through GPU memory in a
    // backend that keeps nothing between them (ADR 0022).
    std::uint64_t groups = 0;
    // Bytes of GPU memory the colour and depth buffers were read from and
    // written back to.
    std::uint64_t uploaded_bytes = 0;
    std::uint64_t downloaded_bytes = 0;
    // Device buffers made.
    std::uint64_t allocations = 0;
    GpuBackendTimes times;
    // A draw asked for something the backend does not implement.
    bool unsupported = false;
};

// What draws a game's command lists. A backend is made with the GPU memory
// the replay keeps, reads from it what a list needs, and leaves what the list
// drew there by the time RunCommandList returns, so that memory fills,
// display transfers and the next list see it. ADR 0022.
//
// A list is the unit of work: there is one call for each, never one for each
// draw or vertex.
class GpuBackend {
public:
    GpuBackend() = default;
    virtual ~GpuBackend() = default;

    GpuBackend(const GpuBackend&) = delete;
    GpuBackend& operator=(const GpuBackend&) = delete;
    GpuBackend(GpuBackend&&) = delete;
    GpuBackend& operator=(GpuBackend&&) = delete;

    [[nodiscard]] virtual core::Status RunCommandList(std::span<const std::uint8_t> list) = 0;

    [[nodiscard]] virtual GpuBackendStatistics Statistics() const = 0;

    [[nodiscard]] virtual std::string Describe() const = 0;
};

}  // namespace hexlith::ports

#endif  // HEXLITH_PORTS_GPU_BACKEND_H
