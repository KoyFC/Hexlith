#ifndef HEXLITH_APPLICATION_GPU_REPLAY_H
#define HEXLITH_APPLICATION_GPU_REPLAY_H

#include "core/result.h"
#include "domain/gpu/gpu_memory.h"
#include "domain/gpu/recording_index.h"
#include "ports/gpu_recording_source.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace hexlith::application {

// What a replay hands on, event by event. Each is called with GPU-visible
// memory as it stood when the game did what the event records; a list, once
// every page recorded before it has been written. A failure stops the replay
// and is its answer. Any of them may be left empty.
struct GpuReplayVisitor {
    std::function<core::Status(const domain::gpu::RecordedCommand&)> command;
    std::function<core::Status(const domain::gpu::RecordedList&, std::span<const std::uint8_t>)>
        list;
    std::function<core::Status(const domain::gpu::RecordedFramebuffer&)> framebuffer;
};

// Replays a GPU recording into memory, the way docs/formats/gpu-recording.md
// describes: memory starts as it is handed in, every page is written as it
// comes, and the other events go to the visitor. With a last frame, it stops
// before the first event of a later one. Every call starts from the first
// event.
core::Status ReplayGpuRecording(ports::GpuRecordingSource& source,
                                domain::gpu::GpuMemory& memory,
                                const GpuReplayVisitor& visitor,
                                std::optional<std::uint64_t> last_frame = std::nullopt);

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_GPU_REPLAY_H
