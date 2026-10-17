#ifndef HEXLITH_APPLICATION_GPU_RECORDING_H
#define HEXLITH_APPLICATION_GPU_RECORDING_H

#include "application/boot.h"
#include "core/result.h"
#include "ports/gpu_recorder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace hexlith::application {

// What a recording holds, for a person to read at the end of a run.
struct GpuRecordingSummary {
    // GX commands by ID: DMA, command list, fill, display transfer, texture
    // copy, cache flush.
    static constexpr std::size_t CommandKinds = 6;
    std::array<std::size_t, CommandKinds> commands{};
    std::size_t lists = 0;
    std::uint64_t list_bytes = 0;
    std::uint32_t smallest_list = 0;
    std::uint32_t largest_list = 0;
    std::size_t pages = 0;
};

// Records the GPU's work as the title hands it to gsp::Gpu: every GX command,
// and for each command list, first every page of GPU-visible memory that has
// changed since the last list, then the list's bytes. GPU-visible memory is
// VRAM and the linear heap, which is where a title puts what the GPU reads.
// ADR 0016 has why it is recorded this way.
//
// Recording reads every mapped page of both before each list, so it costs
// time and is only done when asked.
class GpuRecording {
public:
    GpuRecording(BootedTitle& title, ports::GpuRecorder& recorder);
    GpuRecording(domain::kernel::Kernel& kernel,
                 domain::services::Graphics& gpu,
                 ports::GpuRecorder& recorder);

    GpuRecording(const GpuRecording&) = delete;
    GpuRecording& operator=(const GpuRecording&) = delete;
    GpuRecording(GpuRecording&&) = delete;
    GpuRecording& operator=(GpuRecording&&) = delete;
    ~GpuRecording() = default;

    // The first failure to record anything, which stops nothing while the
    // run goes on, and the recorder's own finish.
    core::Status Finish();

    [[nodiscard]] const GpuRecordingSummary& Summary() const {
        return summary_;
    }

private:
    void Take(const domain::services::Graphics::GxCommand& command);
    void RecordChangedPages(std::uint64_t frame);
    void RecordPagesOf(std::uint64_t frame, std::uint32_t begin, std::uint32_t end);
    void Keep(const core::Status& status);

    domain::kernel::Kernel* kernel_;
    ports::GpuRecorder* recorder_;
    // What each page held when it was last recorded. A page never recorded is
    // taken to have held zeros, so the first list records every page that
    // does not.
    std::map<std::uint32_t, std::vector<std::uint8_t>> shadow_;
    core::Status failure_;
    GpuRecordingSummary summary_;
};

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_GPU_RECORDING_H
