#ifndef HEXLITH_DOMAIN_ARM_PRIMITIVES_TRANSFER_H
#define HEXLITH_DOMAIN_ARM_PRIMITIVES_TRANSFER_H

#include "domain/arm/operation.h"
#include "domain/arm/primitives/packing.h"

#include <cstdint>
#include <optional>

// Loads and stores: how wide each one is, where it lands, what the base holds
// afterwards, and how a value is widened on the way in.
//
// Alignment follows ARMv6 with unaligned access support enabled, the U bit of
// the system control register: a word or halfword load or store may be
// unaligned, and a block, doubleword, swap or exclusive transfer that is not
// word aligned faults. Source: the ARMv6 reference manual (DDI 0100I), section
// A2.8, "Unaligned access support". docs/arm-interpreter.md records what this
// has and has not been checked against.
namespace hexlith::domain::arm::primitives {

constexpr std::uint32_t WordSize = 4;

[[nodiscard]] constexpr bool IsAligned(std::uint32_t address, std::uint32_t size) {
    return (address % size) == 0;
}

enum class Width : std::uint8_t { Byte, SignedByte, Half, SignedHalf, Word };

[[nodiscard]] constexpr Width WidthOf(Operation operation) {
    switch (operation) {
    case Operation::LoadByte:
    case Operation::StoreByte:
    case Operation::LoadByteUnprivileged:
    case Operation::StoreByteUnprivileged:
        return Width::Byte;
    case Operation::LoadSignedByte:
        return Width::SignedByte;
    case Operation::LoadHalf:
    case Operation::StoreHalf:
        return Width::Half;
    case Operation::LoadSignedHalf:
        return Width::SignedHalf;
    default:
        return Width::Word;
    }
}

[[nodiscard]] constexpr Width WidthOfSize(std::uint32_t size) {
    switch (size) {
    case 1:
        return Width::Byte;
    case 2:
        return Width::Half;
    default:
        return Width::Word;
    }
}

// How many bytes an exclusive access covers, which is also the alignment it
// requires.
[[nodiscard]] constexpr std::uint32_t ExclusiveSize(Operation operation) {
    switch (operation) {
    case Operation::LoadExclusiveByte:
    case Operation::StoreExclusiveByte:
        return 1;
    case Operation::LoadExclusiveHalf:
    case Operation::StoreExclusiveHalf:
        return 2;
    case Operation::LoadExclusiveDouble:
    case Operation::StoreExclusiveDouble:
        return 2 * WordSize;
    default:
        return WordSize;
    }
}

struct Addressing {
    // Where the transfer happens.
    std::uint32_t address = 0;

    // What the base register holds afterwards when the instruction writes back.
    std::uint32_t written_back = 0;
};

// A single transfer's address from its base and offset: pre-indexed forms
// transfer at the offset address, post-indexed ones at the base.
[[nodiscard]] constexpr Addressing
SingleAddress(std::uint32_t base, std::uint32_t offset, bool add_offset, bool preindexed) {
    const std::uint32_t offset_address = add_offset ? base + offset : base - offset;
    return {.address = preindexed ? offset_address : base, .written_back = offset_address};
}

// The lowest address a block transfer touches, from its four addressing modes:
// increment after and before, decrement after and before.
[[nodiscard]] constexpr std::uint32_t
BlockStart(bool add_offset, bool preindexed, std::uint32_t base, std::uint32_t bytes) {
    if (add_offset) {
        return preindexed ? base + WordSize : base;
    }
    return preindexed ? base - bytes : base - bytes + WordSize;
}

[[nodiscard]] constexpr std::uint32_t
BlockWrittenBack(bool add_offset, std::uint32_t base, std::uint32_t bytes) {
    return add_offset ? base + bytes : base - bytes;
}

template<class Memory>
[[nodiscard]] std::optional<std::uint32_t>
LoadValue(const Memory& memory, std::uint32_t address, Width width) {
    switch (width) {
    case Width::Byte:
    case Width::SignedByte: {
        const std::optional<std::uint8_t> value = memory.Read8(address);
        if (!value.has_value()) {
            return std::nullopt;
        }
        return width == Width::Byte ? value.value() : SignExtendByte(value.value());
    }
    case Width::Half:
    case Width::SignedHalf: {
        const std::optional<std::uint16_t> value = memory.Read16(address);
        if (!value.has_value()) {
            return std::nullopt;
        }
        return width == Width::Half ? value.value() : SignExtendHalf(value.value());
    }
    case Width::Word:
        return memory.Read32(address);
    }
    return std::nullopt;
}

template<class Memory>
[[nodiscard]] bool
StoreValue(Memory& memory, std::uint32_t address, std::uint32_t value, Width width) {
    switch (width) {
    case Width::Byte:
    case Width::SignedByte:
        return memory.Write8(address, static_cast<std::uint8_t>(value));
    case Width::Half:
    case Width::SignedHalf:
        return memory.Write16(address, static_cast<std::uint16_t>(value));
    case Width::Word:
        return memory.Write32(address, value);
    }
    return false;
}

}  // namespace hexlith::domain::arm::primitives

#endif  // HEXLITH_DOMAIN_ARM_PRIMITIVES_TRANSFER_H
