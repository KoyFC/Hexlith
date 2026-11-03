#ifndef HEXLITH_ADAPTERS_FILESYSTEM_PNG_FRAME_SINK_H
#define HEXLITH_ADAPTERS_FILESYSTEM_PNG_FRAME_SINK_H

#include "ports/screen_sink.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hexlith::adapters::filesystem {

// Saves both screens of chosen frames as frame-<n>-top.png and
// frame-<n>-bottom.png, the names hexlith-gpu-replay gives them, so that
// scripts/compare_screens.py can hold a live run's pictures to another's. It
// sits in front of the sink that shows the screens, if there is one, and hands
// it every frame and its request to stop.
//
// A frame the game drew nothing in is not handed to a sink, and the picture of
// the next one that is stands for it (ADR 0027). A frame is therefore the
// picture of the last call that began at or before it, which is known only when
// the next call comes, so a picture is written one call after the frame it
// stands for. A frame after the last call is never written.
class PngFrameSink final : public ports::ScreenSink {
public:
    // `next` may be null, and must outlive this. `frames` is sorted by the caller.
    PngFrameSink(std::filesystem::path directory,
                 std::vector<std::uint64_t> frames,
                 ports::ScreenSink* next);

    [[nodiscard]] core::Status Present(std::uint64_t frame,
                                       const domain::gpu::Picture& top,
                                       const domain::gpu::Picture& bottom) override;

    [[nodiscard]] bool StopRequested() const override;

    [[nodiscard]] std::string Describe() const override;

private:
    core::Status Write(std::uint64_t frame);

    std::filesystem::path directory_;
    // Ascending, and the ones before `pending_` are written or were before the
    // first picture there was.
    std::vector<std::uint64_t> frames_;
    std::size_t pending_ = 0;
    ports::ScreenSink* next_;

    // The last picture handed to this, kept while a frame is still to come.
    std::uint64_t held_from_ = 0;
    bool holding_ = false;
    domain::gpu::Picture held_top_;
    domain::gpu::Picture held_bottom_;
};

}  // namespace hexlith::adapters::filesystem

#endif  // HEXLITH_ADAPTERS_FILESYSTEM_PNG_FRAME_SINK_H
