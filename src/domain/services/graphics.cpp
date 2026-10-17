#include "domain/services/graphics.h"

#include "core/byte_reader.h"

#include <algorithm>
#include <array>
#include <vector>

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

// The parameters of each command, by byte offset into it. Source: the
// structures in Azahar's gsp_command.h; 3dbrew, GSP Shared Memory.
constexpr std::uint32_t DmaSource = 4;
constexpr std::uint32_t DmaDestination = 8;
constexpr std::uint32_t DmaSize = 12;
constexpr std::uint32_t ListAddress = 4;
constexpr std::uint32_t ListSize = 8;
// A fill's start, value and end for each of its two buffers, then their two
// controls as halfwords of one word.
constexpr std::uint32_t FillFirst = 4;
constexpr std::uint32_t FillSecond = 16;
constexpr std::uint32_t FillValue = 4;
constexpr std::uint32_t FillEnd = 8;
constexpr std::uint32_t FillControls = 28;
constexpr std::uint32_t TransferInput = 4;
constexpr std::uint32_t TransferOutput = 8;
constexpr std::uint32_t TransferInputSize = 12;
constexpr std::uint32_t TransferFlags = 20;
constexpr std::uint32_t CopySize = 12;
constexpr std::uint32_t CopyInputLayout = 16;
constexpr std::uint32_t CopyOutputLayout = 20;

constexpr std::uint32_t WordBytes = 4;
constexpr std::uint32_t HalfwordMask = 0xFFFF;
constexpr unsigned HighHalfword = 16;

// A fill's control: whether it writes 24-bit or 32-bit values, 16-bit if
// neither. Source: MemoryFillConfig in Azahar's regs_external.h.
constexpr std::uint32_t Fill24Bit = 1U << 8U;
constexpr std::uint32_t Fill32Bit = 1U << 9U;

// A texture copy moves whole 16-byte units, and its widths and gaps count in
// them. Source: SwBlitter::TextureCopy in Azahar.
constexpr std::uint32_t CopyUnit = 16;

// The transfer's input format, in bits 8 to 10 of its flags, and the bytes a
// pixel of each takes: RGBA8, RGB8, RGB565, RGB5A1, RGBA4. Source:
// DisplayTransferConfig and BytesPerPixel in Azahar.
constexpr unsigned InputFormatShift = 8;
constexpr std::uint32_t FormatMask = 0x7;
constexpr std::array<std::uint32_t, 5> BytesPerPixel = {4, 3, 2, 2, 2};

// The time a copy takes, at the rates Azahar measured on hardware, in MiB a
// second for RAM to RAM, RAM to VRAM, VRAM to RAM and VRAM to VRAM, by DMA and
// by the texture engine, which fills are taken to match. Source:
// DelayGenerator in Azahar's video_core/gpu.cpp.
constexpr std::array<std::array<double, 4>, 2> CopyRates = {{
    {190.0, 310.0, 380.0, 380.0},
    {450.0, 3100.0, 5400.0, 5400.0},
}};

// What every request costs before its interrupt, and a command of a list.
// Source: sync_delay_nanoseconds in Azahar's gsp_gpu.cpp, and
// nanoseconds_per_command in pica_core.h, which Azahar says is unmeasured.
constexpr std::uint64_t SyncNanoseconds = 300'000;
constexpr double NanosecondsPerCommand = 22.4;

constexpr double NanosecondsPerSecond = 1e9;
constexpr double BytesPerMebibyte = 1024.0 * 1024.0;

std::uint64_t TicksFor(std::uint64_t nanoseconds) {
    return nanoseconds * kernel::Arm11TicksPerSecond /
           static_cast<std::uint64_t>(NanosecondsPerSecond);
}

bool InVram(std::uint32_t address) {
    return address >= kernel::Kernel::VramBase &&
           address < kernel::Kernel::VramBase + kernel::Kernel::VramSize;
}

std::uint64_t CopyNanoseconds(bool texture, bool from_vram, bool to_vram, std::uint64_t bytes) {
    const std::size_t mode = (from_vram ? 2U : 0U) + (to_vram ? 1U : 0U);
    const double rate = CopyRates.at(texture ? 1 : 0).at(mode) * BytesPerMebibyte;
    return static_cast<std::uint64_t>(static_cast<double>(bytes) * NanosecondsPerSecond / rate);
}

// The register writes a PICA command list makes: each header word announces
// one write and any extra parameters after it, and each command is padded to
// eight bytes. Source: 3dbrew, GPU/Internal Registers, command lists.
std::uint64_t CountCommands(std::span<const std::uint8_t> list) {
    constexpr unsigned ExtraShift = 20;
    constexpr std::uint32_t ExtraMask = 0xFF;
    constexpr std::size_t CommandAlignment = 8;
    std::uint64_t writes = 0;
    std::size_t at = 0;
    constexpr std::size_t HeaderAndParameter = 2 * std::size_t{WordBytes};
    while (at + HeaderAndParameter <= list.size()) {
        const std::uint32_t header = core::ReadU32At(list, at + WordBytes).ValueOr(0);
        const std::uint32_t extra = (header >> ExtraShift) & ExtraMask;
        writes += 1 + extra;
        const std::size_t length = (2 + static_cast<std::size_t>(extra)) * WordBytes;
        at += (length + CommandAlignment - 1) / CommandAlignment * CommandAlignment;
    }
    return writes;
}

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

// Operations finish in the order they were started, each once the one
// before it has: the game counts its completions in that order, and lets
// its GPU work stall if a later one overtakes an earlier one.
void Graphics::RaiseAfter(Interrupt interrupt, std::uint64_t nanoseconds) {
    kernel::Scheduler& scheduler = kernel_->Threads();
    const std::uint64_t start = std::max(scheduler.Ticks(), busy_until_);
    busy_until_ = start + TicksFor(nanoseconds + SyncNanoseconds);
    scheduler.ScheduleAt(busy_until_, [this, interrupt] { Raise(interrupt); });
}

void Graphics::Execute(std::uint32_t offset) {
    const std::uint32_t id = Word(offset) & ByteMask;
    switch (id) {
    case RequestDma:
        Dma(offset);
        break;
    case SubmitCommandList:
        CommandList(offset);
        break;
    case MemoryFill:
        Fill(offset);
        break;
    case DisplayTransfer:
        Transfer(offset);
        break;
    case TextureCopy:
        Copy(offset);
        break;
    default:
        // A cache flush, which needs nothing.
        break;
    }
}

void Graphics::Dma(std::uint32_t offset) {
    memory::ProcessMemory& memory = kernel_->Space().Memory();
    const std::uint32_t source = Word(offset + DmaSource);
    const std::uint32_t destination = Word(offset + DmaDestination);
    std::vector<std::uint8_t> bytes(Word(offset + DmaSize));
    if (memory.Copy(source, bytes).Ok()) {
        static_cast<void>(memory.Load(destination, bytes));
    }
    RaiseAfter(Interrupt::Dma,
               CopyNanoseconds(false, InVram(source), InVram(destination), bytes.size()));
}

// The list is not run: nothing is drawn. Its end is raised as if it had been,
// once its commands' worth of time has passed. Azahar raises P3D when a list
// writes the interrupt request register, which lists end with, and adds the
// cost of the triangles drawn, which is not counted here.
void Graphics::CommandList(std::uint32_t offset) {
    std::vector<std::uint8_t> list(Word(offset + ListSize));
    static_cast<void>(kernel_->Space().Memory().Copy(Word(offset + ListAddress), list));
    const auto nanoseconds = static_cast<std::uint64_t>(static_cast<double>(CountCommands(list)) *
                                                        NanosecondsPerCommand);
    RaiseAfter(Interrupt::P3d, nanoseconds);
}

// Each buffer with a start is filled. With both, only PSC0 is raised, as the
// console's GSP module dispatches it. Source: GPU::Execute and MemoryFill in
// Azahar.
void Graphics::Fill(std::uint32_t offset) {
    const std::uint32_t controls = Word(offset + FillControls);
    const bool both = Word(offset + FillFirst) != 0 && Word(offset + FillSecond) != 0;
    for (std::uint32_t buffer = 0; buffer < 2; ++buffer) {
        const std::uint32_t base = offset + (buffer == 0 ? FillFirst : FillSecond);
        const std::uint32_t start = Word(base);
        if (start == 0) {
            continue;
        }
        const std::uint32_t end = Word(base + FillEnd);
        const std::uint32_t control = (controls >> (buffer * HighHalfword)) & HalfwordMask;
        FillRange(start, end, Word(base + FillValue), control);
        const std::uint64_t nanoseconds =
            CopyNanoseconds(true, true, InVram(start), end > start ? end - start : 0);
        if (buffer == 0) {
            if (!both) {
                RaiseAfter(Interrupt::Psc0, nanoseconds);
            }
        } else {
            RaiseAfter(both ? Interrupt::Psc0 : Interrupt::Psc1, nanoseconds);
        }
    }
}

void Graphics::FillRange(std::uint32_t start,
                         std::uint32_t end,
                         std::uint32_t value,
                         std::uint32_t control) {
    if (end <= start) {
        return;
    }
    std::vector<std::uint8_t> bytes(end - start);
    std::size_t width = 2;
    if ((control & Fill24Bit) != 0) {
        width = 3;
    } else if ((control & Fill32Bit) != 0) {
        width = 4;
    }
    for (std::size_t at = 0; at + width <= bytes.size(); at += width) {
        for (std::size_t byte = 0; byte < width; ++byte) {
            bytes.at(at + byte) = static_cast<std::uint8_t>(value >> (8U * byte));
        }
    }
    static_cast<void>(kernel_->Space().Memory().Load(start, bytes));
}

// A display transfer converts and tiles pixels into a framebuffer nothing
// reads before drawing exists, so only its time is kept.
void Graphics::Transfer(std::uint32_t offset) {
    const std::uint32_t size = Word(offset + TransferInputSize);
    const std::uint32_t format = (Word(offset + TransferFlags) >> InputFormatShift) & FormatMask;
    const std::uint32_t bytes_per_pixel =
        format < BytesPerPixel.size() ? BytesPerPixel.at(format) : 2;
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(size & HalfwordMask) * (size >> HighHalfword) * bytes_per_pixel;
    RaiseAfter(Interrupt::Ppf,
               CopyNanoseconds(true,
                               InVram(Word(offset + TransferInput)),
                               InVram(Word(offset + TransferOutput)),
                               bytes));
}

// A texture copy moves rows of whole units from one layout to another, each
// with a gap after it. A gap of zero means the data is contiguous. Source:
// SwBlitter::TextureCopy in Azahar.
void Graphics::Copy(std::uint32_t offset) {
    const std::uint32_t input = Word(offset + TransferInput);
    const std::uint32_t output = Word(offset + TransferOutput);
    const std::uint32_t size = Word(offset + CopySize) / CopyUnit * CopyUnit;
    const std::uint32_t in_layout = Word(offset + CopyInputLayout);
    const std::uint32_t out_layout = Word(offset + CopyOutputLayout);
    const std::uint32_t in_gap = (in_layout >> HighHalfword) * CopyUnit;
    const std::uint32_t out_gap = (out_layout >> HighHalfword) * CopyUnit;
    const std::uint32_t in_width = in_gap == 0 ? size : (in_layout & HalfwordMask) * CopyUnit;
    const std::uint32_t out_width = out_gap == 0 ? size : (out_layout & HalfwordMask) * CopyUnit;

    if (size != 0 && in_width != 0 && out_width != 0) {
        memory::ProcessMemory& memory = kernel_->Space().Memory();
        std::uint32_t from = input;
        std::uint32_t to = output;
        std::uint32_t left_in = in_width;
        std::uint32_t left_out = out_width;
        std::uint32_t remaining = size;
        while (remaining > 0) {
            const std::uint32_t step = std::min({left_in, left_out, remaining});
            std::vector<std::uint8_t> chunk(step);
            if (memory.Copy(from, chunk).Ok()) {
                static_cast<void>(memory.Load(to, chunk));
            }
            from += step;
            to += step;
            left_in -= step;
            left_out -= step;
            remaining -= step;
            if (left_in == 0) {
                left_in = in_width;
                from += in_gap;
            }
            if (left_out == 0) {
                left_out = out_width;
                to += out_gap;
            }
        }
    }
    RaiseAfter(Interrupt::Ppf, CopyNanoseconds(true, InVram(input), InVram(output), size));
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
