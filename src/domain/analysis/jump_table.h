#ifndef HEXLITH_DOMAIN_ANALYSIS_JUMP_TABLE_H
#define HEXLITH_DOMAIN_ANALYSIS_JUMP_TABLE_H

#include "domain/analysis/code_walk.h"
#include "domain/arm/instruction.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace hexlith::domain::analysis {

// A switch the compiler turned into a table of code addresses.
//
// The shape is the one the ARM compilers of this title emit, and the one the
// table region kind exists for:
//
//     cmp   rm, #last
//     ldrls pc, [pc, rm, lsl #2]
//     b     default
//     .word case0, case1, ...
//
// The load adds the index to the program counter, which reads eight bytes
// ahead, so entry 0 sits two instructions after the load: the word between them
// is the branch to the default case and is never read as a table entry. Each
// entry is an absolute address in the segment.
struct JumpTable {
    // Where entry 0 is.
    std::uint32_t address = 0;

    // The entries, in order. Several may be the same address.
    std::vector<std::uint32_t> targets;
};

// Whether the instruction is the load that dispatches through a table: a word
// load into r15 from the program counter plus a register shifted left by two.
[[nodiscard]] bool IsTableDispatch(const arm::Instruction& instruction);

// The most entries read from a table nothing bounds. A switch with more cases
// than this is not one the compiler lays out this way.
constexpr std::size_t MaximumUnboundedEntries = 256;

// Reads the table a dispatch at `address` goes through, or nothing when what
// follows it is not a table.
//
// The number of entries comes from the comparison just before the load when
// there is one, since that is what keeps the load from running past the table:
// `cmp rm, #n` followed by `ldrls` has n + 1 cases and followed by `ldrlo` has
// n. Without one the table runs for as long as the words are plausible code
// addresses. Either way every entry has to be a word aligned address inside
// [code_begin, code_end), and one that is not rejects a table the comparison
// bounded and ends one it did not.
[[nodiscard]] std::optional<JumpTable> ReadJumpTable(const WalkImage& image,
                                                     std::uint32_t address,
                                                     const arm::Instruction& dispatch,
                                                     std::uint32_t code_begin,
                                                     std::uint32_t code_end);

}  // namespace hexlith::domain::analysis

#endif  // HEXLITH_DOMAIN_ANALYSIS_JUMP_TABLE_H
