#include "application/live_display.h"

#include "domain/gpu/gx_command.h"
#include "domain/gpu/memory_transfer.h"

#include <algorithm>
#include <format>
#include <optional>
#include <span>
#include <utility>

namespace hexlith::application {
namespace {

namespace gpu = domain::gpu;
using domain::services::Graphics;

// A command list's address and size, at words 1 and 2 of its command. Source:
// SubmitCmdListCommand in Azahar's gsp_command.h.
constexpr std::size_t ListAddressWord = 1;
constexpr std::size_t ListSizeWord = 2;

}  // namespace

gpu::GpuMemory GpuMemoryOf(BootedTitle& title) {
    domain::kernel::Kernel& kernel = title.Kernel();
    return gpu::GpuMemory{kernel.Vram()->Bytes(), kernel.Space().Region()->Bytes()};
}

LiveDisplay::LiveDisplay(gpu::GpuMemory& memory,
                         ports::GpuBackend& backend,
                         ports::ScreenSink& sink)
    : memory_(&memory), backend_(&backend), sink_(&sink) {}

void LiveDisplay::Watch(Graphics& gpu) {
    gpu.OnCommand([this](const Graphics::GxCommand& command) {
        Keep(Command(command.frame, command.words));
    });
    gpu.OnFramebuffer([this](const Graphics::FramebufferSwap& swap) {
        const Graphics::FramebufferInfo& info = swap.info;
        Keep(Framebuffer(swap.frame,
                         swap.screen,
                         {info.active,
                          info.address_left,
                          info.address_right,
                          info.stride,
                          info.format,
                          info.shown,
                          info.unknown}));
    });
}

void LiveDisplay::Keep(const core::Status& status) {
    if (!status && failure_) {
        failure_ = status;
    }
}

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

// gsp::Gpu tells its observer of a command before it carries it out, so a
// display transfer run here is done where the console would do it.
core::Status LiveDisplay::Command(std::uint64_t frame, const std::array<std::uint32_t, 8>& words) {
    core::Status before = Before(frame, false);
    if (!before) {
        return before;
    }
    switch (gpu::CommandId(words)) {
    case gpu::GxCommandId::ProcessCommandList: {
        const std::uint32_t size = words.at(ListSizeWord);
        const std::optional<std::uint32_t> physical =
            gpu::GpuMemory::PhysicalFromVirtual(words.at(ListAddressWord));
        const std::span<const std::uint8_t> bytes =
            physical ? std::as_const(*memory_).Bytes(*physical, size)
                     : std::span<const std::uint8_t>{};
        if (bytes.size() != size) {
            return core::Error{std::format("a command list at {:08X} is not in GPU memory",
                                           words.at(ListAddressWord))};
        }
        // A list is read while it draws into the same memory, so it is read
        // from a copy.
        list_.assign(bytes.begin(), bytes.end());
        return backend_->RunCommandList(list_);
    }
    case gpu::GxCommandId::DisplayTransfer:
        if (const std::optional<gpu::TransferConfig> transfer = gpu::Transfer(words)) {
            static_cast<void>(gpu::RunTransfer(*memory_, *transfer));
        }
        return {};
    default:
        return {};
    }
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

core::Status LiveDisplay::Finish() {
    Keep(Before(last_seen_ + 1, false));
    return failure_;
}

std::string LiveDisplay::Describe() const {
    return std::format(
        "a live display on {}, drawn by {}", sink_->Describe(), backend_->Describe());
}

}  // namespace hexlith::application
