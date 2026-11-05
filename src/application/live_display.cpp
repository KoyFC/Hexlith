#include "application/live_display.h"

#include "domain/gpu/gx_command.h"
#include "domain/gpu/memory_transfer.h"

#include <algorithm>
#include <chrono>
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

// The time of a call, added to a total when it returns.
template<typename Call>
auto Timed(std::chrono::nanoseconds& total, const Call& call) {
    const auto began = std::chrono::steady_clock::now();
    auto result = call();
    total += std::chrono::steady_clock::now() - began;
    return result;
}

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
    for (const std::uint32_t screen : {gpu::ScreenSetup::Top, gpu::ScreenSetup::Bottom}) {
        const gpu::MemoryRange range = screens_.ShownRange(screen);
        if (range.size != 0) {
            if (core::Status ready = backend_->BeforeRead(range.start, range.size); !ready) {
                return ready;
            }
        }
    }
    return Timed(work_.present_time, [&] {
        return sink_->Present(shown,
                              screens_.Compose(*memory_, gpu::ScreenSetup::Top),
                              screens_.Compose(*memory_, gpu::ScreenSetup::Bottom));
    });
}

// Tells the backend what a command is about to read and write, ahead of
// whoever carries it out.
core::Status LiveDisplay::Announce(const std::array<std::uint32_t, 8>& words) {
    const gpu::GxMemoryUse use = gpu::MemoryUsed(words);
    for (const gpu::MemoryRange& range : use.reads) {
        if (core::Status ready = backend_->BeforeRead(range.start, range.size); !ready) {
            return ready;
        }
    }
    for (const gpu::MemoryRange& range : use.writes) {
        if (core::Status ready = backend_->BeforeWrite(range.start, range.size); !ready) {
            return ready;
        }
    }
    return {};
}

// gsp::Gpu tells its observer of a command before it carries it out, so a
// display transfer run here is done where the console would do it, and the
// backend can be told before anything touches the memory it draws in.
core::Status LiveDisplay::Command(std::uint64_t frame, const std::array<std::uint32_t, 8>& words) {
    if (const core::Status before = Before(frame, false); !before) {
        return before;
    }
    if (gpu::CommandId(words) == gpu::GxCommandId::DisplayTransfer) {
        return DisplayTransfer(words);
    }
    if (const core::Status announced = Announce(words); !announced) {
        return announced;
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
        ++work_.lists;
        return Timed(work_.lists_time, [&] { return backend_->RunCommandList(list_); });
    }
    default:
        return {};
    }
}

// The backend is offered a transfer first, and one that takes it owes memory
// the output when it is read, so nothing is announced or copied here for it
// (ADR 0033). One that declines it leaves the transfer to memory.
core::Status LiveDisplay::DisplayTransfer(const std::array<std::uint32_t, 8>& words) {
    const std::optional<gpu::TransferConfig> transfer = gpu::Transfer(words);
    if (!transfer) {
        return {};
    }
    ++work_.transfers;
    const core::Result<bool> taken =
        Timed(work_.transfers_time, [&] { return backend_->RunDisplayTransfer(*transfer); });
    if (!taken) {
        return taken.Failure();
    }
    if (taken.Value()) {
        return {};
    }
    if (const core::Status announced = Announce(words); !announced) {
        return announced;
    }
    Timed(work_.transfers_time, [&] {
        static_cast<void>(gpu::RunTransfer(*memory_, *transfer));
        return 0;
    });
    return {};
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
    // The run is over, and whatever reads the title's memory next sees it true.
    Keep(backend_->BeforeRead(gpu::GpuMemory::VramBase, gpu::GpuMemory::VramSize));
    Keep(backend_->BeforeRead(gpu::GpuMemory::FcramBase, gpu::GpuMemory::FcramSize));
    return failure_;
}

std::string LiveDisplay::Describe() const {
    return std::format(
        "a live display on {}, drawn by {}", sink_->Describe(), backend_->Describe());
}

}  // namespace hexlith::application
