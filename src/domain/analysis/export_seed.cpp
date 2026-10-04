#include "domain/analysis/export_seed.h"

#include <algorithm>
#include <format>

namespace hexlith::domain::analysis {
namespace {

namespace ctr = domain::ctr;

// The low bit of a code position is the flag BX reads to switch instruction
// set, not part of the address. docs/formats/cro0.md records the measurement
// that establishes it: of the static module's 341 code exports 338 are odd and
// none sits at an even address that is not a multiple of four, which is what
// the interworking convention predicts and what the bytes at those addresses
// confirm.
constexpr std::uint32_t InterworkingBit = 1;

SymbolKind KindOf(const ctr::CroSegment& segment) {
    if (!segment.type_known) {
        return SymbolKind::Unknown;
    }
    return segment.type == ctr::CroSegmentType::Code ? SymbolKind::Function : SymbolKind::Data;
}

}  // namespace

core::Result<SeededTables> SeedFromExports(const ctr::CroHeader& header,
                                           std::span<const ctr::CroNamedExport> exports) {
    SeededTables seeded;
    seeded.symbols.reserve(exports.size());

    for (const ctr::CroNamedExport& entry : exports) {
        const std::uint32_t index = entry.tag.SegmentIndex();
        if (index >= header.segments.size()) {
            return core::Error{
                std::format("the export '{}' sits in segment {}, and the module declares {}",
                            entry.name,
                            index,
                            header.segments.size())};
        }

        const ctr::CroSegment& segment = header.segments.at(index);
        const SymbolKind kind = KindOf(segment);
        const bool in_code = kind == SymbolKind::Function;
        const bool thumb = in_code && (entry.position & InterworkingBit) != 0;

        const auto address = static_cast<std::uint32_t>(in_code ? entry.position & ~InterworkingBit
                                                                : entry.position);

        seeded.symbols.push_back(Symbol{.address = address,
                                        .size = UnknownSize,
                                        .kind = kind,
                                        .name = entry.name,
                                        .source = Provenance::CrsExport});

        if (!in_code) {
            continue;
        }

        // Two names at one address is ordinary: a constructor and its
        // base-object variant are the same code under two mangled names, which
        // the static module's table does several times over. They are one
        // function, so the function record is written once.
        const bool already_known =
            std::ranges::any_of(seeded.functions, [address](const FunctionRange& function) {
                return function.address == address;
            });
        if (already_known) {
            continue;
        }

        seeded.functions.push_back(FunctionRange{
            .address = address,
            .size = UnknownSize,
            .instruction_set = thumb ? arm::InstructionSet::Thumb : arm::InstructionSet::Arm,
            .alternate_entries = {},
            .source = Provenance::CrsExport});
    }

    return seeded;
}

std::vector<WalkSeed> SeedFromImports(const ctr::CroHeader& exporter,
                                      std::span<const ctr::CroSegmentTag> imported) {
    std::vector<WalkSeed> seeds;
    for (const ctr::CroSegmentTag tag : imported) {
        const core::Result<std::size_t> position =
            ctr::ResolveCroSegmentTag(exporter, tag, "an imported position");
        if (!position) {
            continue;
        }
        const ctr::CroSegment& segment = exporter.segments.at(tag.SegmentIndex());
        if (KindOf(segment) != SymbolKind::Function) {
            continue;
        }
        const auto value = static_cast<std::uint32_t>(position.Value());
        const bool thumb = (value & InterworkingBit) != 0;
        seeds.push_back(WalkSeed{.address = value & ~InterworkingBit,
                                 .instruction_set =
                                     thumb ? arm::InstructionSet::Thumb : arm::InstructionSet::Arm,
                                 .source = Provenance::Relocation});
    }
    return seeds;
}

}  // namespace hexlith::domain::analysis
