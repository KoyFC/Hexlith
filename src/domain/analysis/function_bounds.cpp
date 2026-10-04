#include "domain/analysis/function_bounds.h"

#include "domain/arm/decode_arm.h"
#include "domain/arm/decode_thumb.h"

#include <algorithm>
#include <map>
#include <unordered_set>

namespace hexlith::domain::analysis {
namespace {

namespace arm = domain::arm;

arm::Instruction DecodeAt(const WalkImage& image, std::uint32_t address, arm::InstructionSet set) {
    const std::size_t offset = address - image.base;

    if (set == arm::InstructionSet::Arm) {
        const core::Result<std::uint32_t> word = core::ReadU32At(image.bytes, offset);
        return word ? arm::DecodeArm(word.Value()) : arm::Instruction{};
    }

    const core::Result<std::uint16_t> first = core::ReadU16At(image.bytes, offset);
    if (!first) {
        return arm::Instruction{};
    }
    const core::Result<std::uint16_t> second = core::ReadU16At(image.bytes, offset + 2);
    return arm::DecodeThumb(first.Value(), second.ValueOr(0));
}

// The code region an address falls in, or nothing when the walk never reached it.
const Region* RegionOf(std::span<const Region> regions, std::uint32_t address) {
    // Regions are sorted by address and do not overlap, so the last one that
    // starts at or before the address is the only candidate.
    const auto after = std::ranges::upper_bound(
        regions, address, {}, [](const Region& region) { return region.address; });
    if (after == regions.begin()) {
        return nullptr;
    }

    const Region& candidate = *std::prev(after);
    if (candidate.kind != RegionKind::Code) {
        return nullptr;
    }
    if (address - candidate.address >= candidate.size) {
        return nullptr;
    }
    return &candidate;
}

// Follows control flow inside one function and answers where it ends.
//
// Every address reached is recorded, because the end of a function is not the
// furthest branch target: a function whose last instruction is a return reached
// by a conditional branch ends after that return, and one whose body jumps
// backwards ends where the forward path stopped.
class Extent {
public:
    Extent(const WalkImage& image,
           const Region& region,
           const std::unordered_set<std::uint32_t>& entries)
        : image_(&image), region_(&region), entries_(&entries) {}

    void Measure(std::uint32_t entry, arm::InstructionSet set) {
        pending_.push_back(entry);
        entry_ = entry;
        set_ = set;

        while (!pending_.empty()) {
            const std::uint32_t address = pending_.back();
            pending_.pop_back();
            Follow(address);
        }
    }

    [[nodiscard]] std::uint32_t End() const {
        return end_;
    }

    [[nodiscard]] const std::vector<std::uint32_t>& Reached() const {
        return reached_;
    }

private:
    void Follow(std::uint32_t address) {
        while (InsideTheRegion(address)) {
            if (!visited_.insert(address).second) {
                return;
            }
            reached_.push_back(address);

            const arm::Instruction decoded = DecodeAt(*image_, address, set_);
            if (!arm::IsDefined(decoded)) {
                return;
            }

            end_ = std::max(end_, address + decoded.width);

            if (arm::HasKnownTarget(decoded) && !arm::IsCall(decoded)) {
                const std::uint32_t target = arm::BranchTarget(decoded, address);

                // A branch to another function's entry point is a tail call. The
                // function it reaches is its own and its bytes are not these.
                const bool another_function = target != entry_ && entries_->contains(target);
                if (!another_function && InsideTheRegion(target)) {
                    pending_.push_back(target);
                }
            }

            if (!arm::ContinuesToTheNextInstruction(decoded)) {
                return;
            }
            address += decoded.width;
        }
    }

    [[nodiscard]] bool InsideTheRegion(std::uint32_t address) const {
        return address >= region_->address && address - region_->address < region_->size;
    }

    const WalkImage* image_;
    const Region* region_;
    const std::unordered_set<std::uint32_t>* entries_;

    std::uint32_t entry_ = 0;
    arm::InstructionSet set_ = arm::InstructionSet::Arm;
    std::uint32_t end_ = 0;
    std::vector<std::uint32_t> pending_;
    std::unordered_set<std::uint32_t> visited_;
    std::vector<std::uint32_t> reached_;
};

}  // namespace

BoundsResult FindFunctionBounds(const WalkImage& image,
                                std::span<const FunctionRange> entries,
                                std::span<const Region> regions) {
    BoundsResult result;
    result.report.entry_points = entries.size();

    // One entry per address, in address order, so that a function is measured
    // before anything that might turn out to be inside it.
    std::map<std::uint32_t, FunctionRange> ordered;
    std::unordered_set<std::uint32_t> addresses;
    for (const FunctionRange& entry : entries) {
        ordered.emplace(entry.address, entry);
        addresses.insert(entry.address);
    }

    std::vector<Region> sorted{regions.begin(), regions.end()};
    std::ranges::sort(sorted, {}, [](const Region& region) { return region.address; });

    // Addresses folded into an earlier function as an alternate entry point.
    std::unordered_set<std::uint32_t> absorbed;

    for (const auto& [address, entry] : ordered) {
        if (absorbed.contains(address)) {
            continue;
        }

        const Region* region = RegionOf(sorted, address);
        if (region == nullptr) {
            result.report.without_an_extent += 1;
            result.functions.push_back(entry);
            continue;
        }

        const arm::InstructionSet set = region->instruction_set.value_or(entry.instruction_set);

        Extent extent{image, *region, addresses};
        extent.Measure(address, set);

        if (extent.End() <= address) {
            result.report.without_an_extent += 1;
            result.functions.push_back(entry);
            continue;
        }

        FunctionRange measured = entry;
        measured.instruction_set = set;
        measured.size = extent.End() - address;
        measured.alternate_entries.clear();

        // An entry point the body ran through is a second way into this function
        // rather than a function of its own. Two of them sharing a tail is
        // ordinary in hand-written code and in what a compiler folds.
        for (const std::uint32_t reached : extent.Reached()) {
            // A body that jumps back past its own entry can run through
            // another entry point there. That one starts before this function
            // does, so it cannot be a way into it.
            const bool inside = reached > address && reached < extent.End();
            if (inside && addresses.contains(reached)) {
                measured.alternate_entries.push_back(reached);
                absorbed.insert(reached);
                result.report.alternate_entries += 1;
            }
        }

        result.report.bytes_covered += measured.size;
        result.report.largest = std::max(result.report.largest, measured.size);
        result.functions.push_back(measured);
    }

    result.report.functions = result.functions.size();

    // Overlaps, counted after the fact because a function that shares a tail with
    // another produces one and so does a wrong entry point.
    std::ranges::sort(
        result.functions, {}, [](const FunctionRange& function) { return function.address; });
    for (std::size_t index = 1; index < result.functions.size(); ++index) {
        const FunctionRange& previous = result.functions[index - 1];
        const FunctionRange& current = result.functions[index];
        if (previous.size != UnknownSize && previous.address + previous.size > current.address) {
            result.report.overlapping += 1;
        }
    }

    return result;
}

}  // namespace hexlith::domain::analysis
