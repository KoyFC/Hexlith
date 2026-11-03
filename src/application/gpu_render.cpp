#include "application/gpu_render.h"

#include "application/gpu_replay.h"
#include "domain/gpu/gx_command.h"
#include "domain/gpu/memory_transfer.h"

#include <algorithm>
#include <format>

namespace hexlith::application {
namespace {

namespace gpu = domain::gpu;

// The size every recorded page has.
constexpr std::uint32_t PageSize = 0x1000;

class Renderer {
public:
    Renderer(gpu::GpuMemory& memory,
             ports::GpuBackend& backend,
             const std::set<std::uint64_t>& frames,
             const std::function<core::Status(const RenderedFrame&)>& sink)
        : memory_(&memory), backend_(&backend), sink_(&sink), pending_(frames) {}

    // A frame's picture is taken at its first event that is not a
    // framebuffer line, which come first in a frame.
    core::Status Before(std::uint64_t frame, bool framebuffer) {
        seen_ = std::max(seen_, frame);
        while (!pending_.empty() &&
               (*pending_.begin() < frame || (*pending_.begin() == frame && !framebuffer))) {
            const std::uint64_t wanted = *pending_.begin();
            pending_.erase(pending_.begin());
            for (const std::uint32_t screen : {gpu::ScreenSetup::Top, gpu::ScreenSetup::Bottom}) {
                const gpu::MemoryRange range = screens_.ShownRange(screen);
                if (range.size != 0) {
                    if (core::Status ready = backend_->BeforeRead(range.start, range.size);
                        !ready) {
                        return ready;
                    }
                }
            }
            const core::Status status = (*sink_)(
                RenderedFrame{.frame = wanted,
                              .top = screens_.Compose(*memory_, gpu::ScreenSetup::Top),
                              .bottom = screens_.Compose(*memory_, gpu::ScreenSetup::Bottom)});
            if (!status) {
                return status;
            }
        }
        return {};
    }

    core::Status List(std::span<const std::uint8_t> bytes) {
        return backend_->RunCommandList(bytes);
    }

    // Tells the backend what the command's engines are about to read and
    // write, then does what they do.
    core::Status Command(const gpu::GxCommandWords& words) {
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
        for (const gpu::MemoryFillConfig& fill : gpu::MemoryFills(words)) {
            ++summary_.fills;
            static_cast<void>(gpu::RunMemoryFill(*memory_, fill));
        }
        if (const std::optional<gpu::TransferConfig> transfer = gpu::Transfer(words)) {
            ++summary_.transfers;
            if (gpu::RunTransfer(*memory_, *transfer) == gpu::TransferOutcome::Unsupported) {
                summary_.unsupported = true;
            }
        }
        return {};
    }

    core::Status Page(const gpu::RecordedPage& page) {
        const std::optional<std::uint32_t> physical =
            gpu::GpuMemory::PhysicalFromVirtual(page.address);
        return physical ? backend_->BeforeWrite(*physical, PageSize) : core::Status{};
    }

    void Framebuffer(const gpu::RecordedFramebuffer& framebuffer) {
        screens_.Apply(framebuffer);
    }

    // The frames still pending that the recording reached, once it has
    // ended or the last of them has been replayed.
    core::Status Finish() {
        std::erase_if(pending_, [this](std::uint64_t frame) { return frame > seen_; });
        return Before(seen_ + 1, false);
    }

    [[nodiscard]] const GpuRenderSummary& Summary() const {
        return summary_;
    }

private:
    gpu::GpuMemory* memory_;
    ports::GpuBackend* backend_;
    gpu::ScreenSetup screens_;
    const std::function<core::Status(const RenderedFrame&)>* sink_;
    std::set<std::uint64_t> pending_;
    GpuRenderSummary summary_;
    std::uint64_t seen_ = 0;
};

}  // namespace

ReferenceGpuBackend::ReferenceGpuBackend(gpu::GpuMemory& memory, gpu::Sampling sampling)
    : renderer_(memory, sampling), processor_(state_, memory, renderer_), sampling_(sampling) {}

core::Status ReferenceGpuBackend::RunCommandList(std::span<const std::uint8_t> list) {
    ++lists_;
    const core::Result<gpu::CommandListRun> run = processor_.Run(list);
    return run ? core::Status{} : core::Status{run.Failure()};
}

ports::GpuBackendStatistics ReferenceGpuBackend::Statistics() const {
    ports::GpuBackendStatistics statistics;
    statistics.lists = lists_;
    statistics.draws = renderer_.Statistics().draws;
    statistics.unsupported =
        renderer_.Statistics().unsupported_shader || renderer_.Rasterization().unsupported;
    return statistics;
}

std::string ReferenceGpuBackend::Describe() const {
    return std::format("the reference renderer, sampling {}",
                       sampling_ == gpu::Sampling::Hardware ? "as the registers say"
                                                            : "as Azahar does");
}

core::Result<GpuRenderSummary>
RenderGpuRecording(ports::GpuRecordingSource& source,
                   gpu::GpuMemory& memory,
                   ports::GpuBackend& backend,
                   const std::set<std::uint64_t>& frames,
                   const std::function<core::Status(const RenderedFrame&)>& sink) {
    if (frames.empty()) {
        return GpuRenderSummary{};
    }
    Renderer renderer(memory, backend, frames, sink);
    GpuReplayVisitor visitor;
    visitor.command = [&renderer](const gpu::RecordedCommand& command) {
        core::Status before = renderer.Before(command.frame, false);
        if (!before) {
            return before;
        }
        return renderer.Command(command.words);
    };
    visitor.page = [&renderer](const gpu::RecordedPage& page) { return renderer.Page(page); };
    visitor.list = [&renderer](const gpu::RecordedList& list, std::span<const std::uint8_t> bytes) {
        const core::Status before = renderer.Before(list.frame, false);
        return before ? renderer.List(bytes) : before;
    };
    visitor.framebuffer = [&renderer](const gpu::RecordedFramebuffer& framebuffer) {
        core::Status before = renderer.Before(framebuffer.frame, true);
        if (before) {
            renderer.Framebuffer(framebuffer);
        }
        return before;
    };
    const core::Status replayed = ReplayGpuRecording(source, memory, visitor, *frames.rbegin());
    if (!replayed) {
        return replayed.Failure();
    }
    // The last frame asked for has no later event to be taken at.
    const core::Status finished = renderer.Finish();
    if (!finished) {
        return finished.Failure();
    }
    return renderer.Summary();
}

}  // namespace hexlith::application
