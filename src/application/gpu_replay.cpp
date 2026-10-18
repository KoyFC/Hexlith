#include "application/gpu_replay.h"

#include <format>
#include <type_traits>
#include <variant>

namespace hexlith::application {
namespace {

namespace gpu = domain::gpu;

// The size every recorded page has.
constexpr std::size_t PageSize = 0x1000;

class Replayer {
public:
    Replayer(ports::GpuRecordingSource& source,
             gpu::GpuMemory& memory,
             const GpuReplayVisitor& visitor)
        : source_(&source), memory_(&memory), visitor_(&visitor) {}

    core::Status operator()(const gpu::RecordedPage& page) {
        const std::optional<std::uint32_t> physical =
            gpu::GpuMemory::PhysicalFromVirtual(page.address);
        if (!physical.has_value()) {
            return core::Error{
                std::format("a page at {:08X} is in neither VRAM nor a linear heap", page.address)};
        }
        const core::Result<std::vector<std::uint8_t>> bytes = source_->Page(page.digest);
        if (!bytes) {
            return bytes.Failure();
        }
        if (bytes.Value().size() != PageSize || !memory_->Write(*physical, bytes.Value())) {
            return core::Error{
                std::format("the page at {:08X} is not a whole page in GPU memory", page.address)};
        }
        return {};
    }

    core::Status operator()(const gpu::RecordedList& list) {
        const core::Result<std::vector<std::uint8_t>> bytes = source_->List(list.file);
        if (!bytes) {
            return bytes.Failure();
        }
        if (bytes.Value().size() != list.size) {
            return core::Error{std::format("list {} holds {} bytes where the index says {}",
                                           list.file,
                                           bytes.Value().size(),
                                           list.size)};
        }
        return visitor_->list ? visitor_->list(list, bytes.Value()) : core::Status{};
    }

    core::Status operator()(const gpu::RecordedCommand& command) {
        return visitor_->command ? visitor_->command(command) : core::Status{};
    }

    core::Status operator()(const gpu::RecordedFramebuffer& framebuffer) {
        return visitor_->framebuffer ? visitor_->framebuffer(framebuffer) : core::Status{};
    }

private:
    ports::GpuRecordingSource* source_;
    gpu::GpuMemory* memory_;
    const GpuReplayVisitor* visitor_;
};

std::uint64_t FrameOf(const gpu::RecordingEvent& event) {
    return std::visit([](const auto& recorded) { return recorded.frame; }, event);
}

}  // namespace

core::Status ReplayGpuRecording(ports::GpuRecordingSource& source,
                                gpu::GpuMemory& memory,
                                const GpuReplayVisitor& visitor,
                                std::optional<std::uint64_t> last_frame) {
    const core::Result<std::string> text = source.Index();
    if (!text) {
        return text.Failure();
    }
    const core::Result<gpu::RecordingIndex> index = gpu::ParseRecordingIndex(text.Value());
    if (!index) {
        return index.Failure().WithContext(source.Describe());
    }
    Replayer replayer{source, memory, visitor};
    for (const gpu::RecordingEvent& event : index.Value().events) {
        if (last_frame.has_value() && FrameOf(event) > *last_frame) {
            break;
        }
        const core::Status done = std::visit(replayer, event);
        if (!done) {
            return done.Failure().WithContext(source.Describe());
        }
    }
    return {};
}

}  // namespace hexlith::application
