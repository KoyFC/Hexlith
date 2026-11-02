#ifndef HEXLITH_APPLICATION_LIVE_DISPLAY_H
#define HEXLITH_APPLICATION_LIVE_DISPLAY_H

#include "core/result.h"
#include "domain/gpu/gpu_memory.h"
#include "domain/gpu/screen.h"
#include "ports/gpu_backend.h"
#include "ports/gpu_recorder.h"
#include "ports/screen_sink.h"

#include <cstdint>
#include <string>

namespace hexlith::application {

// Draws the game's GPU work as it arrives instead of writing it down: it
// takes what a GpuRecording hands to a recorder, and does what a replay does
// with the same events (ADR 0027). Pages are written into GPU memory, lists
// go to the backend, memory fills and display transfers run on the memory,
// and the screens' framebuffers are remembered.
//
// A frame's picture is taken at the first event of the frame that is not a
// framebuffer line, which come first, so what it shows is what a replay of
// the same recording shows for that frame, and is handed to the sink.
class LiveDisplay final : public ports::GpuRecorder {
public:
    LiveDisplay(domain::gpu::GpuMemory& memory,
                ports::GpuBackend& backend,
                ports::ScreenSink& sink);

    core::Status Command(std::uint64_t frame, const std::array<std::uint32_t, 8>& words) override;
    core::Status Page(std::uint64_t frame, std::uint32_t address, core::ByteSpan bytes) override;
    core::Status List(std::uint64_t frame, std::uint32_t address, core::ByteSpan bytes) override;
    core::Status Framebuffer(std::uint64_t frame,
                             std::uint32_t screen,
                             const std::array<std::uint32_t, FramebufferWords>& words) override;
    core::Status Finish() override;

    [[nodiscard]] std::string Describe() const override;

    [[nodiscard]] std::uint64_t Presented() const {
        return presented_;
    }

private:
    core::Status Before(std::uint64_t frame, bool framebuffer);

    domain::gpu::GpuMemory* memory_;
    ports::GpuBackend* backend_;
    ports::ScreenSink* sink_;
    domain::gpu::ScreenSetup screens_;
    // The first frame whose picture has not been handed on.
    std::uint64_t next_ = 0;
    bool started_ = false;
    std::uint64_t last_seen_ = 0;
    std::uint64_t presented_ = 0;
};

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_LIVE_DISPLAY_H
