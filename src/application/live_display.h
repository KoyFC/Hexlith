#ifndef HEXLITH_APPLICATION_LIVE_DISPLAY_H
#define HEXLITH_APPLICATION_LIVE_DISPLAY_H

#include "application/boot.h"
#include "core/result.h"
#include "domain/gpu/gpu_memory.h"
#include "domain/gpu/screen.h"
#include "ports/gpu_backend.h"
#include "ports/screen_sink.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace hexlith::application {

// The title's own VRAM and FCRAM region as the GPU sees them, so that what a
// backend draws is drawn where the game and gsp::Gpu find it. The title has
// to outlive it.
domain::gpu::GpuMemory GpuMemoryOf(BootedTitle& title);

// Draws the game's GPU work as gsp::Gpu takes it from the game, in the game's
// own memory (ADR 0027). Lists go to the backend and display transfers run on
// the memory; memory fills, texture copies and DMA are gsp::Gpu's own work on
// the same memory and are left to it. The screens' framebuffers are
// remembered.
//
// A frame's picture is taken at the first event of the frame that is not a
// framebuffer line, which come first, so what it shows is what a replay of a
// recording of the same run shows for that frame, and is handed to the sink.
class LiveDisplay final {
public:
    static constexpr std::size_t FramebufferWords = 7;

    // What the display did and how long each kind of work took, the backend's
    // own time in RunCommandList included in `lists_time`.
    struct WorkDone {
        std::uint64_t lists = 0;
        std::uint64_t transfers = 0;
        std::chrono::nanoseconds lists_time{0};
        std::chrono::nanoseconds transfers_time{0};
        // Composing the screens from memory and handing them to the sink.
        std::chrono::nanoseconds present_time{0};
    };

    // The memory is the one the backend draws in.
    LiveDisplay(domain::gpu::GpuMemory& memory,
                ports::GpuBackend& backend,
                ports::ScreenSink& sink);

    LiveDisplay(const LiveDisplay&) = delete;
    LiveDisplay& operator=(const LiveDisplay&) = delete;
    LiveDisplay(LiveDisplay&&) = delete;
    LiveDisplay& operator=(LiveDisplay&&) = delete;
    ~LiveDisplay() = default;

    // Takes gsp::Gpu's commands and framebuffers from now on, which no other
    // observer may then take.
    void Watch(domain::services::Graphics& gpu);

    core::Status Command(std::uint64_t frame, const std::array<std::uint32_t, 8>& words);
    core::Status Framebuffer(std::uint64_t frame,
                             std::uint32_t screen,
                             const std::array<std::uint32_t, FramebufferWords>& words);

    // Shows what the last frame left, and answers with the first failure met
    // while watching.
    core::Status Finish();

    [[nodiscard]] std::string Describe() const;

    [[nodiscard]] std::uint64_t Presented() const {
        return presented_;
    }

    [[nodiscard]] const WorkDone& Work() const {
        return work_;
    }

private:
    core::Status Before(std::uint64_t frame, bool framebuffer);
    core::Status Announce(const std::array<std::uint32_t, 8>& words);
    core::Status DisplayTransfer(const std::array<std::uint32_t, 8>& words);
    void Keep(const core::Status& status);

    domain::gpu::GpuMemory* memory_;
    ports::GpuBackend* backend_;
    ports::ScreenSink* sink_;
    domain::gpu::ScreenSetup screens_;
    std::vector<std::uint8_t> list_;
    // The first frame whose picture has not been handed on.
    std::uint64_t next_ = 0;
    bool started_ = false;
    std::uint64_t last_seen_ = 0;
    std::uint64_t presented_ = 0;
    WorkDone work_;
    core::Status failure_;
};

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_LIVE_DISPLAY_H
