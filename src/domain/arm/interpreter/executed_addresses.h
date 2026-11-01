#ifndef HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTED_ADDRESSES_H
#define HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTED_ADDRESSES_H

#include "domain/arm/interpreter/interpreter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace hexlith::domain::arm {

// How many times each instruction ran, by address and instruction set.
//
// This is what a run observed, as opposed to what the analysis inferred, and
// the two are compared: the analysis says which bytes are code, and a run says
// which of them the game actually executes. Addresses are the ones the program
// counter held, so a module is recorded where it was loaded, and the same
// module loaded at two addresses appears twice.
class ExecutedAddresses final : public ExecutionObserver {
public:
    struct Entry {
        std::uint32_t address = 0;
        bool thumb = false;
        std::uint64_t count = 0;
    };

    void BeforeInstruction(std::uint32_t address, bool thumb) override;

    // Every address that ran at least once, in address order, ARM before Thumb
    // at one address.
    [[nodiscard]] std::vector<Entry> Entries() const;

    [[nodiscard]] std::uint64_t Total() const {
        return total_;
    }

    [[nodiscard]] std::uint64_t CountOf(std::uint32_t address, bool thumb) const;

private:
    // A page of counters: an ARM instruction is word aligned and a Thumb one
    // halfword aligned, so the two are kept apart rather than shared.
    static constexpr std::uint32_t PageBits = 12;
    static constexpr std::uint32_t PageSize = 1U << PageBits;
    static constexpr std::uint32_t PageMask = PageSize - 1;

    struct Page {
        std::array<std::uint32_t, PageSize / 4> arm{};
        std::array<std::uint32_t, PageSize / 2> thumb{};
    };

    Page& PageOf(std::uint32_t address);

    std::unordered_map<std::uint32_t, std::unique_ptr<Page>> pages_;

    // Most instructions run in the page the one before them did.
    std::uint32_t last_page_number_ = 0;
    Page* last_page_ = nullptr;
    std::uint64_t total_ = 0;
};

}  // namespace hexlith::domain::arm

#endif  // HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTED_ADDRESSES_H
