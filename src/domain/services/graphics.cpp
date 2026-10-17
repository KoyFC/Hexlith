#include "domain/services/graphics.h"

#include <array>

namespace hexlith::domain::services {
namespace {

// The shared block: one interrupt queue of 0x40 bytes per client from offset
// 0, then framebuffer information, then one command queue of 0x200 bytes per
// client from 0x800. Source: GetInterruptRelayQueue and GetCommandBuffer in
// Azahar's gsp_gpu.cpp.
constexpr std::uint32_t SharedBlockSize = 0x1000;
constexpr std::uint32_t ReadWrite = 3;
constexpr std::uint32_t InterruptQueueSize = 0x40;
constexpr std::uint32_t CommandQueueOffset = 0x800;

// An interrupt queue: its first slot's index, how many are queued, an error
// byte and a flags byte whose bit 0 asks for no vertical blanks; then two
// counts of vertical blanks missed while the queue was nearly full, then the
// slots. Source: InterruptRelayQueue in Azahar.
constexpr std::uint32_t QueueSlotsOffset = 0xC;
constexpr std::uint32_t QueueSlots = 0x34;
constexpr std::uint32_t VerticalBlankLimit = 0x20;
constexpr std::uint32_t QueueFull = 1;
constexpr std::uint32_t MissedPdc0Offset = 4;
constexpr std::uint32_t MissedPdc1Offset = 8;

// A command queue: a header word with the next command's index in bits 0 to
// 7, the count queued in 8 to 15, a status in 16 to 23 and a stop request in
// 24 to 31; seven unknown words; then fifteen commands of 0x20 bytes, each
// with its ID in the low byte of its first word and a flag to stop after it
// in bits 16 to 23. Source: CommandBuffer and Command in Azahar's
// gsp_command.h.
constexpr std::uint32_t CommandsOffset = 0x20;
constexpr std::uint32_t CommandSize = 0x20;
constexpr std::uint32_t CommandSlots = 0xF;
constexpr std::uint32_t ByteMask = 0xFF;
constexpr unsigned CountShift = 8;
constexpr unsigned StatusShift = 16;
constexpr unsigned StopShift = 24;
constexpr std::uint32_t StatusStopped = 1;

// Command IDs.
constexpr std::uint32_t RequestDma = 0;
constexpr std::uint32_t SubmitCommandList = 1;
constexpr std::uint32_t MemoryFill = 2;
constexpr std::uint32_t DisplayTransfer = 3;
constexpr std::uint32_t TextureCopy = 4;

// A memory fill's two start addresses, at words 1 and 4 of the command; a
// fill with a start of zero raises nothing, as Azahar observes. A DMA's
// source, destination and size at words 1 to 3.
constexpr std::uint32_t FillFirstStart = 4;
constexpr std::uint32_t FillSecondStart = 16;
constexpr std::uint32_t DmaSource = 4;
constexpr std::uint32_t DmaDestination = 8;
constexpr std::uint32_t DmaSize = 12;

constexpr std::uint32_t WordBytes = 4;

}  // namespace

Graphics::Graphics(kernel::Kernel& kernel)
    : kernel_(&kernel), shared_(kernel.CreateSystemMemoryBlock(SharedBlockSize, ReadWrite)) {
    ScheduleVerticalBlank();
}

std::span<const Graphics::Command> Graphics::Commands() {
    static constexpr std::array<Command, 22> Table = {{
        {.id = 0x0001, .name = "WriteHWRegs", .handler = &Graphics::WriteHwRegs},
        {.id = 0x0002, .name = "WriteHWRegsWithMask", .handler = &Graphics::WriteHwRegsWithMask},
        {.id = 0x0003, .name = "WriteHWRegRepeat"},
        {.id = 0x0004, .name = "ReadHWRegs", .handler = &Graphics::ReadHwRegs},
        {.id = 0x0005, .name = "SetBufferSwap", .handler = &Graphics::SetBufferSwap},
        {.id = 0x0007, .name = "RequestDma"},
        {.id = 0x0008, .name = "FlushDataCache", .handler = &Graphics::FlushDataCache},
        {.id = 0x0009, .name = "InvalidateDataCache", .handler = &Graphics::FlushDataCache},
        {.id = 0x000B, .name = "SetLcdForceBlack", .handler = &Graphics::Succeed},
        {.id = 0x000C, .name = "TriggerCmdReqQueue", .handler = &Graphics::TriggerCmdReqQueue},
        {.id = 0x0010, .name = "SetAxiConfigQoSMode", .handler = &Graphics::Succeed},
        {.id = 0x0011, .name = "SetPerfLogMode"},
        {.id = 0x0013,
         .name = "RegisterInterruptRelayQueue",
         .handler = &Graphics::RegisterInterruptRelayQueue},
        {.id = 0x0014, .name = "UnregisterInterruptRelayQueue", .handler = &Graphics::Succeed},
        {.id = 0x0015, .name = "TryAcquireRight"},
        {.id = 0x0016, .name = "AcquireRight", .handler = &Graphics::AcquireRight},
        {.id = 0x0017, .name = "ReleaseRight", .handler = &Graphics::Succeed},
        {.id = 0x0018, .name = "ImportDisplayCaptureInfo"},
        {.id = 0x0019, .name = "SaveVramSysArea"},
        {.id = 0x001A, .name = "RestoreVramSysArea"},
        {.id = 0x001E, .name = "SetInternalPriorities", .handler = &Graphics::Succeed},
        {.id = 0x001F, .name = "StoreDataCache", .handler = &Graphics::FlushDataCache},
    }};
    return Table;
}

std::uint32_t Graphics::Word(std::uint32_t offset) const {
    const std::span<const std::uint8_t> bytes = shared_->pages.front().backing->Bytes();
    std::uint32_t value = 0;
    for (std::uint32_t byte = 0; byte < WordBytes; ++byte) {
        value |= static_cast<std::uint32_t>(bytes[offset + byte]) << (8U * byte);
    }
    return value;
}

void Graphics::SetWord(std::uint32_t offset, std::uint32_t value) {
    const std::span<std::uint8_t> bytes = shared_->pages.front().backing->Bytes();
    for (std::uint32_t byte = 0; byte < WordBytes; ++byte) {
        bytes[offset + byte] = static_cast<std::uint8_t>(value >> (8U * byte));
    }
}

void Graphics::ScheduleVerticalBlank() {
    kernel::Scheduler& scheduler = kernel_->Threads();
    scheduler.ScheduleAt(scheduler.Ticks() + FrameTicks, [this] {
        ++frames_;
        Raise(Interrupt::Pdc0);
        Raise(Interrupt::Pdc1);
        ScheduleVerticalBlank();
    });
}

void Graphics::Queue(const Client& client, Interrupt interrupt) {
    const std::uint32_t base = client.thread_id * InterruptQueueSize;
    const std::span<std::uint8_t> bytes = shared_->pages.front().backing->Bytes();
    const std::uint8_t index = bytes[base];
    const std::uint8_t count = bytes[base + 1];
    const bool vertical_blank = interrupt == Interrupt::Pdc0 || interrupt == Interrupt::Pdc1;

    if (vertical_blank) {
        if ((bytes[base + 3] & 1U) != 0) {
            return;
        }
        if (count >= VerticalBlankLimit) {
            const std::uint32_t missed =
                base + (interrupt == Interrupt::Pdc0 ? MissedPdc0Offset : MissedPdc1Offset);
            SetWord(missed, Word(missed) + 1);
            return;
        }
    }
    if (count >= QueueSlots) {
        bytes[base + 2] = QueueFull;
        return;
    }
    bytes[base + QueueSlotsOffset + ((index + count) % QueueSlots)] =
        static_cast<std::uint8_t>(interrupt);
    bytes[base + 1] = static_cast<std::uint8_t>(count + 1);
    kernel_->Signal(*client.event);
}

void Graphics::Raise(Interrupt interrupt) {
    // Vertical blanks go to every client, the rest to the one with the right.
    const bool vertical_blank = interrupt == Interrupt::Pdc0 || interrupt == Interrupt::Pdc1;
    if (!vertical_blank && !has_right_) {
        return;
    }
    for (const Client& client : clients_) {
        Queue(client, interrupt);
        if (!vertical_blank) {
            return;
        }
    }
}

void Graphics::Execute(std::uint32_t offset) {
    const std::uint32_t id = Word(offset) & ByteMask;
    switch (id) {
    case RequestDma: {
        memory::ProcessMemory& memory = kernel_->Space().Memory();
        std::vector<std::uint8_t> bytes(Word(offset + DmaSize));
        if (memory.Copy(Word(offset + DmaSource), bytes).Ok()) {
            static_cast<void>(memory.Load(Word(offset + DmaDestination), bytes));
        }
        Raise(Interrupt::Dma);
        break;
    }
    case SubmitCommandList:
        Raise(Interrupt::P3d);
        break;
    case MemoryFill:
        if (Word(offset + FillFirstStart) != 0) {
            Raise(Interrupt::Psc0);
        }
        if (Word(offset + FillSecondStart) != 0) {
            Raise(Interrupt::Psc1);
        }
        break;
    case DisplayTransfer:
    case TextureCopy:
        Raise(Interrupt::Ppf);
        break;
    default:
        // A cache flush, which needs nothing.
        break;
    }
}

void Graphics::TriggerCmdReqQueue(kernel::IpcRequest& request) {
    if (has_right_) {
        // Only one client holds the right, the first to register.
        const std::uint32_t queue = CommandQueueOffset;
        while (true) {
            const std::uint32_t header = Word(queue);
            const std::uint32_t index = header & ByteMask;
            const std::uint32_t count = (header >> CountShift) & ByteMask;
            const std::uint32_t status = (header >> StatusShift) & ByteMask;
            if (count == 0 || status == StatusStopped || (header >> StopShift) != 0) {
                break;
            }
            const std::uint32_t command = queue + CommandsOffset + (index * CommandSize);
            const bool stop = ((Word(command) >> StatusShift) & ByteMask) != 0;
            std::uint32_t next = ((index + 1) % CommandSlots) | ((count - 1) << CountShift) |
                                 (status << StatusShift);
            if (stop) {
                next |= StatusStopped << StatusShift;
            }
            SetWord(queue, next);
            Execute(command);
        }
    }
    request.ReplyWith(kernel::result::Success);
}

void Graphics::RegisterInterruptRelayQueue(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop());  // flags
    const std::vector<std::shared_ptr<kernel::Object>> handles = request.PopHandles();
    auto event =
        handles.empty() ? nullptr : std::dynamic_pointer_cast<kernel::Event>(handles.front());

    const std::uint32_t thread_id = next_thread_id_;
    ++next_thread_id_;
    clients_.push_back(Client{.thread_id = thread_id, .event = event});
    const std::uint32_t base = thread_id * InterruptQueueSize;
    for (std::uint32_t word = 0; word < QueueSlotsOffset; word += WordBytes) {
        SetWord(base + word, 0);
    }

    request.Begin(2, 2);
    request.Push(thread_id == 0 ? FirstInitialization : kernel::result::Success);
    request.Push(thread_id);
    const std::array<std::shared_ptr<kernel::Object>, 1> objects = {shared_};
    request.PushCopyObjects(objects);
}

void Graphics::AcquireRight(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop());  // whether to force it
    static_cast<void>(request.PopHandles());
    has_right_ = true;
    request.ReplyWith(kernel::result::Success);
}

void Graphics::WriteHwRegs(kernel::IpcRequest& request) {
    const std::uint32_t address = request.Pop();
    static_cast<void>(request.Pop());
    const std::vector<std::uint8_t> data = request.PopStaticBuffer();
    for (std::size_t offset = 0; offset + WordBytes <= data.size(); offset += WordBytes) {
        std::uint32_t value = 0;
        for (std::uint32_t byte = 0; byte < WordBytes; ++byte) {
            value |= static_cast<std::uint32_t>(data.at(offset + byte)) << (8U * byte);
        }
        registers_[address + static_cast<std::uint32_t>(offset)] = value;
    }
    request.ReplyWith(kernel::result::Success);
}

void Graphics::WriteHwRegsWithMask(kernel::IpcRequest& request) {
    const std::uint32_t address = request.Pop();
    static_cast<void>(request.Pop());
    const std::vector<std::uint8_t> data = request.PopStaticBuffer();
    const std::vector<std::uint8_t> mask = request.PopStaticBuffer();
    for (std::size_t offset = 0; offset + WordBytes <= std::min(data.size(), mask.size());
         offset += WordBytes) {
        std::uint32_t value = 0;
        std::uint32_t bits = 0;
        for (std::uint32_t byte = 0; byte < WordBytes; ++byte) {
            value |= static_cast<std::uint32_t>(data.at(offset + byte)) << (8U * byte);
            bits |= static_cast<std::uint32_t>(mask.at(offset + byte)) << (8U * byte);
        }
        std::uint32_t& stored = registers_[address + static_cast<std::uint32_t>(offset)];
        stored = (stored & ~bits) | (value & bits);
    }
    request.ReplyWith(kernel::result::Success);
}

void Graphics::ReadHwRegs(kernel::IpcRequest& request) {
    const std::uint32_t address = request.Pop();
    const std::uint32_t size = request.Pop();
    std::vector<std::uint8_t> data(size, 0);
    for (std::uint32_t offset = 0; offset + WordBytes <= size; offset += WordBytes) {
        const auto found = registers_.find(address + offset);
        const std::uint32_t value = found == registers_.end() ? 0 : found->second;
        for (std::uint32_t byte = 0; byte < WordBytes; ++byte) {
            data.at(offset + byte) = static_cast<std::uint8_t>(value >> (8U * byte));
        }
    }
    request.Begin(1, 2);
    request.Push(kernel::result::Success);
    request.PushStaticBuffer(data, 0);
}

void Graphics::Succeed(kernel::IpcRequest& request) {
    request.ReplyWith(kernel::result::Success);
}

void Graphics::SetBufferSwap(kernel::IpcRequest& request) {
    // The screen and the framebuffer it should show next, which nothing here
    // shows.
    request.ReplyWith(kernel::result::Success);
}

void Graphics::FlushDataCache(kernel::IpcRequest& request) {
    // An address and a size and the caller's process: the cache is the host's,
    // and coherent.
    request.ReplyWith(kernel::result::Success);
}

}  // namespace hexlith::domain::services
