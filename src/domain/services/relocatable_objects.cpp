#include "domain/services/relocatable_objects.h"

#include "domain/ctr/layout.h"

#include <array>
#include <format>
#include <string>

namespace hexlith::domain::services {
namespace {

namespace cro = ctr::cro;

// A module has to be mapped inside the range the executable is loaded in.
// Source: PROCESS_IMAGE_VADDR and PROCESS_IMAGE_MAX_SIZE in Azahar's memory.h,
// which ldr_ro.cpp checks against.
constexpr std::uint32_t ProcessImageBase = 0x00100000;
constexpr std::uint32_t ProcessImageEnd = ProcessImageBase + 0x03F00000;

constexpr std::uint32_t PageMask = kernel::layout::PageSize - 1;

std::uint32_t AlignDown(std::uint32_t value) {
    return value & ~PageMask;
}

std::uint32_t AlignUp(std::uint32_t value) {
    return (value + PageMask) & ~PageMask;
}

std::array<std::uint8_t, 4> LittleEndian(std::uint32_t value) {
    std::array<std::uint8_t, 4> bytes{};
    for (std::size_t byte = 0; byte < bytes.size(); ++byte) {
        bytes.at(byte) = static_cast<std::uint8_t>(value >> (8U * byte));
    }
    return bytes;
}

// A bool travels in the low byte of its word.
constexpr std::uint32_t BoolMask = 0xFF;

}  // namespace

RelocatableObjects::RelocatableObjects(kernel::Kernel& kernel) : kernel_(&kernel) {}

std::span<const RelocatableObjects::Command> RelocatableObjects::Commands() {
    static constexpr std::array<Command, 9> Table = {{
        {.id = 0x0001, .name = "Initialize", .handler = &RelocatableObjects::Initialize},
        {.id = 0x0002, .name = "LoadCRR", .handler = &RelocatableObjects::LoadCrr},
        {.id = 0x0003, .name = "UnloadCRR"},
        {.id = 0x0004, .name = "LoadCRO", .handler = &RelocatableObjects::LoadCro},
        {.id = 0x0005, .name = "UnloadCRO"},
        {.id = 0x0006, .name = "LinkCRO"},
        {.id = 0x0007, .name = "UnlinkCRO"},
        {.id = 0x0008, .name = "Shutdown"},
        {.id = 0x0009, .name = "LoadCRO_New"},
    }};
    return Table;
}

kernel::ResultCode RelocatableObjects::CheckMapping(std::uint32_t buffer,
                                                    std::uint32_t address,
                                                    std::uint32_t size) const {
    if (size < cro::HeaderSize) {
        return BufferTooSmall;
    }
    if ((buffer & PageMask) != 0 || (address & PageMask) != 0) {
        return MisalignedAddress;
    }
    if ((size & PageMask) != 0) {
        return MisalignedSize;
    }
    // The buffer has to be the game's own memory, readable and writable, as
    // VerifyBufferState checks.
    const kernel::AddressSpace& space = kernel_->Space();
    for (std::uint32_t page = buffer; page < buffer + size; page += kernel::layout::PageSize) {
        if (space.StateAt(page) != kernel::MemoryState::Private ||
            space.Memory().AccessAt(page) != memory::Access::ReadWrite) {
            return InvalidMemoryState;
        }
    }
    if (address < ProcessImageBase || address + size > ProcessImageEnd) {
        return IllegalAddress;
    }
    return kernel::result::Success;
}

std::optional<RelocatableObjects::Resident>
RelocatableObjects::MapModule(kernel::IpcRequest& request,
                              std::uint32_t buffer,
                              std::uint32_t address,
                              std::uint32_t size,
                              const linker::Placement& placement,
                              const linker::BufferSizes& sizes) {
    // The module's pages become code at its address, and the buffer is closed
    // to the game while the module lives there.
    kernel::AddressSpace& space = kernel_->Space();
    const kernel::ResultCode mapped =
        space.MapAlias(address, buffer, size, memory::Access::Read, true);
    if (mapped.IsError()) {
        request.Decline(std::format("a module the kernel could not map: 0x{:08X}", mapped.raw));
        return std::nullopt;
    }

    std::vector<std::uint8_t> image(size);
    static_cast<void>(space.Memory().Copy(address, image));
    core::Result<linker::CroModule> read = linker::ReadCroModule(image);
    if (!read) {
        request.Decline("a module Hexlith cannot read: " + read.Failure().Message());
        return std::nullopt;
    }

    const bool is_static = read.Value().header.IsStaticModule();
    const core::Status rebased =
        linker::RebaseModuleImage(image, address, placement, sizes, is_static);
    if (!rebased) {
        request.Decline("a module the loader would refuse: " + rebased.Failure().Message());
        return std::nullopt;
    }
    static_cast<void>(space.Memory().Load(address, image));

    Resident resident;
    resident.module = std::make_unique<linker::CroModule>(std::move(read).Value());
    resident.address = address;
    resident.buffer = buffer;
    resident.size = size;
    resident.external_table = HeaderField(address, cro::ExternalRelocationTableOffsetField);
    return resident;
}

void RelocatableObjects::Initialize(kernel::IpcRequest& request) {
    const std::uint32_t buffer = request.Pop();
    const std::uint32_t size = request.Pop();
    const std::uint32_t address = request.Pop();
    static_cast<void>(request.PopHandles());  // the calling process

    if (linker_ != nullptr) {
        request.ReplyWith(AlreadyInitialized);
        return;
    }
    const kernel::ResultCode checked = CheckMapping(buffer, address, size);
    if (checked.IsError()) {
        request.ReplyWith(checked);
        return;
    }

    std::optional<Resident> crs = MapModule(request, buffer, address, size, {}, {});
    if (!crs.has_value()) {
        return;
    }
    if (!crs->module->header.IsStaticModule()) {
        request.Decline("a CRS that is not the static module");
        return;
    }
    core::Result<linker::Linker> linker = linker::Linker::FromStaticModule(*crs->module);
    if (!linker) {
        request.Decline("a static module Hexlith cannot link: " + linker.Failure().Message());
        return;
    }
    linker_ = std::make_unique<linker::Linker>(std::move(linker).Value());
    residents_.push_back(std::move(*crs));
    WriteLinked(0);
    request.ReplyWith(kernel::result::Success);
}

void RelocatableObjects::LoadCrr(kernel::IpcRequest& request) {
    // The CRR lists the hashes a module must match. Azahar checks none of
    // them, and neither does Hexlith: the modules come from the cartridge.
    static_cast<void>(request.Pop());
    static_cast<void>(request.Pop());
    static_cast<void>(request.PopHandles());
    request.ReplyWith(kernel::result::Success);
}

void RelocatableObjects::LoadCro(kernel::IpcRequest& request) {
    LoadRequest load;
    load.buffer = request.Pop();
    load.address = request.Pop();
    load.size = request.Pop();
    load.data_address = request.Pop();
    load.zero = request.Pop();
    load.data_size = request.Pop();
    load.bss_address = request.Pop();
    load.bss_size = request.Pop();
    load.auto_link = (request.Pop() & BoolMask) != 0;
    load.fix_level = request.Pop();
    static_cast<void>(request.Pop());  // the CRR, which nothing checks
    static_cast<void>(request.PopHandles());

    auto refuse = [&request](kernel::ResultCode result) {
        request.Begin(2, 0);
        request.Push(result);
        request.Push(0);
    };
    if (linker_ == nullptr) {
        refuse(NotInitialized);
        return;
    }
    const kernel::ResultCode checked = CheckMapping(load.buffer, load.address, load.size);
    if (checked.IsError()) {
        refuse(checked);
        return;
    }
    if (load.zero != 0) {
        request.Decline("a LoadCRO whose reserved word is not zero");
        return;
    }

    const linker::Placement placement{.module_address = load.address,
                                      .data_address = load.data_address,
                                      .bss_address = load.bss_address};
    std::optional<Resident> cro = MapModule(request,
                                            load.buffer,
                                            load.address,
                                            load.size,
                                            placement,
                                            {.data = load.data_size, .bss = load.bss_size});
    if (!cro.has_value()) {
        return;
    }
    const core::Result<std::size_t> linked = linker_->Load(*cro->module, placement);
    if (!linked) {
        request.Decline("a module Hexlith cannot link: " + linked.Failure().Message());
        return;
    }
    residents_.push_back(std::move(*cro));
    WriteLinked(linked.Value());
    Register(load.address, load.auto_link);

    // Fix touches the header alone, so only the header is written back.
    std::vector<std::uint8_t> image(load.size);
    memory::ProcessMemory& memory = kernel_->Space().Memory();
    static_cast<void>(memory.Copy(load.address, image));
    const core::Result<std::uint32_t> fixed =
        linker::FixModuleImage(image, load.address, load.fix_level);
    if (!fixed) {
        request.Decline(fixed.Failure().Message());
        return;
    }
    static_cast<void>(
        memory.Load(load.address, std::span<const std::uint8_t>(image).first(cro::HeaderSize)));

    // What a fixed module no longer needs goes back to the game.
    const std::uint32_t fixed_size = fixed.Value();
    kernel::AddressSpace& space = kernel_->Space();
    if (fixed_size != load.size) {
        static_cast<void>(space.UnmapAlias(load.address + fixed_size,
                                           load.buffer + fixed_size,
                                           load.size - fixed_size,
                                           memory::Access::ReadWrite,
                                           true));
    }
    // And its code becomes executable. Source: GetExecutablePages.
    for (const ctr::CroSegment& segment : residents_.back().module->header.segments) {
        if (segment.type_known && segment.type == ctr::CroSegmentType::Code && segment.size != 0) {
            const auto begin = static_cast<std::uint32_t>(load.address + segment.offset);
            const std::uint32_t first = AlignDown(begin);
            const std::uint32_t end = AlignUp(begin + static_cast<std::uint32_t>(segment.size));
            static_cast<void>(space.Protect(first, end - first, memory::Access::ReadExecute));
            break;
        }
    }

    if (module_observer_) {
        module_observer_(ModuleEvent{.kind = ModuleEvent::Kind::Loaded,
                                     .name = residents_.back().module->header.name,
                                     .address = load.address});
    }

    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(fixed_size);
}

void RelocatableObjects::WriteInternal(const Resident& resident,
                                       const linker::LoadedModule& loaded) {
    // An internal relocation into data is patched in the file, at the data
    // segment's place in the module, before the game copies the data to its
    // buffer, which is where the Linker reports the word. docs/cro-linking.md
    // has the detail.
    memory::ProcessMemory& memory = kernel_->Space().Memory();
    const ctr::CroHeader& header = resident.module->header;
    for (const linker::LinkedWord& word : loaded.internal) {
        if (!word.value.has_value()) {
            continue;
        }
        const std::uint32_t value = word.value.value();
        const ctr::CroSegment& segment = header.segments.at(word.target.SegmentIndex());
        const bool into_data = !header.IsStaticModule() && segment.type_known &&
                               segment.type == ctr::CroSegmentType::Data;
        const std::uint32_t address = into_data ? resident.address +
                                                      static_cast<std::uint32_t>(segment.offset) +
                                                      word.target.OffsetInSegment()
                                                : word.address;
        static_cast<void>(memory.Load(address, LittleEndian(value)));
    }
}

void RelocatableObjects::WriteLinked(std::size_t newest) {
    memory::ProcessMemory& memory = kernel_->Space().Memory();
    const std::vector<linker::LoadedModule>& modules = linker_->Modules();
    for (std::size_t index = 0; index < modules.size(); ++index) {
        const linker::LoadedModule& loaded = modules.at(index);
        Resident& resident = residents_.at(index);
        if (index == newest) {
            WriteInternal(resident, loaded);
        }

        resident.written.resize(loaded.external.size());
        resident.resolved.resize(loaded.external.size(), false);
        const std::vector<ctr::CroBatchRelocation>& entries =
            resident.module->tables.external_relocations;
        bool batch_begins = true;
        for (std::size_t entry = 0; entry < loaded.external.size(); ++entry) {
            const linker::LinkedWord& word = loaded.external.at(entry);
            const std::optional<std::uint32_t> value = word.value;
            if (value.has_value() && resident.written.at(entry) != value) {
                static_cast<void>(memory.Load(word.address, LittleEndian(value.value())));
                resident.written.at(entry) = value;
            }
            // ApplyRelocationBatch marks the first entry of a batch it applies.
            const bool resolved = loaded.resolutions.at(entry).resolved;
            if (batch_begins && resident.resolved.at(entry) != resolved) {
                const std::uint32_t flag =
                    resident.external_table +
                    static_cast<std::uint32_t>(entry * cro::RelocationEntrySize) +
                    static_cast<std::uint32_t>(cro::RelocationBatchResolvedOffset);
                const std::array<std::uint8_t, 1> marked = {resolved ? std::uint8_t{1}
                                                                     : std::uint8_t{0}};
                static_cast<void>(memory.Load(flag, marked));
                resident.resolved.at(entry) = resolved;
            }
            batch_begins = entries.at(entry).batch_end;
        }
    }
}

std::uint32_t RelocatableObjects::HeaderField(std::uint32_t base, std::size_t field) const {
    return kernel_->Space()
        .Memory()
        .Read32(base + static_cast<std::uint32_t>(cro::FieldOffset(field)))
        .value_or(0);
}

void RelocatableObjects::SetHeaderField(std::uint32_t base,
                                        std::size_t field,
                                        std::uint32_t value) {
    static_cast<void>(kernel_->Space().Memory().Load(
        base + static_cast<std::uint32_t>(cro::FieldOffset(field)), LittleEndian(value)));
}

void RelocatableObjects::Register(std::uint32_t address, bool auto_link) {
    // The static module's header holds the heads of two lists, the modules
    // linked automatically in its next field and the others in its previous
    // field. Each head's previous field points at the tail of its list.
    const std::uint32_t crs = residents_.front().address;
    const std::size_t head_field = auto_link ? cro::NextModuleField : cro::PreviousModuleField;
    const std::uint32_t head = HeaderField(crs, head_field);
    if (head != 0) {
        const std::uint32_t tail = HeaderField(head, cro::PreviousModuleField);
        SetHeaderField(address, cro::PreviousModuleField, tail);
        SetHeaderField(tail, cro::NextModuleField, address);
        SetHeaderField(head, cro::PreviousModuleField, address);
    } else {
        SetHeaderField(address, cro::PreviousModuleField, address);
        SetHeaderField(crs, head_field, address);
    }
    SetHeaderField(address, cro::NextModuleField, 0);
}

}  // namespace hexlith::domain::services
