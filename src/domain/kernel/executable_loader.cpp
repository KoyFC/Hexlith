#include "domain/kernel/executable_loader.h"

#include <format>
#include <string_view>

namespace hexlith::domain::kernel {
namespace {

using memory::Access;

std::uint32_t RoundUpToPage(std::uint32_t size) {
    return (size + layout::PageSize - 1) / layout::PageSize * layout::PageSize;
}

// Commits one segment's pages and copies its bytes in. The pages run past the
// bytes when the segment does not fill its last page, and those stay zero.
core::Status LoadSegment(AddressSpace& space,
                         std::string_view name,
                         std::uint32_t address,
                         std::uint32_t pages_size,
                         core::ByteSpan bytes,
                         Access access,
                         MemoryState state) {
    if (pages_size == 0) {
        return {};
    }
    const ResultCode committed = space.CommitFixed(address, pages_size, access, state);
    if (committed.IsError()) {
        return core::Error{std::format("cannot commit the {} segment, 0x{:X} bytes at 0x{:08X}: "
                                       "result 0x{:08X}",
                                       name,
                                       pages_size,
                                       address,
                                       committed.raw)};
    }
    return space.Memory().Load(address, bytes);
}

}  // namespace

core::Result<MainThreadStart> LoadExecutable(AddressSpace& space,
                                             const ctr::ExHeader& header,
                                             core::ByteSpan code,
                                             const SystemPages& pages) {
    if (code.size() != header.DecompressedCodeSize()) {
        return core::Error{
            std::format("the code is 0x{:X} bytes and the extended header's segments fill 0x{:X}",
                        code.size(),
                        header.DecompressedCodeSize())};
    }

    const auto text_size = static_cast<std::uint32_t>(header.text.PhysicalRegionSize());
    const auto read_only_size = static_cast<std::uint32_t>(header.read_only.PhysicalRegionSize());
    const auto data_size = static_cast<std::uint32_t>(header.data.PhysicalRegionSize());

    // The BSS follows the data in the same pages, rounded up on its own.
    // Azahar's loader notes it is not sure whether the console rounds the sum
    // or each part; rounding each part can only give more room, never less.
    const std::uint32_t data_and_bss = data_size + RoundUpToPage(header.bss_size);

    if (const core::Status text = LoadSegment(space,
                                              "text",
                                              header.text.address,
                                              text_size,
                                              code.first(text_size),
                                              Access::ReadExecute,
                                              MemoryState::Code);
        !text) {
        return text.Failure();
    }
    if (const core::Status read_only = LoadSegment(space,
                                                   "read-only",
                                                   header.read_only.address,
                                                   read_only_size,
                                                   code.subspan(text_size, read_only_size),
                                                   Access::Read,
                                                   MemoryState::Code);
        !read_only) {
        return read_only.Failure();
    }
    if (const core::Status data = LoadSegment(space,
                                              "data",
                                              header.data.address,
                                              data_and_bss,
                                              code.subspan(text_size + read_only_size, data_size),
                                              Access::ReadWrite,
                                              MemoryState::Private);
        !data) {
        return data.Failure();
    }

    const std::uint32_t stack_size = RoundUpToPage(header.stack_size);
    const ResultCode stack = space.CommitFixed(
        layout::HeapEnd - stack_size, stack_size, Access::ReadWrite, MemoryState::Locked);
    if (stack.IsError()) {
        return core::Error{std::format(
            "cannot commit the 0x{:X} byte stack: result 0x{:08X}", stack_size, stack.raw)};
    }

    const ResultCode configuration = space.MapBacking(layout::ConfigMemory,
                                                      layout::PageSize,
                                                      pages.configuration,
                                                      0,
                                                      Access::Read,
                                                      MemoryState::Shared);
    const ResultCode shared = space.MapBacking(
        layout::SharedPage, layout::PageSize, pages.shared, 0, Access::Read, MemoryState::Shared);
    if (configuration.IsError() || shared.IsError()) {
        return core::Error{"cannot map the configuration memory and the shared page"};
    }

    return MainThreadStart{.entry_point = header.text.address,
                           .stack_top = layout::HeapEnd,
                           .fpscr = MainThreadFpscr,
                           .priority = header.main_thread_priority,
                           .ideal_processor = header.ideal_processor};
}

}  // namespace hexlith::domain::kernel
