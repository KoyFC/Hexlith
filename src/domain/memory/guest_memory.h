#ifndef HEXLITH_DOMAIN_MEMORY_GUEST_MEMORY_H
#define HEXLITH_DOMAIN_MEMORY_GUEST_MEMORY_H

#include <concepts>
#include <cstdint>
#include <optional>

namespace hexlith::domain::memory {

// What the interpreter needs from guest memory: little-endian reads and writes
// of one, two, four and eight bytes at any address, each of which can fail.
//
// A concept rather than a virtual interface, because ARCHITECTURE.md keeps
// virtual dispatch off the path that reads a guest word. The interpreter is a
// template over the memory it runs against, so the runtime's flat memory, a
// test's paged one and whatever a tool needs all compile to direct calls.
//
// Failure is a value: a read that reaches unmapped memory answers nothing, and
// a write that reaches unmapped or read-only memory answers false. What a
// failure means, a crash in the game or the end of a test, is for the caller to
// decide.
//
// Alignment is not memory's business either. The ARM11 allows an unaligned
// word load and faults on an unaligned block transfer, and those rules belong
// to the instruction, so memory accepts any address and the interpreter applies
// them.
template<class Memory>
concept GuestMemory = requires(Memory& memory, const Memory& view, std::uint32_t address) {
    { view.Read8(address) } -> std::same_as<std::optional<std::uint8_t>>;
    { view.Read16(address) } -> std::same_as<std::optional<std::uint16_t>>;
    { view.Read32(address) } -> std::same_as<std::optional<std::uint32_t>>;
    { view.Read64(address) } -> std::same_as<std::optional<std::uint64_t>>;
    { memory.Write8(address, std::uint8_t{}) } -> std::same_as<bool>;
    { memory.Write16(address, std::uint16_t{}) } -> std::same_as<bool>;
    { memory.Write32(address, std::uint32_t{}) } -> std::same_as<bool>;
    { memory.Write64(address, std::uint64_t{}) } -> std::same_as<bool>;
};

}  // namespace hexlith::domain::memory

#endif  // HEXLITH_DOMAIN_MEMORY_GUEST_MEMORY_H
