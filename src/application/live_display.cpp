#include "application/live_display.h"

#include "domain/gpu/gx_command.h"
#include "domain/gpu/memory_transfer.h"

#include <format>

namespace hexlith::application {
namespace {

namespace gpu = domain::gpu;

}  // namespace

LiveDisplay::LiveDisplay(gpu::GpuMemory& memory,
                         ports::GpuBackend& backend,
                         ports::ScreenSink& sink)
    : memory_(&memory), backend_(&backend), sink_(&sink) {}

// A frame that had no event of its own is not shown: the picture of the next
// one that does is the same, since nothing changed in between.
core::Status LiveDisplay::Before(std::uint64_t frame, bool framebuffer) {
    if (!started_) {
        // The game's first frame is the first one there is a picture of.
        started_ = true;
        next_ = frame;
    }
    last_seen_ = frame;
    if (next_ > frame || (next_ == frame && framebuffer)) {
        return {};
    }
    const std::uint64_t shown = next_;
    next_ = frame + 1;
    ++presented_;
    return sink_->Present(shown,
                          screens_.Compose(*memory_, gpu::ScreenSetup::Top),
                          screens_.Compose(*memory_, gpu::ScreenSetup::Bottom));
}

core::Status LiveDisplay::Command(std::uint64_t frame, const std::array<std::uint32_t, 8>& words) {
    const core::Status before = Before(frame, false);
    if (!before) {
        return before;
    }
    for (const gpu::MemoryFillConfig& fill : gpu::MemoryFills(words)) {
        static_cast<void>(gpu::RunMemoryFill(*memory_, fill));
    }
    if (const std::optional<gpu::TransferConfig> transfer = gpu::Transfer(words)) {
        static_cast<void>(gpu::RunTransfer(*memory_, *transfer));
    }
    return {};
}

core::Status
LiveDisplay::Page(std::uint64_t /*frame*/, std::uint32_t address, core::ByteSpan bytes) {
    const std::optional<std::uint32_t> physical = gpu::GpuMemory::PhysicalFromVirtual(address);
    if (!physical.has_value() || !memory_->Write(*physical, bytes)) {
        return core::Error{std::format("a page at {:08X} is not in GPU memory", address)};
    }
    return {};
}

core::Status
LiveDisplay::List(std::uint64_t frame, std::uint32_t /*address*/, core::ByteSpan bytes) {
    const core::Status before = Before(frame, false);
    return before ? backend_->RunCommandList(bytes) : before;
}

core::Status LiveDisplay::Framebuffer(std::uint64_t frame,
                                      std::uint32_t screen,
                                      const std::array<std::uint32_t, FramebufferWords>& words) {
    const core::Status before = Before(frame, true);
    if (before) {
        screens_.Apply(gpu::RecordedFramebuffer{.frame = frame, .screen = screen, .words = words});
    }
    return before;
}

// What the last frame left, which no later event takes a picture of.
core::Status LiveDisplay::Finish() {
    return Before(last_seen_ + 1, false);
}

std::string LiveDisplay::Describe() const {
    return std::format(
        "a live display on {}, drawn by {}", sink_->Describe(), backend_->Describe());
}

}  // namespace hexlith::application
