#include "adapters/filesystem/png_frame_sink.h"

#include "adapters/filesystem/png_file.h"

#include <format>
#include <utility>

namespace hexlith::adapters::filesystem {

PngFrameSink::PngFrameSink(std::filesystem::path directory,
                           std::vector<std::uint64_t> frames,
                           ports::ScreenSink* next)
    : directory_(std::move(directory)), frames_(std::move(frames)), next_(next) {}

core::Status PngFrameSink::Write(std::uint64_t frame) {
    core::Status written = WritePng(directory_ / std::format("frame-{}-top.png", frame), held_top_);
    if (written) {
        written = WritePng(directory_ / std::format("frame-{}-bottom.png", frame), held_bottom_);
    }
    return written;
}

core::Status PngFrameSink::Present(std::uint64_t frame,
                                   const domain::gpu::Picture& top,
                                   const domain::gpu::Picture& bottom) {
    core::Status written;
    // This call shows where the held picture stops standing for frames.
    while (pending_ < frames_.size() && frames_[pending_] < frame) {
        if (holding_ && frames_[pending_] >= held_from_ && written) {
            written = Write(frames_[pending_]);
        }
        ++pending_;
    }
    if (pending_ < frames_.size()) {
        holding_ = true;
        held_from_ = frame;
        held_top_ = top;
        held_bottom_ = bottom;
    }
    // The window is shown whether or not a picture could be kept.
    if (next_ != nullptr) {
        if (core::Status shown = next_->Present(frame, top, bottom); !shown) {
            return shown;
        }
    }
    return written;
}

bool PngFrameSink::StopRequested() const {
    return next_ != nullptr && next_->StopRequested();
}

std::string PngFrameSink::Describe() const {
    return std::format("{} frames saved in {}{}",
                       frames_.size(),
                       directory_.string(),
                       next_ != nullptr ? std::format(", then {}", next_->Describe()) : "");
}

}  // namespace hexlith::adapters::filesystem
