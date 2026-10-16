#ifndef HEXLITH_DOMAIN_MEMORY_PROCESS_MEMORY_H
#define HEXLITH_DOMAIN_MEMORY_PROCESS_MEMORY_H

#include "core/byte_reader.h"
#include "core/result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace hexlith::domain::memory {

// What a page allows. The ARM11 user mode checks read and write on every
// access; execute is recorded for QueryMemory and for the day lifted code
// cares, but the interpreter fetches through Read and does not check it.
enum class Access : std::uint8_t {
    None = 0,
    Read = 1,
    Write = 2,
    ReadWrite = 3,
    Execute = 4,
    ReadExecute = 5,
    ReadWriteExecute = 7,
};

[[nodiscard]] constexpr bool Allows(Access granted, Access wanted) {
    const auto bits = static_cast<std::uint8_t>(wanted);
    return (static_cast<std::uint8_t>(granted) & bits) == bits;
}

// Host memory standing in for a stretch of the console's physical memory.
//
// Mappings point into a backing rather than owning bytes of their own, which is
// what lets two guest addresses share them. Horizon does this routinely: ldr:ro
// maps the buffer a module was read into at the address the module runs at, and
// MapMemoryBlock maps one shared block into several places. ADR 0012 has why.
//
// The bytes are allocated and zeroed up front. The largest backing is the
// application's FCRAM region, 64 MiB, which is small enough on a PC not to be
// worth allocating lazily.
class Backing {
public:
    // Zero filled. Size is a whole number of pages.
    static std::shared_ptr<Backing> Allocate(std::uint32_t size);

    [[nodiscard]] std::span<std::uint8_t> Bytes() {
        return bytes_;
    }

    [[nodiscard]] std::span<const std::uint8_t> Bytes() const {
        return bytes_;
    }

    [[nodiscard]] std::uint32_t Size() const {
        return static_cast<std::uint32_t>(bytes_.size());
    }

private:
    explicit Backing(std::uint32_t size) : bytes_(size, 0) {}

    std::vector<std::uint8_t> bytes_;
};

// Where one guest page points.
struct PageTarget {
    std::shared_ptr<Backing> backing;
    std::uint32_t offset = 0;
    Access access = Access::None;
};

// A process's 4 GiB address space as a table of 4 KiB pages, each pointing into
// a backing or at nothing.
//
// A read or a write is one table lookup and one host access when it stays inside
// a page, which is every aligned access, so this is what the interpreter runs
// against when it runs the game. The table is a flat array of host pointers,
// one per page, eight megabytes for the whole space: a lookup is an index, not
// a search, which is what ARCHITECTURE.md asks of the path that reads a guest
// word.
//
// It knows nothing of Horizon. Which ranges hold what, and the rules for
// changing them, belong to kernel::AddressSpace.
//
// Satisfies GuestMemory.
class ProcessMemory {
public:
    // The ARM11's small page, which is also the granularity Horizon maps in.
    static constexpr std::uint32_t PageSize = 0x1000;

    ProcessMemory();

    // Points the pages of a range at consecutive pages of a backing. The range
    // and the offset must be page aligned, the backing must hold the whole of
    // it, and no page of the range may be mapped already.
    core::Status Map(std::uint32_t address,
                     std::uint32_t size,
                     const std::shared_ptr<Backing>& backing,
                     std::uint32_t offset,
                     Access access);

    // Unmaps a range, every page of which must be mapped. The backing lives on
    // while anything else points into it.
    core::Status Unmap(std::uint32_t address, std::uint32_t size);

    // Changes what a mapped range allows.
    core::Status Protect(std::uint32_t address, std::uint32_t size, Access access);

    [[nodiscard]] bool IsMapped(std::uint32_t address) const;
    [[nodiscard]] Access AccessAt(std::uint32_t address) const;

    // Where a mapped page points, or nothing. This is how an alias is made: map
    // a second range at the targets of the first.
    [[nodiscard]] std::optional<PageTarget> TargetOf(std::uint32_t address) const;

    // Copies bytes into or out of mapped memory whatever it allows, the way the
    // kernel writes a segment into read-only pages. Fail without copying
    // anything if any byte of the range is unmapped.
    core::Status Load(std::uint32_t address, core::ByteSpan bytes);
    core::Status Copy(std::uint32_t address, std::span<std::uint8_t> bytes) const;

    [[nodiscard]] std::optional<std::uint8_t> Read8(std::uint32_t address) const;
    [[nodiscard]] std::optional<std::uint16_t> Read16(std::uint32_t address) const;
    [[nodiscard]] std::optional<std::uint32_t> Read32(std::uint32_t address) const;
    [[nodiscard]] std::optional<std::uint64_t> Read64(std::uint32_t address) const;

    bool Write8(std::uint32_t address, std::uint8_t value);
    bool Write16(std::uint32_t address, std::uint16_t value);
    bool Write32(std::uint32_t address, std::uint32_t value);
    bool Write64(std::uint32_t address, std::uint64_t value);

private:
    template<typename Value>
    [[nodiscard]] std::optional<Value> ReadValue(std::uint32_t address) const;
    template<typename Value>
    bool WriteValue(std::uint32_t address, Value value);

    // The bytes from address to the end of its page, if the page allows the
    // access, or an empty span.
    [[nodiscard]] std::span<std::uint8_t> HostBytes(std::uint32_t address, Access wanted) const;
    [[nodiscard]] bool IsRangeMapped(std::uint32_t address, std::size_t size) const;
    core::Status CheckRange(std::uint32_t address, std::uint32_t size, bool must_be_mapped) const;

    // Indexed by page number: the first host byte of the page, or null for an
    // unmapped page. A bare pointer rather than a span keeps the table at eight
    // bytes a page.
    std::vector<std::uint8_t*> host_;
    std::vector<Access> access_;

    // What keeps each mapped page's backing alive, and where in it the page is.
    // Touched only when the mapping changes, never by a read or a write.
    std::unordered_map<std::uint32_t, PageTarget> targets_;
};

}  // namespace hexlith::domain::memory

#endif  // HEXLITH_DOMAIN_MEMORY_PROCESS_MEMORY_H
