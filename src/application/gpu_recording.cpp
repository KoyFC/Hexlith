#include "application/gpu_recording.h"

#include <algorithm>

namespace hexlith::application {
namespace {

namespace kernel = domain::kernel;

constexpr std::uint32_t PageSize = kernel::layout::PageSize;
constexpr std::uint32_t CommandIdMask = 0xFF;

// A command list's address and size, at words 1 and 2 of its command. Source:
// SubmitCmdListCommand in Azahar's gsp_command.h.
constexpr std::size_t ListAddressWord = 1;
constexpr std::size_t ListSizeWord = 2;

}  // namespace

GpuRecording::GpuRecording(BootedTitle& title, ports::GpuRecorder& recorder)
    : GpuRecording(title.Kernel(), title.Gpu(), recorder) {}

GpuRecording::GpuRecording(kernel::Kernel& kernel,
                           domain::services::Graphics& gpu,
                           ports::GpuRecorder& recorder)
    : kernel_(&kernel), recorder_(&recorder) {
    gpu.OnCommand([this](const domain::services::Graphics::GxCommand& command) { Take(command); });
}

void GpuRecording::Keep(const core::Status& status) {
    if (!status && failure_) {
        failure_ = status;
    }
}

void GpuRecording::Take(const domain::services::Graphics::GxCommand& command) {
    const std::uint32_t id = command.words.front() & CommandIdMask;
    if (id < summary_.commands.size()) {
        ++summary_.commands.at(id);
    }
    Keep(recorder_->Command(command.frame, command.words));
    if (id != domain::services::Graphics::SubmitCommandListId) {
        return;
    }

    RecordChangedPages(command.frame);
    const std::uint32_t address = command.words.at(ListAddressWord);
    std::vector<std::uint8_t> list(command.words.at(ListSizeWord));
    if (!kernel_->Space().Memory().Copy(address, list).Ok()) {
        Keep(core::Error{"a command list the game named could not be read"});
        return;
    }
    Keep(recorder_->List(command.frame, address, list));

    const auto size = static_cast<std::uint32_t>(list.size());
    summary_.smallest_list = summary_.lists == 0 ? size : std::min(summary_.smallest_list, size);
    summary_.largest_list = std::max(summary_.largest_list, size);
    summary_.list_bytes += size;
    ++summary_.lists;
}

void GpuRecording::RecordChangedPages(std::uint64_t frame) {
    const kernel::AddressSpace& space = kernel_->Space();
    RecordPagesOf(
        frame, kernel::Kernel::VramBase, kernel::Kernel::VramBase + kernel::Kernel::VramSize);
    RecordPagesOf(frame, space.LinearHeapBase(), space.LinearHeapEnd());
}

void GpuRecording::RecordPagesOf(std::uint64_t frame, std::uint32_t begin, std::uint32_t end) {
    const kernel::AddressSpace& space = kernel_->Space();
    std::vector<std::uint8_t> page(PageSize);
    for (std::uint32_t address = begin; address < end; address += PageSize) {
        if (space.StateAt(address) == kernel::MemoryState::Free ||
            !space.Memory().Copy(address, page).Ok()) {
            continue;
        }
        const auto known = shadow_.find(address);
        const bool unchanged =
            known == shadow_.end()
                ? std::ranges::all_of(page, [](std::uint8_t byte) { return byte == 0; })
                : known->second == page;
        if (unchanged) {
            continue;
        }
        Keep(recorder_->Page(frame, address, page));
        shadow_[address] = page;
        ++summary_.pages;
    }
}

core::Status GpuRecording::Finish() {
    Keep(recorder_->Finish());
    return failure_;
}

}  // namespace hexlith::application
