#ifndef HEXLITH_APPLICATION_GPU_RENDER_H
#define HEXLITH_APPLICATION_GPU_RENDER_H

#include "core/result.h"
#include "domain/gpu/fragment_stage.h"
#include "domain/gpu/screen.h"
#include "ports/gpu_recording_source.h"

#include <cstdint>
#include <functional>
#include <set>

namespace hexlith::application {

// What both screens showed in a frame.
struct RenderedFrame {
    std::uint64_t frame = 0;
    domain::gpu::Picture top;
    domain::gpu::Picture bottom;
};

struct GpuRenderOptions {
    domain::gpu::Sampling sampling = domain::gpu::Sampling::Hardware;
    // The frames to hand on. The replay stops after the last.
    std::set<std::uint64_t> frames;
};

// What a render went through.
struct GpuRenderSummary {
    std::uint64_t lists = 0;
    std::uint64_t fills = 0;
    std::uint64_t transfers = 0;
    std::uint64_t draws = 0;
    // A transfer, shader or fragment setting the reference does not
    // implement was met.
    bool unsupported = false;
};

// Replays a GPU recording through the reference renderer, carrying out its
// memory fills and transfers, and hands on what the screens show in each
// frame asked for: the framebuffers the frame's framebuffer lines chose, as
// every event of the frames before left them (docs/formats/gpu-recording.md).
// A frame past the recording's end is not handed on.
core::Result<GpuRenderSummary>
RenderGpuRecording(ports::GpuRecordingSource& source,
                   const GpuRenderOptions& options,
                   const std::function<core::Status(const RenderedFrame&)>& sink);

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_GPU_RENDER_H
