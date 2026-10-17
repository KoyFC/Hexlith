#include "application/golden_trace.h"

#include <format>
#include <memory>

namespace hexlith::application {
namespace {

using ModuleEvent = domain::services::RelocatableObjects::ModuleEvent;

constexpr std::string_view FormatLine = "# hexlith golden trace, format 1";

std::string_view KindOf(ModuleEvent::Kind kind) {
    return kind == ModuleEvent::Kind::Loaded ? "cro-load" : "cro-unload";
}

}  // namespace

std::string GoldenTraceHeader() {
    return std::string{FormatLine};
}

std::string GoldenRequestLine(std::uint64_t frame, std::string_view service, std::uint32_t header) {
    return std::format("{} ipc {} {:08X}", frame, service, header);
}

std::string GoldenModuleLine(std::uint64_t frame, const ModuleEvent& event) {
    return std::format("{} {} {} {:08X}", frame, KindOf(event.kind), event.name, event.address);
}

void RecordGoldenTrace(BootedTitle& title, std::function<void(std::string_view)> sink) {
    // Both observers write to the same sink, which outlives neither of them.
    auto shared = std::make_shared<std::function<void(std::string_view)>>(std::move(sink));
    (*shared)(GoldenTraceHeader());

    const domain::services::Graphics* gpu = &title.Gpu();
    title.Kernel().OnServiceRequest(
        [shared, gpu](const domain::kernel::ServiceRequestRecord& record) {
            (*shared)(GoldenRequestLine(gpu->Frames(), record.service, record.header));
        });
    title.Loader().OnModuleEvent([shared, gpu](const ModuleEvent& event) {
        (*shared)(GoldenModuleLine(gpu->Frames(), event));
    });
}

}  // namespace hexlith::application
