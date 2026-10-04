#ifndef HEXLITH_DOMAIN_ANALYSIS_TABLES_H
#define HEXLITH_DOMAIN_ANALYSIS_TABLES_H

#include "core/result.h"
#include "domain/analysis/provenance.h"
#include "domain/arm/instruction_set.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::domain::analysis {

// The four tables of ADR 0006, each a file of records keyed by address.
//
// Every writer here sorts before it writes, and every number it writes is
// fixed width, so regenerating a table rewrites only the lines that changed.
// That is the property the whole scheme rests on: a pass over two million
// instructions that reshuffled its own output would make the diff useless for
// reviewing what it did, and reviewing what it did is the only way to tell an
// improvement from a regression.
//
// Addresses in a table are in the address space of one module. For the main
// executable they are virtual addresses, which that image fixes. For a CRO
// module they are the segment offsets the module itself uses, because the
// loader decides where the module lands and the analysis cannot know it. One
// table therefore belongs to one module and never mixes the two.

constexpr std::string_view SymbolsFileName = "symbols.txt";
constexpr std::string_view FunctionsFileName = "functions.txt";
constexpr std::string_view RegionsFileName = "regions.txt";
constexpr std::string_view PointersFileName = "pointers.txt";

// A size of zero means the extent is not known yet, which is the state a
// symbol seeded from an export table starts in: the table names an address and
// says nothing about how much of what follows belongs to it.
constexpr std::uint32_t UnknownSize = 0;

enum class SymbolKind : std::uint8_t {
    Function,
    Data,

    // An export table says where a symbol is and not what it is, so a seeded
    // symbol has to be able to admit that until analysis settles it.
    Unknown,
};

std::string_view Describe(SymbolKind kind);

std::optional<SymbolKind> ParseSymbolKind(std::string_view text);

struct Symbol {
    std::uint32_t address = 0;
    std::uint32_t size = UnknownSize;
    SymbolKind kind = SymbolKind::Unknown;
    std::string name;
    Provenance source = Provenance::Heuristic;
};

enum class RegionKind : std::uint8_t {
    Code,
    Data,

    // A table of branch targets, read by a PC-relative load followed by a
    // computed branch. It sits in the middle of code and decoding it as
    // instructions produces nonsense that still looks plausible, which is why
    // it gets a kind of its own rather than being data in a code region.
    JumpTable,

    // Alignment between two things, belonging to neither.
    Padding,
};

std::string_view Describe(RegionKind kind);

std::optional<RegionKind> ParseRegionKind(std::string_view text);

struct Region {
    std::uint32_t address = 0;
    std::uint32_t size = UnknownSize;
    RegionKind kind = RegionKind::Data;

    // Set when the region is code and unset otherwise. A code region whose
    // encoding is unknown cannot be decoded, so the writer refuses to leave
    // it out and the reader refuses a file that does.
    std::optional<arm::InstructionSet> instruction_set;

    Provenance source = Provenance::Heuristic;
};

// One function: where it starts, how far it runs, and the other addresses the
// lifter is allowed to enter it at.
//
// Alternate entry points are not a curiosity. Hand written ARM routines share
// a tail between two entry points, and a compiler emits one when it folds two
// functions whose endings agree. A lifter that assumed one entry per function
// would either lose the second or split the function at it and duplicate the
// shared tail.
struct FunctionRange {
    std::uint32_t address = 0;
    std::uint32_t size = UnknownSize;
    arm::InstructionSet instruction_set = arm::InstructionSet::Arm;
    std::vector<std::uint32_t> alternate_entries;
    Provenance source = Provenance::Heuristic;
};

// A place in data holding the address of code.
//
// The main executable has no relocation table, so an absolute address sitting
// in its data is indistinguishable from an integer until something proves
// otherwise. Each one found is recorded here with how it was found, because a
// pointer claimed by a heuristic and a pointer a person confirmed need telling
// apart.
struct CodePointer {
    std::uint32_t address = 0;
    std::uint32_t target = 0;
    Provenance source = Provenance::Heuristic;
};

core::Result<std::vector<Symbol>> ParseSymbols(std::string_view text);

core::Result<std::vector<FunctionRange>> ParseFunctions(std::string_view text);

core::Result<std::vector<Region>> ParseRegions(std::string_view text);

core::Result<std::vector<CodePointer>> ParsePointers(std::string_view text);

// The writers take their records by value because they sort them. A caller
// that has already sorted pays a copy; a caller that has not gets a file it
// can diff against the last one, which is worth more.
std::string RenderSymbols(std::vector<Symbol> records);

std::string RenderFunctions(std::vector<FunctionRange> records);

std::string RenderRegions(std::vector<Region> records);

std::string RenderPointers(std::vector<CodePointer> records);

}  // namespace hexlith::domain::analysis

#endif  // HEXLITH_DOMAIN_ANALYSIS_TABLES_H
