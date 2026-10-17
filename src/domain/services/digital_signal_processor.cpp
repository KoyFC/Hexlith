#include "domain/services/digital_signal_processor.h"

#include <algorithm>

namespace hexlith::domain::services {
namespace {

// The audio pipe's start-up commands, the first byte of a four-byte message.
constexpr std::uint8_t Initialize = 0;
constexpr std::uint8_t Shutdown = 1;
constexpr std::uint8_t Wakeup = 2;
constexpr std::uint8_t Sleep = 3;
constexpr std::size_t AudioMessageSize = 4;

// The addresses, in DSP words, of the fifteen structures in each region of
// DSP memory, in the order the firmware reports them: frame counter, source
// configurations, source statuses, ADPCM coefficients, DSP configuration, DSP
// status, final samples, intermediate mix samples, compressor, debug, and five
// unknown blocks. They follow from the region's layout in Azahar's
// audio_core/hle/shared_memory.h, whose sizes it checks: the region starts at
// DSP word 0x8000 and each structure's word address is that plus half its
// byte offset.
constexpr std::array<std::uint16_t, 15> StructureAddresses = {
    0xBFFF,
    0x9E92,
    0x8680,
    0xA792,
    0x9430,
    0x8400,
    0x8540,
    0x9492,
    0x8710,
    0x8410,
    0xA912,
    0xAA12,
    0xAAD2,
    0xAC52,
    0xAC5C,
};

// A DSP word address maps to the process's view of DSP memory at twice the
// address, plus this. Source: ConvertProcessAddressFromDspDram in Azahar.
constexpr std::uint32_t DspDramOffset = 0x40000;

}  // namespace

DigitalSignalProcessor::DigitalSignalProcessor(kernel::Kernel& kernel) : kernel_(&kernel) {}

std::span<const DigitalSignalProcessor::Command> DigitalSignalProcessor::Commands() {
    using D = DigitalSignalProcessor;
    static constexpr std::array<Command, 22> Table = {{
        {.id = 0x0001, .name = "RecvData", .handler = &D::RecvData},
        {.id = 0x0002, .name = "RecvDataIsReady", .handler = &D::RecvDataIsReady},
        {.id = 0x0003, .name = "SendData"},
        {.id = 0x0004, .name = "SendDataIsEmpty"},
        {.id = 0x0007, .name = "SetSemaphore", .handler = &D::SetSemaphore},
        {.id = 0x0008, .name = "GetSemaphore"},
        {.id = 0x0009, .name = "ClearSemaphore"},
        {.id = 0x000C,
         .name = "ConvertProcessAddressFromDspDram",
         .handler = &D::ConvertProcessAddressFromDspDram},
        {.id = 0x000D, .name = "WriteProcessPipe", .handler = &D::WriteProcessPipe},
        {.id = 0x000E, .name = "ReadPipe"},
        {.id = 0x000F, .name = "GetPipeReadableSize", .handler = &D::GetPipeReadableSize},
        {.id = 0x0010, .name = "ReadPipeIfPossible", .handler = &D::ReadPipeIfPossible},
        {.id = 0x0011, .name = "LoadComponent", .handler = &D::LoadComponent},
        {.id = 0x0012, .name = "UnloadComponent"},
        {.id = 0x0013, .name = "FlushDataCache", .handler = &D::FlushDataCache},
        {.id = 0x0014, .name = "InvalidateDCache", .handler = &D::FlushDataCache},
        {.id = 0x0015, .name = "RegisterInterruptEvents", .handler = &D::RegisterInterruptEvents},
        {.id = 0x0016, .name = "GetSemaphoreEventHandle", .handler = &D::GetSemaphoreEventHandle},
        {.id = 0x0017, .name = "SetSemaphoreMask", .handler = &D::SetSemaphoreMask},
        {.id = 0x001F, .name = "GetHeadphoneStatus", .handler = &D::GetHeadphoneStatus},
        {.id = 0x0020, .name = "ForceHeadphoneOut"},
        {.id = 0x0021, .name = "GetIsDspOccupied"},
    }};
    return Table;
}

void DigitalSignalProcessor::Interrupt(std::uint32_t type, std::uint32_t pipe) {
    if (type < InterruptTypes && pipe < PipeCount) {
        if (const std::shared_ptr<kernel::Event>& event = events_.at(type).at(pipe)) {
            kernel_->Signal(*event);
        }
    }
}

void DigitalSignalProcessor::ScheduleFrame() {
    kernel::Scheduler& scheduler = kernel_->Threads();
    scheduler.ScheduleAt(scheduler.Ticks() + FrameTicks, [this] {
        if (on_) {
            Interrupt(MessageInterrupt, AudioPipe);
        }
        ScheduleFrame();
    });
}

void DigitalSignalProcessor::StartAudio() {
    // The firmware clears its memory and its pipes when it starts, then
    // reports where its structures are.
    std::ranges::fill(kernel_->DspMemory()->Bytes(), 0);
    for (std::vector<std::uint8_t>& pipe : pipes_) {
        pipe.clear();
    }
    std::vector<std::uint8_t>& audio = pipes_.at(AudioPipe);
    auto put = [&](std::uint16_t value) {
        audio.push_back(static_cast<std::uint8_t>(value));
        audio.push_back(static_cast<std::uint8_t>(value >> 8U));
    };
    put(static_cast<std::uint16_t>(StructureAddresses.size()));
    for (const std::uint16_t address : StructureAddresses) {
        put(address);
    }
    on_ = true;
    Interrupt(MessageInterrupt, AudioPipe);
    if (!ticking_) {
        ticking_ = true;
        ScheduleFrame();
    }
}

void DigitalSignalProcessor::RecvData(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop());
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    // One when the DSP is off or asleep, which the game checks after asking
    // it to stop.
    request.Push(on_ ? 0U : 1U);
}

void DigitalSignalProcessor::RecvDataIsReady(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop());
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(1);
}

void DigitalSignalProcessor::SetSemaphore(kernel::IpcRequest& request) {
    semaphore_ = static_cast<std::uint16_t>(request.Pop());
    request.ReplyWith(kernel::result::Success);
}

void DigitalSignalProcessor::ConvertProcessAddressFromDspDram(kernel::IpcRequest& request) {
    const std::uint32_t address = request.Pop();
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push((address << 1U) + kernel::Kernel::DspMemoryBase + DspDramOffset);
}

void DigitalSignalProcessor::WriteProcessPipe(kernel::IpcRequest& request) {
    const std::uint32_t pipe = request.Pop();
    static_cast<void>(request.Pop());
    const std::vector<std::uint8_t> message = request.PopStaticBuffer();
    if (pipe != AudioPipe || message.size() != AudioMessageSize) {
        request.Decline("a message on DSP pipe " + std::to_string(pipe));
        return;
    }
    switch (message.front()) {
    case Initialize:
    case Wakeup:
        StartAudio();
        break;
    case Shutdown:
    case Sleep:
        on_ = false;
        break;
    default:
        request.Decline("an unknown audio pipe command");
        return;
    }
    request.ReplyWith(kernel::result::Success);
}

void DigitalSignalProcessor::ReadPipeIfPossible(kernel::IpcRequest& request) {
    const std::uint32_t pipe = request.Pop();
    static_cast<void>(request.Pop());  // the peer
    const auto size = static_cast<std::size_t>(request.Pop() & 0xFFFFU);
    std::vector<std::uint8_t> data;
    if (pipe < PipeCount && pipes_.at(pipe).size() >= size) {
        std::vector<std::uint8_t>& source = pipes_.at(pipe);
        data.assign(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(size));
        source.erase(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(size));
    }
    request.Begin(2, 2);
    request.Push(kernel::result::Success);
    request.Push(static_cast<std::uint32_t>(data.size()));
    request.PushStaticBuffer(data, 0);
}

void DigitalSignalProcessor::GetPipeReadableSize(kernel::IpcRequest& request) {
    const std::uint32_t pipe = request.Pop();
    static_cast<void>(request.Pop());
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(pipe < PipeCount ? static_cast<std::uint32_t>(pipes_.at(pipe).size()) : 0U);
}

void DigitalSignalProcessor::LoadComponent(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop());  // the size
    static_cast<void>(request.Pop());  // the program and data masks
    const kernel::MappedBuffer buffer = request.PopMappedBuffer();
    component_loaded_ = true;
    request.Begin(2, 2);
    request.Push(kernel::result::Success);
    request.Push(1);
    request.PushMappedBuffer(buffer);
}

void DigitalSignalProcessor::FlushDataCache(kernel::IpcRequest& request) {
    request.ReplyWith(kernel::result::Success);
}

void DigitalSignalProcessor::RegisterInterruptEvents(kernel::IpcRequest& request) {
    const std::uint32_t type = request.Pop();
    const std::uint32_t pipe = request.Pop();
    const std::vector<std::shared_ptr<kernel::Object>> handles = request.PopHandles();
    if (type >= InterruptTypes || pipe >= PipeCount) {
        request.Decline("an interrupt of type " + std::to_string(type));
        return;
    }
    events_.at(type).at(pipe) =
        handles.empty() ? nullptr : std::dynamic_pointer_cast<kernel::Event>(handles.front());
    request.ReplyWith(kernel::result::Success);
}

void DigitalSignalProcessor::GetSemaphoreEventHandle(kernel::IpcRequest& request) {
    request.Begin(1, 2);
    request.Push(kernel::result::Success);
    const std::array<std::shared_ptr<kernel::Object>, 1> objects = {semaphore_event_};
    request.PushCopyObjects(objects);
}

void DigitalSignalProcessor::SetSemaphoreMask(kernel::IpcRequest& request) {
    semaphore_mask_ = static_cast<std::uint16_t>(request.Pop());
    request.ReplyWith(kernel::result::Success);
}

void DigitalSignalProcessor::GetHeadphoneStatus(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    // No headphones.
    request.Push(0);
}

}  // namespace hexlith::domain::services
