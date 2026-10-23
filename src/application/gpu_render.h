#ifndef HEXLITH_APPLICATION_GPU_RENDER_H
#define HEXLITH_APPLICATION_GPU_RENDER_H

#include "core/result.h"
#include "domain/gpu/command_processor.h"
#include "domain/gpu/fragment_stage.h"
#include "domain/gpu/gpu_memory.h"
#include "domain/gpu/reference_renderer.h"
#include "domain/gpu/screen.h"
#include "ports/gpu_backend.h"
#include "ports/gpu_recording_source.h"

#include <cstdint>
#include <functional>
#include <set>

namespace hexlith::application {

// The reference renderer as a backend: every list drawn in software into the
// memory it was made with.
class ReferenceGpuBackend final : public ports::GpuBackend {
public:
    ReferenceGpuBackend(domain::gpu::GpuMemory& memory, domain::gpu::Sampling sampling);

    [[nodiscard]] core::Status RunCommandList(std::span<const std::uint8_t> list) override;
    [[nodiscard]] ports::GpuBackendStatistics Statistics() const override;
    [[nodiscard]] std::string Describe() const override;

private:
    domain::gpu::PicaState state_;
    domain::gpu::ReferenceRenderer renderer_;
    domain::gpu::CommandProcessor<domain::gpu::ReferenceRenderer> processor_;
    domain::gpu::Sampling sampling_;
    std::uint64_t lists_ = 0;
};

// What both screens showed in a frame.
struct RenderedFrame {
    std::uint64_t frame = 0;
    domain::gpu::Picture top;
    domain::gpu::Picture bottom;
};

// What a render went through besides the backend's work.
struct GpuRenderSummary {
    std::uint64_t fills = 0;
    std::uint64_t transfers = 0;
    // A transfer the engine does not implement was met.
    bool unsupported = false;
};

// Replays a GPU recording into memory, the lists through a backend made with
// that memory, the memory fills and transfers on the memory itself, and hands
// on what the screens show in each frame asked for: the framebuffers the
// frame's framebuffer lines chose, as every event of the frames before left
// them (docs/formats/gpu-recording.md). A frame past the recording's end is
// not handed on, and the replay stops after the last frame asked for.
core::Result<GpuRenderSummary>
RenderGpuRecording(ports::GpuRecordingSource& source,
                   domain::gpu::GpuMemory& memory,
                   ports::GpuBackend& backend,
                   const std::set<std::uint64_t>& frames,
                   const std::function<core::Status(const RenderedFrame&)>& sink);

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_GPU_RENDER_H
