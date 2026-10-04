#ifndef HEXLITH_DOMAIN_MEMORY_PAGED_MEMORY_H
#define HEXLITH_DOMAIN_MEMORY_PAGED_MEMORY_H

#include "core/byte_reader.h"
#include "core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>

namespace hexlith::domain::memory {

// A sparse guest address space made of 4 KiB pages, each mapped on its own,
// each readable and optionally writable.
//
// For tests and tools. Mapping a page costs a hash table entry and 4 KiB, so a
// test can map the handful of pages it touches anywhere in the four gigabytes
// and leave the rest unmapped, which is what makes a stray access visible. The
// runtime's guest memory will be a flat allocation with the 3DS memory map laid
// out in it instead, which is faster and needs the layout to be known; this one
// needs nothing.
//
// Satisfies GuestMemory.
class PagedMemory {
public:
    // The ARM11's small page, which is also the granularity Horizon maps memory
    // in.
    static constexpr std::uint32_t PageSize = 0x1000;

    // Maps the pages covering a range, zero filled. The range must start and
    // end on a page boundary and must not overlap anything already mapped.
    core::Status Map(std::uint32_t address, std::uint32_t size, bool writable);

    // Copies bytes into mapped memory, whether or not it is writable, which is
    // how code and read-only data get into the address space. Fails without
    // writing anything if any byte of the range is unmapped.
    core::Status Load(std::uint32_t address, core::ByteSpan bytes);

    // Copies bytes out of mapped memory, the counterpart of Load. Fails without
    // copying anything if any byte of the range is unmapped.
    core::Status Copy(std::uint32_t address, std::span<std::uint8_t> bytes) const;

    [[nodiscard]] bool IsMapped(std::uint32_t address) const;

    [[nodiscard]] std::optional<std::uint8_t> Read8(std::uint32_t address) const;
    [[nodiscard]] std::optional<std::uint16_t> Read16(std::uint32_t address) const;
    [[nodiscard]] std::optional<std::uint32_t> Read32(std::uint32_t address) const;
    [[nodiscard]] std::optional<std::uint64_t> Read64(std::uint32_t address) const;

    bool Write8(std::uint32_t address, std::uint8_t value);
    bool Write16(std::uint32_t address, std::uint16_t value);
    bool Write32(std::uint32_t address, std::uint32_t value);
    bool Write64(std::uint32_t address, std::uint64_t value);

private:
    struct Page {
        std::array<std::uint8_t, PageSize> bytes{};
        bool writable = false;
    };

    [[nodiscard]] bool IsRangeMapped(std::uint32_t address, std::size_t size) const;

    [[nodiscard]] const Page* Find(std::uint32_t address) const;
    [[nodiscard]] Page* Find(std::uint32_t address);

    // Reads or writes a little-endian value one byte at a time, so that a value
    // straddling two pages works, and fails as a whole if any byte cannot be
    // reached. A failed write changes nothing.
    [[nodiscard]] std::optional<std::uint64_t> ReadBytes(std::uint32_t address,
                                                         unsigned count) const;
    bool WriteBytes(std::uint32_t address, std::uint64_t value, unsigned count);

    std::unordered_map<std::uint32_t, std::unique_ptr<Page>> pages_;
};

}  // namespace hexlith::domain::memory

#endif  // HEXLITH_DOMAIN_MEMORY_PAGED_MEMORY_H
