#ifndef HEXLITH_PORTS_GPU_BACKEND_H
#define HEXLITH_PORTS_GPU_BACKEND_H

#include "core/result.h"

#include <cstdint>
#include <span>
#include <string>

namespace hexlith::ports {

struct GpuBackendStatistics {
    std::uint64_t lists = 0;
    std::uint64_t draws = 0;
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
