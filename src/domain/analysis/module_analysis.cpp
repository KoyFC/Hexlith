#include "domain/analysis/module_analysis.h"

#include <algorithm>
#include <format>
#include <optional>

namespace hexlith::domain::analysis {
namespace {

namespace ctr = domain::ctr;

// The interworking bit a code address carries when it is Thumb. Internal
// relocations into code keep it in the addend, as an export keeps it in its
// position. docs/formats/cro0.md has the measurement.
constexpr std::uint32_t ThumbBit = 1;

// The BSS goes on the first page boundary past the end of the file, so that no
// address in it is also an offset into the file.
constexpr std::uint32_t PageSize = 0x1000;

struct CodeRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;

    [[nodiscard]] bool Contains(std::uint32_t address) const {
        return address >= begin && address < end;
    }
};

// The span of the module's code segments. The title's modules have one each.
std::optional<CodeRange> CodeOf(const ctr::CroHeader& header) {
    std::optional<CodeRange> range;
    for (const ctr::CroSegment& segment : header.segments) {
        if (!segment.type_known || segment.type != ctr::CroSegmentType::Code || segment.size == 0) {
            continue;
        }
        const auto begin = static_cast<std::uint32_t>(segment.offset);
        const auto end = static_cast<std::uint32_t>(segment.offset + segment.size);
        range = range ? CodeRange{.begin = std::min(range->begin, begin),
                                  .end = std::max(range->end, end)}
                      : CodeRange{.begin = begin, .end = end};
    }
    return range;
}

WalkSeed SeedAt(std::uint32_t value, Provenance source) {
    const bool thumb = (value & ThumbBit) != 0;
    return WalkSeed{.address = value & ~ThumbBit,
                    .instruction_set =
                        thumb ? arm::InstructionSet::Thumb : arm::InstructionSet::Arm,
                    .source = source};
}

core::Result<linker::LoadedModule> LinkAlone(const linker::CroModule& static_module,
                                             const linker::CroModule& module) {
    core::Result<linker::Linker> linking = linker::Linker::FromStaticModule(static_module);
    if (!linking) {
        return linking.Failure();
    }
    const core::Result<std::size_t> index =
        linking.Value().Load(module, AnalysisPlacement(module.header));
    if (!index) {
        return index.Failure();
    }
    return linking.Value().Modules().at(index.Value());
}

}  // namespace

linker::Placement AnalysisPlacement(const ctr::CroHeader& header) {
    linker::Placement placement;
    for (const ctr::CroSegment& segment : header.segments) {
        if (segment.type_known && segment.type == ctr::CroSegmentType::Data && segment.size != 0) {
            placement.data_address = static_cast<std::uint32_t>(segment.offset);
        }
    }
    const auto file_end = static_cast<std::uint32_t>(header.declared_file_size);
    placement.bss_address = ((file_end + PageSize - 1) / PageSize) * PageSize;
    return placement;
}

core::Result<ModuleAnalysis> AnalyzeModule(const linker::CroModule& static_module,
                                           const ModuleAnalysisRequest& request) {
    const linker::CroModule& module = *request.module;
    const ctr::CroHeader& header = module.header;
    ModuleAnalysis analysis;

    const std::optional<CodeRange> code = CodeOf(header);
    if (!code) {
        return analysis;
    }
    analysis.report.code_bytes = code->end - code->begin;

    const core::Result<linker::LoadedModule> linked = LinkAlone(static_module, module);
    if (!linked) {
        return linked.Failure().WithContext(header.name);
    }

    const core::Result<SeededTables> exported = SeedFromExports(header, module.named_exports);
    if (!exported) {
        return exported.Failure().WithContext(header.name);
    }
    analysis.symbols = exported.Value().symbols;
    analysis.report.exported_functions = exported.Value().functions.size();

    std::vector<WalkSeed> seeds;
    for (const FunctionRange& function : exported.Value().functions) {
        seeds.push_back(WalkSeed{.address = function.address,
                                 .instruction_set = function.instruction_set,
                                 .source = Provenance::CrsExport});
    }

    // A position another module imports from this one's code is an entry point
    // the module offers, whether or not it has a name.
    for (const ctr::CroSegmentTag tag : request.imported_by_others) {
        const core::Result<std::size_t> position =
            ctr::ResolveCroSegmentTag(header, tag, "a position another module imports");
        if (!position || !code->Contains(static_cast<std::uint32_t>(position.Value()))) {
            continue;
        }
        seeds.push_back(
            SeedAt(static_cast<std::uint32_t>(position.Value()), Provenance::Relocation));
        ++analysis.report.imported_positions;
    }

    // A relocated word holding an address in the module's own code is a code
    // pointer, as a fact rather than a guess.
    for (const linker::LinkedWord& word : linked.Value().internal) {
        if (!word.value.has_value()) {
            continue;
        }
        const WalkSeed seed = SeedAt(word.value.value(), Provenance::Relocation);
        if (!code->Contains(seed.address)) {
            continue;
        }
        seeds.push_back(seed);
        analysis.pointers.push_back(CodePointer{
            .address = word.address, .target = seed.address, .source = Provenance::Relocation});
        ++analysis.report.relocated_code_pointers;
    }
    analysis.report.imported_words = linked.Value().external.size();

    // The walk sees the code segment and nothing else, so that a path running
    // off the end of the code stops there instead of decoding read-only data.
    if (code->end > request.file.size()) {
        return core::Error{
            std::format("the code of {} runs past the end of its file", header.name)};
    }
    const WalkImage image{.bytes = request.file.subspan(code->begin, code->end - code->begin),
                          .base = code->begin};
    WalkOptions options;
    options.follow_literal_pointers = false;
    options.code_begin = code->begin;
    options.code_end = code->end;

    const WalkResult walked = WalkCode(image, seeds, options);
    analysis.report.walk = walked.report;
    analysis.regions = walked.regions;
    for (const Region& region : walked.regions) {
        if (region.kind == RegionKind::Code) {
            analysis.report.code_reached += region.size;
        }
    }

    // Every seed is a function too, for the reason Analyze gives for the main
    // executable's.
    std::vector<FunctionRange> entries;
    entries.reserve(seeds.size() + walked.functions.size());
    for (const WalkSeed& seed : seeds) {
        entries.push_back(FunctionRange{.address = seed.address,
                                        .size = UnknownSize,
                                        .instruction_set = seed.instruction_set,
                                        .alternate_entries = {},
                                        .source = seed.source});
    }
    entries.insert(entries.end(), walked.functions.begin(), walked.functions.end());

    BoundsResult bounds = FindFunctionBounds(image, entries, walked.regions);
    analysis.report.bounds = bounds.report;
    analysis.functions = std::move(bounds.functions);
    return analysis;
}

}  // namespace hexlith::domain::analysis
