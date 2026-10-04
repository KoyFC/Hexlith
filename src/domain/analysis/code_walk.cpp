#include "domain/analysis/code_walk.h"

#include "domain/arm/decode_arm.h"
#include "domain/arm/decode_thumb.h"

#include <algorithm>
#include <unordered_set>

namespace hexlith::domain::analysis {
namespace {

namespace arm = domain::arm;

// What is known about one byte. Two instruction sets rather than one flag,
// because an address reached as both is a contradiction worth reporting and not
// a byte that is somehow both.
constexpr std::uint8_t MarkArm = 0x01;
constexpr std::uint8_t MarkThumb = 0x02;
constexpr std::uint8_t MarkInstructionStart = 0x04;
constexpr std::uint8_t MarkData = 0x08;

constexpr std::uint8_t MarkFor(arm::InstructionSet set) {
    return set == arm::InstructionSet::Arm ? MarkArm : MarkThumb;
}

constexpr std::uint8_t OtherMarkFor(arm::InstructionSet set) {
    return set == arm::InstructionSet::Arm ? MarkThumb : MarkArm;
}

// The interworking bit, which a function pointer carries and an address does not.
constexpr std::uint32_t ThumbBit = 1;

std::uint32_t ReadWord(const WalkImage& image, std::uint32_t address) {
    const std::size_t offset = address - image.base;
    return core::ReadU32At(image.bytes, offset).ValueOr(0);
}

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

// The walk itself, as a class only because it carries five pieces of state that
// every step reads and writes.
class Walk {
public:
    Walk(const WalkImage& image, const WalkOptions& options)
        : image_(image), options_(options), marks_(image.bytes.size(), 0) {}

    void Seed(const WalkSeed& seed) {
        pending_.push_back(seed);
        ++result_.report.seeds;
    }

    // Sweeps a range of data for words that look like the address of a function.
    // Only the ones pointing at something shaped like a prologue are taken, for
    // the reason WalkOptions gives with the measurement behind it.
    void SweepForEntryPoints() {
        if (options_.pointer_scan_end <= options_.pointer_scan_begin) {
            return;
        }

        for (std::uint32_t at = options_.pointer_scan_begin; at + 4 <= options_.pointer_scan_end;
             at += 4) {
            if (!image_.Contains(at) || !image_.Contains(at + 3)) {
                continue;
            }

            const std::uint32_t value = ReadWord(image_, at);
            const arm::InstructionSet set =
                (value & ThumbBit) != 0 ? arm::InstructionSet::Thumb : arm::InstructionSet::Arm;
            const std::uint32_t pointed = value & ~ThumbBit;

            const bool plausible = pointed >= options_.code_begin && pointed < options_.code_end &&
                                   (pointed % arm::Alignment(set)) == 0;
            if (!plausible) {
                continue;
            }

            ++result_.report.words_in_range;
            if (!LooksLikeAnEntryPoint(image_, pointed, set)) {
                ++result_.report.words_without_a_prologue;
                continue;
            }

            result_.pointers.push_back(
                CodePointer{.address = at, .target = pointed, .source = Provenance::Heuristic});
            RecordFunction(pointed, set, Provenance::Heuristic);
            Seed(WalkSeed{
                .address = pointed, .instruction_set = set, .source = Provenance::Heuristic});
        }
    }

    void Run() {
        SweepForEntryPoints();

        while (!pending_.empty()) {
            const WalkSeed seed = pending_.back();
            pending_.pop_back();
            Follow(seed);
        }
        result_.regions = BuildRegions();
    }

    WalkResult Take() {
        return std::move(result_);
    }

private:
    void Follow(const WalkSeed& seed) {
        if (!image_.Contains(seed.address)) {
            ++result_.report.seeds_outside_the_image;
            return;
        }
        if ((seed.address % arm::Alignment(seed.instruction_set)) != 0) {
            ++result_.report.misaligned_seeds;
            return;
        }

        ++result_.report.seeds_followed;

        std::uint32_t address = seed.address;
        while (Step(address, seed)) {
            // Step advances the address, or returns false when the walk stops.
        }
    }

    // Decodes one instruction and acts on it. Returns whether the walk carries on
    // to the instruction after it.
    bool Step(std::uint32_t& address, const WalkSeed& seed) {
        if (!image_.Contains(address)) {
            return false;
        }

        const std::size_t offset = address - image_.base;
        const std::uint8_t mark = marks_.at(offset);

        if ((mark & OtherMarkFor(seed.instruction_set)) != 0) {
            ++result_.report.instruction_set_conflicts;
            return false;
        }
        if ((mark & MarkFor(seed.instruction_set)) != 0 && (mark & MarkInstructionStart) != 0) {
            // Already walked from here, by this path or another.
            return false;
        }

        const arm::Instruction decoded = Decode(address, seed.instruction_set);
        if (!arm::IsDefined(decoded)) {
            ++result_.report.undefined_stops;
            return false;
        }
        if (offset + decoded.width > marks_.size()) {
            return false;
        }

        ++result_.report.instructions;
        for (std::uint8_t byte = 0; byte < decoded.width; ++byte) {
            std::uint8_t& target = marks_.at(offset + byte);
            target = static_cast<std::uint8_t>(target | MarkFor(seed.instruction_set));
            if (byte == 0) {
                target = static_cast<std::uint8_t>(target | MarkInstructionStart);
            }
        }

        FollowBranch(decoded, address, seed);
        FollowLiteral(decoded, address, seed);

        if (!arm::ContinuesToTheNextInstruction(decoded)) {
            return false;
        }

        address += decoded.width;
        return true;
    }

    [[nodiscard]] arm::Instruction Decode(std::uint32_t address, arm::InstructionSet set) const {
        return DecodeAt(image_, address, set);
    }

    void
    FollowBranch(const arm::Instruction& decoded, std::uint32_t address, const WalkSeed& seed) {
        if (!arm::HasKnownTarget(decoded)) {
            return;
        }

        const std::uint32_t target = arm::BranchTarget(decoded, address);

        // Only the exchanging form changes the instruction set, and it always
        // changes it: that is what it is for.
        const arm::InstructionSet set =
            decoded.operation == arm::Operation::BranchLinkExchangeImmediate
                ? Other(seed.instruction_set)
                : seed.instruction_set;

        if (!image_.Contains(target)) {
            ++result_.report.targets_outside_the_image;
            return;
        }

        if (arm::IsCall(decoded)) {
            RecordFunction(target, set, Provenance::Heuristic);
        }
        pending_.push_back(
            WalkSeed{.address = target, .instruction_set = set, .source = Provenance::Heuristic});
    }

    // A read from a literal pool is two facts at once: those four bytes are data
    // inside a function, and the value in them may be the only evidence that
    // whatever it points at is code.
    void
    FollowLiteral(const arm::Instruction& decoded, std::uint32_t address, const WalkSeed& seed) {
        const std::optional<std::uint32_t> at =
            arm::ProgramCounterRelativeAddress(decoded, address);
        if (!at.has_value() || !image_.Contains(at.value())) {
            return;
        }

        const std::size_t offset = at.value() - image_.base;
        if (offset + 4 > marks_.size()) {
            return;
        }

        if ((marks_.at(offset) & MarkData) == 0) {
            ++result_.report.literal_pool_words;
            for (std::size_t byte = 0; byte < 4; ++byte) {
                std::uint8_t& target = marks_.at(offset + byte);
                target = static_cast<std::uint8_t>(target | MarkData);
            }
        }

        if (!options_.follow_literal_pointers || decoded.operation != arm::Operation::LoadWord) {
            return;
        }

        const std::uint32_t value = ReadWord(image_, at.value());
        const bool thumb = (value & ThumbBit) != 0;
        const std::uint32_t pointed = value & ~ThumbBit;
        const arm::InstructionSet set =
            thumb ? arm::InstructionSet::Thumb : arm::InstructionSet::Arm;

        const bool plausible = pointed >= options_.code_begin && pointed < options_.code_end &&
                               (pointed % arm::Alignment(set)) == 0;
        if (!plausible) {
            ++result_.report.pointers_ignored;
            return;
        }

        ++result_.report.pointers_followed;
        result_.pointers.push_back(
            CodePointer{.address = at.value(), .target = pointed, .source = Provenance::Heuristic});
        RecordFunction(pointed, set, Provenance::Heuristic);
        pending_.push_back(
            WalkSeed{.address = pointed, .instruction_set = set, .source = Provenance::Heuristic});

        // seed.source is not used here on purpose: a function found through a
        // pointer is a guess however trustworthy the path that led to it was.
        (void)seed;
    }

    static arm::InstructionSet Other(arm::InstructionSet set) {
        return set == arm::InstructionSet::Arm ? arm::InstructionSet::Thumb
                                               : arm::InstructionSet::Arm;
    }

    void RecordFunction(std::uint32_t address, arm::InstructionSet set, Provenance source) {
        if (!known_functions_.insert(address).second) {
            return;
        }
        result_.functions.push_back(FunctionRange{.address = address,
                                                  .size = UnknownSize,
                                                  .instruction_set = set,
                                                  .alternate_entries = {},
                                                  .source = source});
    }

    // Runs of bytes with the same marking become one region each. A region is
    // emitted only for what the walk actually reached, so a gap in the output is
    // a gap in what is known rather than a claim that the bytes are data.
    [[nodiscard]] std::vector<Region> BuildRegions() const {
        std::vector<Region> regions;

        // The mark that says an instruction starts here is deliberately left out
        // of the comparison. It differs between the first byte of an instruction
        // and the rest of it, so including it would end a region at every
        // instruction boundary and produce one region per two bytes.
        constexpr std::uint8_t KindMarks = MarkArm | MarkThumb | MarkData;

        std::size_t index = 0;
        while (index < marks_.size()) {
            const auto mark = static_cast<std::uint8_t>(marks_.at(index) & KindMarks);
            if (mark == 0) {
                ++index;
                continue;
            }

            const std::size_t start = index;
            while (index < marks_.size() &&
                   static_cast<std::uint8_t>(marks_.at(index) & KindMarks) == mark) {
                ++index;
            }

            const bool code = (mark & (MarkArm | MarkThumb)) != 0;
            Region region;
            region.address = static_cast<std::uint32_t>(image_.base + start);
            region.size = static_cast<std::uint32_t>(index - start);
            region.source = Provenance::Heuristic;

            if (code) {
                region.kind = RegionKind::Code;
                region.instruction_set =
                    (mark & MarkArm) != 0 ? arm::InstructionSet::Arm : arm::InstructionSet::Thumb;
            } else {
                region.kind = RegionKind::Data;
            }

            regions.push_back(region);
        }

        return regions;
    }

    WalkImage image_;
    WalkOptions options_;
    std::vector<std::uint8_t> marks_;
    std::vector<WalkSeed> pending_;
    std::unordered_set<std::uint32_t> known_functions_;
    WalkResult result_;
};

}  // namespace

bool LooksLikeAnEntryPoint(const WalkImage& image, std::uint32_t address, arm::InstructionSet set) {
    if (!image.Contains(address) || (address % arm::Alignment(set)) != 0) {
        return false;
    }

    const arm::Instruction decoded = DecodeAt(image, address, set);
    if (!arm::IsDefined(decoded)) {
        return false;
    }

    // Saving registers on the stack.
    if (decoded.operation == arm::Operation::StoreMultiple && decoded.first == arm::StackPointer) {
        return true;
    }

    // Making room for locals.
    const bool adjusts_the_stack =
        decoded.operation == arm::Operation::Sub || decoded.operation == arm::Operation::Add;
    if (adjusts_the_stack && decoded.destination == arm::StackPointer) {
        return true;
    }

    // Saving one register, which is how a function that calls exactly one other
    // keeps its return address.
    return decoded.operation == arm::Operation::StoreWord && decoded.first == arm::StackPointer &&
           decoded.writes_back;
}

WalkResult
WalkCode(const WalkImage& image, std::span<const WalkSeed> seeds, const WalkOptions& options) {
    Walk walk{image, options};
    for (const WalkSeed& seed : seeds) {
        walk.Seed(seed);
    }
    walk.Run();
    return walk.Take();
}

}  // namespace hexlith::domain::analysis
