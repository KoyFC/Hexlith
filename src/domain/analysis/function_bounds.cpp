#include "domain/analysis/function_bounds.h"

#include "domain/analysis/jump_table.h"
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

// What the program counter reads in ARM, which is where a jump table starts
// relative to the load that goes through it, and how wide an entry is.
constexpr std::uint32_t TableOffset = 8;
constexpr std::uint32_t WordBytes = 4;

// The region of any kind that holds an address.
const Region* RegionAt(std::span<const Region> regions, std::uint32_t address) {
    const auto after = std::ranges::upper_bound(
        regions, address, {}, [](const Region& region) { return region.address; });
    if (after == regions.begin()) {
        return nullptr;
    }
    const Region& candidate = *std::prev(after);
    return address - candidate.address < candidate.size ? &candidate : nullptr;
}

// Follows control flow inside one function and answers where it ends.
//
// Every address reached is recorded, because the end of a function is not the
// furthest branch target: a function whose last instruction is a return reached
// by a conditional branch ends after that return, and one whose body jumps
// backwards ends where the forward path stopped.
//
// A function is not one region. A literal pool in the middle of it, or the table
// of a switch, splits its code into several. So the function may reach any code
// in its instruction set that lies in the unbroken run of regions from its own
// onwards, with data and jump tables in between, and the entries of a jump table
// are followed too. A gap, bytes the walk never reached, ends the run: following
// a branch across one is how a measurement ends up chaining through tail calls
// into half the program. Within the run, a branch to another function's entry
// point is a tail call and is left alone.
class Extent {
public:
    Extent(const WalkImage& image,
           std::span<const Region> regions,
           const std::unordered_set<std::uint32_t>& entries)
        : image_(&image), regions_(regions), entries_(&entries) {}

    void Measure(std::uint32_t entry, arm::InstructionSet set) {
        pending_.push_back(entry);
        entry_ = entry;
        set_ = set;
        FindTheRun(entry);

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
        while (InCode(address)) {
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
                if (!another_function && InCode(target)) {
                    pending_.push_back(target);
                }
            }
            FollowTable(decoded, address);

            if (!arm::ContinuesToTheNextInstruction(decoded)) {
                return;
            }
            address += decoded.width;
        }
    }

    // The cases of a switch that the walk found: the region that follows the
    // load is the table, and each of its words is the address of a case.
    void FollowTable(const arm::Instruction& decoded, std::uint32_t address) {
        if (!IsTableDispatch(decoded)) {
            return;
        }
        const std::uint32_t table = address + TableOffset;
        const Region* region = RegionAt(regions_, table);
        if (region == nullptr || region->kind != RegionKind::JumpTable ||
            region->address != table) {
            return;
        }
        for (std::uint32_t at = table; at < table + region->size; at += WordBytes) {
            const std::uint32_t target =
                core::ReadU32At(image_->bytes, at - image_->base).ValueOr(0);
            if (InCode(target)) {
                pending_.push_back(target);
            }
        }
    }

    // The unbroken run of regions the entry's region starts, as far as bytes
    // the walk reached go. Padding and code in the other instruction set end it.
    void FindTheRun(std::uint32_t entry) {
        const Region* first = RegionOf(regions_, entry);
        run_begin_ = first == nullptr ? entry : first->address;
        run_end_ = first == nullptr ? entry : first->address + first->size;

        while (true) {
            const Region* next = RegionAt(regions_, run_end_);
            if (next == nullptr || next->address != run_end_) {
                return;
            }
            const bool data = next->kind == RegionKind::Data || next->kind == RegionKind::JumpTable;
            const bool code = next->kind == RegionKind::Code && next->instruction_set == set_;
            if (!data && !code) {
                return;
            }
            run_end_ = next->address + next->size;
        }
    }

    // Whether an address is code in the instruction set this function is in,
    // within the run its entry's region starts.
    [[nodiscard]] bool InCode(std::uint32_t address) const {
        if (address < run_begin_ || address >= run_end_) {
            return false;
        }
        const Region* region = RegionOf(regions_, address);
        return region != nullptr && region->instruction_set.value_or(set_) == set_;
    }

    const WalkImage* image_;
    std::span<const Region> regions_;
    const std::unordered_set<std::uint32_t>* entries_;

    std::uint32_t entry_ = 0;
    arm::InstructionSet set_ = arm::InstructionSet::Arm;
    std::uint32_t run_begin_ = 0;
    std::uint32_t run_end_ = 0;
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

        Extent extent{image, sorted, addresses};
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
