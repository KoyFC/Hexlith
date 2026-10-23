#include "application/gpu_render.h"

#include "application/gpu_replay.h"
#include "domain/gpu/command_processor.h"
#include "domain/gpu/gx_command.h"
#include "domain/gpu/memory_transfer.h"
#include "domain/gpu/reference_renderer.h"

#include <algorithm>
#include <memory>

namespace hexlith::application {
namespace {

namespace gpu = domain::gpu;

class Renderer {
public:
    Renderer(const GpuRenderOptions& options,
             const std::function<core::Status(const RenderedFrame&)>& sink)
        : memory_(std::make_unique<gpu::GpuMemory>()),
          renderer_(*memory_, options.sampling),
          processor_(state_, *memory_, renderer_),
          sink_(&sink),
          pending_(options.frames) {}

    [[nodiscard]] gpu::GpuMemory& Memory() {
        return *memory_;
    }

    // A frame's picture is taken at its first event that is not a
    // framebuffer line, which come first in a frame.
    core::Status Before(std::uint64_t frame, bool framebuffer) {
        seen_ = std::max(seen_, frame);
        while (!pending_.empty() &&
               (*pending_.begin() < frame || (*pending_.begin() == frame && !framebuffer))) {
            const std::uint64_t wanted = *pending_.begin();
            pending_.erase(pending_.begin());
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
        ++summary_.lists;
        const core::Result<gpu::CommandListRun> run = processor_.Run(bytes);
        return run ? core::Status{} : core::Status{run.Failure()};
    }

    void Command(const gpu::GxCommandWords& words) {
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

    [[nodiscard]] GpuRenderSummary Summary() const {
        GpuRenderSummary summary = summary_;
        summary.draws = renderer_.Statistics().draws;
        summary.unsupported = summary.unsupported || renderer_.Statistics().unsupported_shader ||
                              renderer_.Rasterization().unsupported;
        return summary;
    }

private:
    std::unique_ptr<gpu::GpuMemory> memory_;
    gpu::PicaState state_;
    gpu::ReferenceRenderer renderer_;
    gpu::CommandProcessor<gpu::ReferenceRenderer> processor_;
    gpu::ScreenSetup screens_;
    const std::function<core::Status(const RenderedFrame&)>* sink_;
    std::set<std::uint64_t> pending_;
    GpuRenderSummary summary_;
    std::uint64_t seen_ = 0;
};

}  // namespace

core::Result<GpuRenderSummary>
RenderGpuRecording(ports::GpuRecordingSource& source,
                   const GpuRenderOptions& options,
                   const std::function<core::Status(const RenderedFrame&)>& sink) {
    if (options.frames.empty()) {
        return GpuRenderSummary{};
    }
    // Nothing a list can write sets the interrupt registers; Nintendo's code
    // sets them before the first (InterruptSetup's defaults).
    Renderer renderer(options, sink);
    GpuReplayVisitor visitor;
    visitor.command = [&renderer](const gpu::RecordedCommand& command) {
        core::Status before = renderer.Before(command.frame, false);
        if (!before) {
            return before;
        }
        renderer.Command(command.words);
        return core::Status{};
    };
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
    const std::uint64_t last = *options.frames.rbegin();
    const core::Status replayed = ReplayGpuRecording(source, renderer.Memory(), visitor, last);
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
