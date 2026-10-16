#include "domain/memory/process_memory.h"

#include "domain/memory/guest_memory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <limits>

namespace hexlith::domain::memory {
namespace {

static_assert(GuestMemory<ProcessMemory>);

// A guest value is little-endian, and a host access copies it as it lies, so a
// big-endian host would need a byte swap on every access that nothing here
// does.
static_assert(std::endian::native == std::endian::little);

constexpr std::uint64_t AddressSpaceSize = std::uint64_t{1} << 32U;
constexpr std::size_t PageCount = AddressSpaceSize / ProcessMemory::PageSize;

std::uint32_t PageNumber(std::uint32_t address) {
    return address / ProcessMemory::PageSize;
}

std::uint32_t PageOffset(std::uint32_t address) {
    return address % ProcessMemory::PageSize;
}

}  // namespace

std::shared_ptr<Backing> Backing::Allocate(std::uint32_t size) {
    // The constructor is private so that a backing is always shared, which is
    // what keeps it alive while any page points into it.
    return std::shared_ptr<Backing>(new Backing(size));
}

ProcessMemory::ProcessMemory() : host_(PageCount, nullptr), access_(PageCount, Access::None) {}

core::Status
ProcessMemory::CheckRange(std::uint32_t address, std::uint32_t size, bool must_be_mapped) const {
    if (PageOffset(address) != 0 || PageOffset(size) != 0) {
        return core::Error{
            std::format("0x{:X} bytes at 0x{:08X} are not on page boundaries", size, address)};
    }
    if (static_cast<std::uint64_t>(address) + size > AddressSpaceSize) {
        return core::Error{std::format(
            "0x{:X} bytes at 0x{:08X} run past the end of the address space", size, address)};
    }
    for (std::uint32_t done = 0; done < size; done += PageSize) {
        if (IsMapped(address + done) != must_be_mapped) {
            return core::Error{std::format("the page at 0x{:08X} is {}",
                                           address + done,
                                           must_be_mapped ? "not mapped" : "already mapped")};
        }
    }
    return {};
}

core::Status ProcessMemory::Map(std::uint32_t address,
                                std::uint32_t size,
                                const std::shared_ptr<Backing>& backing,
                                std::uint32_t offset,
                                Access access) {
    if (const core::Status range = CheckRange(address, size, false); !range) {
        return range.Failure().WithContext("cannot map");
    }
    if (backing == nullptr || PageOffset(offset) != 0 ||
        static_cast<std::uint64_t>(offset) + size > backing->Size()) {
        return core::Error{std::format(
            "cannot map 0x{:X} bytes at 0x{:08X}: the backing does not hold them at 0x{:X}",
            size,
            address,
            offset)};
    }

    for (std::uint32_t done = 0; done < size; done += PageSize) {
        const std::uint32_t page = PageNumber(address + done);
        host_.at(page) = backing->Bytes().subspan(offset + done, PageSize).data();
        access_.at(page) = access;
        targets_.insert_or_assign(
            page, PageTarget{.backing = backing, .offset = offset + done, .access = access});
    }
    return {};
}

core::Status ProcessMemory::Unmap(std::uint32_t address, std::uint32_t size) {
    if (const core::Status range = CheckRange(address, size, true); !range) {
        return range.Failure().WithContext("cannot unmap");
    }
    for (std::uint32_t done = 0; done < size; done += PageSize) {
        const std::uint32_t page = PageNumber(address + done);
        host_.at(page) = nullptr;
        access_.at(page) = Access::None;
        targets_.erase(page);
    }
    return {};
}

core::Status ProcessMemory::Protect(std::uint32_t address, std::uint32_t size, Access access) {
    if (const core::Status range = CheckRange(address, size, true); !range) {
        return range.Failure().WithContext("cannot change the protection");
    }
    for (std::uint32_t done = 0; done < size; done += PageSize) {
        const std::uint32_t page = PageNumber(address + done);
        access_.at(page) = access;
        targets_.at(page).access = access;
    }
    return {};
}

bool ProcessMemory::IsMapped(std::uint32_t address) const {
    return host_.at(PageNumber(address)) != nullptr;
}

Access ProcessMemory::AccessAt(std::uint32_t address) const {
    return access_.at(PageNumber(address));
}

std::optional<PageTarget> ProcessMemory::TargetOf(std::uint32_t address) const {
    const auto found = targets_.find(PageNumber(address));
    if (found == targets_.end()) {
        return std::nullopt;
    }
    PageTarget target = found->second;
    target.offset += PageOffset(address);
    return target;
}

bool ProcessMemory::IsRangeMapped(std::uint32_t address, std::size_t size) const {
    const std::uint64_t end = static_cast<std::uint64_t>(address) + size;
    for (std::uint64_t at = address; at < end; at = ((at / PageSize) + 1) * PageSize) {
        if (at > std::numeric_limits<std::uint32_t>::max() ||
            !IsMapped(static_cast<std::uint32_t>(at))) {
            return false;
        }
    }
    return true;
}

core::Status ProcessMemory::Load(std::uint32_t address, core::ByteSpan bytes) {
    if (!IsRangeMapped(address, bytes.size())) {
        return core::Error{
            std::format("cannot load 0x{:X} bytes at 0x{:08X}: not all of it is mapped",
                        bytes.size(),
                        address)};
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto at = static_cast<std::uint32_t>(address + done);
        const std::span<std::uint8_t> page = HostBytes(at, Access::None);
        const std::size_t chunk = std::min(page.size(), bytes.size() - done);
        std::ranges::copy(bytes.subspan(done, chunk), page.begin());
        done += chunk;
    }
    return {};
}

core::Status ProcessMemory::Copy(std::uint32_t address, std::span<std::uint8_t> bytes) const {
    if (!IsRangeMapped(address, bytes.size())) {
        return core::Error{
            std::format("cannot copy 0x{:X} bytes at 0x{:08X}: not all of it is mapped",
                        bytes.size(),
                        address)};
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto at = static_cast<std::uint32_t>(address + done);
        const std::span<const std::uint8_t> page = HostBytes(at, Access::None);
        const std::size_t chunk = std::min(page.size(), bytes.size() - done);
        std::ranges::copy(page.first(chunk), bytes.subspan(done).begin());
        done += chunk;
    }
    return {};
}

std::span<std::uint8_t> ProcessMemory::HostBytes(std::uint32_t address, Access wanted) const {
    const std::uint32_t page = PageNumber(address);
    std::uint8_t* host = host_[page];
    if (host == nullptr || !Allows(access_[page], wanted)) {
        return {};
    }
    return std::span<std::uint8_t>(host, PageSize).subspan(PageOffset(address));
}

template<typename Value>
std::optional<Value> ProcessMemory::ReadValue(std::uint32_t address) const {
    // Inside one page, which is every aligned access: one lookup, one copy.
    if (PageOffset(address) + sizeof(Value) <= PageSize) {
        const std::span<const std::uint8_t> host = HostBytes(address, Access::Read);
        if (host.empty()) {
            return std::nullopt;
        }
        Value value{};
        std::memcpy(&value, host.data(), sizeof(Value));
        return value;
    }

    // Straddling two pages, which an unaligned word load can do. Each byte is
    // checked on its own page, and the access fails as a whole.
    std::array<std::uint8_t, sizeof(Value)> bytes{};
    for (std::size_t index = 0; index < sizeof(Value); ++index) {
        const std::span<const std::uint8_t> host =
            HostBytes(static_cast<std::uint32_t>(address + index), Access::Read);
        if (host.empty()) {
            return std::nullopt;
        }
        bytes.at(index) = host.front();
    }
    return std::bit_cast<Value>(bytes);
}

template<typename Value>
bool ProcessMemory::WriteValue(std::uint32_t address, Value value) {
    if (PageOffset(address) + sizeof(Value) <= PageSize) {
        const std::span<std::uint8_t> host = HostBytes(address, Access::Write);
        if (host.empty()) {
            return false;
        }
        std::memcpy(host.data(), &value, sizeof(Value));
        return true;
    }

    // Every byte is checked before any is written, so a failed write changes
    // nothing.
    std::array<std::span<std::uint8_t>, sizeof(Value)> hosts{};
    for (std::size_t index = 0; index < sizeof(Value); ++index) {
        hosts.at(index) = HostBytes(static_cast<std::uint32_t>(address + index), Access::Write);
        if (hosts.at(index).empty()) {
            return false;
        }
    }
    const auto bytes = std::bit_cast<std::array<std::uint8_t, sizeof(Value)>>(value);
    for (std::size_t index = 0; index < sizeof(Value); ++index) {
        hosts.at(index).front() = bytes.at(index);
    }
    return true;
}

std::optional<std::uint8_t> ProcessMemory::Read8(std::uint32_t address) const {
    return ReadValue<std::uint8_t>(address);
}

std::optional<std::uint16_t> ProcessMemory::Read16(std::uint32_t address) const {
    return ReadValue<std::uint16_t>(address);
}

std::optional<std::uint32_t> ProcessMemory::Read32(std::uint32_t address) const {
    return ReadValue<std::uint32_t>(address);
}

std::optional<std::uint64_t> ProcessMemory::Read64(std::uint32_t address) const {
    return ReadValue<std::uint64_t>(address);
}

bool ProcessMemory::Write8(std::uint32_t address, std::uint8_t value) {
    return WriteValue(address, value);
}

bool ProcessMemory::Write16(std::uint32_t address, std::uint16_t value) {
    return WriteValue(address, value);
}

bool ProcessMemory::Write32(std::uint32_t address, std::uint32_t value) {
    return WriteValue(address, value);
}

bool ProcessMemory::Write64(std::uint32_t address, std::uint64_t value) {
    return WriteValue(address, value);
}

}  // namespace hexlith::domain::memory
