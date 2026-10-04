#include "domain/memory/paged_memory.h"

#include "domain/memory/guest_memory.h"

#include <format>

namespace hexlith::domain::memory {
namespace {

static_assert(GuestMemory<PagedMemory>);

constexpr unsigned BitsPerByte = 8;
constexpr std::uint64_t ByteMask = 0xFF;

std::uint32_t PageNumber(std::uint32_t address) {
    return address / PagedMemory::PageSize;
}

std::uint32_t PageOffset(std::uint32_t address) {
    return address % PagedMemory::PageSize;
}

}  // namespace

core::Status PagedMemory::Map(std::uint32_t address, std::uint32_t size, bool writable) {
    if (PageOffset(address) != 0 || PageOffset(size) != 0) {
        return core::Error{std::format(
            "cannot map 0x{:X} bytes at 0x{:08X}: not on page boundaries", size, address)};
    }

    const std::uint64_t end = static_cast<std::uint64_t>(address) + size;
    for (std::uint64_t page = address; page < end; page += PageSize) {
        if (IsMapped(static_cast<std::uint32_t>(page))) {
            return core::Error{
                std::format("cannot map at 0x{:08X}: the page is already mapped", page)};
        }
    }

    for (std::uint64_t page = address; page < end; page += PageSize) {
        auto mapped = std::make_unique<Page>();
        mapped->writable = writable;
        pages_.emplace(PageNumber(static_cast<std::uint32_t>(page)), std::move(mapped));
    }
    return {};
}

core::Status PagedMemory::Load(std::uint32_t address, core::ByteSpan bytes) {
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (!IsMapped(static_cast<std::uint32_t>(address + index))) {
            return core::Error{
                std::format("cannot load 0x{:X} bytes at 0x{:08X}: 0x{:08X} is not mapped",
                            bytes.size(),
                            address,
                            address + index)};
        }
    }

    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto at = static_cast<std::uint32_t>(address + index);
        Find(at)->bytes.at(PageOffset(at)) = bytes[index];
    }
    return {};
}

bool PagedMemory::IsMapped(std::uint32_t address) const {
    return Find(address) != nullptr;
}

const PagedMemory::Page* PagedMemory::Find(std::uint32_t address) const {
    const auto found = pages_.find(PageNumber(address));
    return found == pages_.end() ? nullptr : found->second.get();
}

PagedMemory::Page* PagedMemory::Find(std::uint32_t address) {
    const auto found = pages_.find(PageNumber(address));
    return found == pages_.end() ? nullptr : found->second.get();
}

std::optional<std::uint64_t> PagedMemory::ReadBytes(std::uint32_t address, unsigned count) const {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < count; ++index) {
        const auto at = static_cast<std::uint32_t>(address + index);
        const Page* page = Find(at);
        if (page == nullptr) {
            return std::nullopt;
        }
        value |= static_cast<std::uint64_t>(page->bytes.at(PageOffset(at)))
                 << (BitsPerByte * index);
    }
    return value;
}

bool PagedMemory::WriteBytes(std::uint32_t address, std::uint64_t value, unsigned count) {
    for (unsigned index = 0; index < count; ++index) {
        const Page* page = Find(static_cast<std::uint32_t>(address + index));
        if (page == nullptr || !page->writable) {
            return false;
        }
    }

    for (unsigned index = 0; index < count; ++index) {
        const auto at = static_cast<std::uint32_t>(address + index);
        Find(at)->bytes.at(PageOffset(at)) =
            static_cast<std::uint8_t>((value >> (BitsPerByte * index)) & ByteMask);
    }
    return true;
}

std::optional<std::uint8_t> PagedMemory::Read8(std::uint32_t address) const {
    const std::optional<std::uint64_t> value = ReadBytes(address, sizeof(std::uint8_t));
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value.value());
}

std::optional<std::uint16_t> PagedMemory::Read16(std::uint32_t address) const {
    const std::optional<std::uint64_t> value = ReadBytes(address, sizeof(std::uint16_t));
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value.value());
}

std::optional<std::uint32_t> PagedMemory::Read32(std::uint32_t address) const {
    const std::optional<std::uint64_t> value = ReadBytes(address, sizeof(std::uint32_t));
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value.value());
}

std::optional<std::uint64_t> PagedMemory::Read64(std::uint32_t address) const {
    return ReadBytes(address, sizeof(std::uint64_t));
}

bool PagedMemory::Write8(std::uint32_t address, std::uint8_t value) {
    return WriteBytes(address, value, sizeof(value));
}

bool PagedMemory::Write16(std::uint32_t address, std::uint16_t value) {
    return WriteBytes(address, value, sizeof(value));
}

bool PagedMemory::Write32(std::uint32_t address, std::uint32_t value) {
    return WriteBytes(address, value, sizeof(value));
}

bool PagedMemory::Write64(std::uint32_t address, std::uint64_t value) {
    return WriteBytes(address, value, sizeof(value));
}

}  // namespace hexlith::domain::memory
