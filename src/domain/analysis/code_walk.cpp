#include "domain/analysis/code_walk.h"

#include "domain/analysis/jump_table.h"
#include "domain/arm/decode_arm.h"
#include "domain/arm/decode_thumb.h"

#include <algorithm>
#include <array>
#include <optional>
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
constexpr std::uint8_t MarkJumpTable = 0x10;

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

// How much a place to walk from is worth, which decides when it is walked.
//
// The first path to reach a byte claims it in its instruction set, and a later
// path that disagrees stops there as a conflict. So the order is not neutral: if
// a guess is walked before a fact, the guess wins. A module that imports
// 0x00100184 as ARM lost to a Thumb path the data sweep had started, and the
// function record took the instruction set of the region the guess had left.
//
// Every stated place, each a fact the image or a loader acts on, is walked
// before anything found by following one, and everything followed from a fact
// before any guess.
enum class Confidence : std::uint8_t { Stated, Followed, Guessed };

constexpr std::size_t ConfidenceLevels = 3;

constexpr Confidence ConfidenceOf(Provenance source) {
    return source == Provenance::Heuristic ? Confidence::Guessed : Confidence::Stated;
}

// A branch is as good as the path it was found on. A path from a fact may still
// be followed into data by a branch the decoder took wrongly, so it ranks below
// the fact itself, but above anything a guess started.
constexpr Confidence FollowedFrom(Confidence path) {
    return path == Confidence::Guessed ? Confidence::Guessed : Confidence::Followed;
}

// The walk itself, as a class only because it carries five pieces of state that
// every step reads and writes.
class Walk {
public:
    Walk(const WalkImage& image, const WalkOptions& options)
        : image_(image), options_(options), marks_(image.bytes.size(), 0) {}

    void Seed(const WalkSeed& seed) {
        Push(seed, ConfidenceOf(seed.source));
        ++result_.report.seeds;
    }

    // Sweeps a range of data for words that look like the address of a function.
    // A word is taken when it points at something shaped like a prologue, or when
    // it sits in a run of such words: the table a virtual call goes through is a
    // run of function addresses, and the functions in it are often leaves that
    // start with no prologue at all. Both rules are in WalkOptions with the
    // measurements behind them.
    void SweepForEntryPoints() {
        if (options_.pointer_scan_end <= options_.pointer_scan_begin) {
            return;
        }

        std::vector<std::uint32_t> candidates;
        for (std::uint32_t at = options_.pointer_scan_begin; at + 4 <= options_.pointer_scan_end;
             at += 4) {
            if (image_.Contains(at) && image_.Contains(at + 3) && IsPlausiblePointer(at)) {
                candidates.push_back(at);
            }
        }

        std::size_t index = 0;
        while (index < candidates.size()) {
            std::size_t end = index + 1;
            while (end < candidates.size() && candidates.at(end) == candidates.at(end - 1) + 4) {
                ++end;
            }
            const bool is_table = end - index >= options_.minimum_pointer_run;
            for (std::size_t each = index; each < end; ++each) {
                TakePointer(candidates.at(each), is_table);
            }
            index = end;
        }
    }

    [[nodiscard]] bool IsPlausiblePointer(std::uint32_t at) const {
        const std::uint32_t value = ReadWord(image_, at);
        const arm::InstructionSet set =
            (value & ThumbBit) != 0 ? arm::InstructionSet::Thumb : arm::InstructionSet::Arm;
        const std::uint32_t pointed = value & ~ThumbBit;
        return pointed >= options_.code_begin && pointed < options_.code_end &&
               (pointed % arm::Alignment(set)) == 0;
    }

    // Whether the walk has already decoded the byte, in either instruction set.
    [[nodiscard]] bool IsWalked(std::uint32_t address) const {
        return image_.Contains(address) &&
               (marks_.at(address - image_.base) & (MarkArm | MarkThumb)) != 0;
    }

    void TakePointer(std::uint32_t at, bool in_a_table) {
        const std::uint32_t value = ReadWord(image_, at);
        const arm::InstructionSet set =
            (value & ThumbBit) != 0 ? arm::InstructionSet::Thumb : arm::InstructionSet::Arm;
        const std::uint32_t pointed = value & ~ThumbBit;

        ++result_.report.words_in_range;
        if (!in_a_table && !LooksLikeAnEntryPoint(image_, pointed, set)) {
            ++result_.report.words_without_a_prologue;
            return;
        }
        if (in_a_table && !LooksLikeAnEntryPoint(image_, pointed, set)) {
            // Thumb is rare in this title and a Thumb address in a table of
            // otherwise plain words is more often an integer than a pointer.
            if (set == arm::InstructionSet::Thumb || IsWalked(pointed)) {
                ++result_.report.words_into_walked_code;
                return;
            }
            ++result_.report.words_taken_from_tables;
        }

        result_.pointers.push_back(
            CodePointer{.address = at, .target = pointed, .source = Provenance::Heuristic});
        RecordFunction(pointed, set, Provenance::Heuristic);
        Seed(WalkSeed{.address = pointed, .instruction_set = set, .source = Provenance::Heuristic});
    }

    void Run() {
        // Everything stated, and everything that follows from it, is walked
        // before the sweep looks at data. What the sweep takes for an entry
        // point without a prologue is judged against that: a word in a run of
        // code addresses that points into code already walked is the address
        // of a case or a label and not of a function, and only what points at
        // bytes nothing has reached is one.
        DrainWhile([](Confidence confidence) { return confidence != Confidence::Guessed; });
        SweepForEntryPoints();
        DrainWhile([](Confidence) { return true; });
        result_.regions = BuildRegions();
    }

    template<class Wanted>
    void DrainWhile(Wanted wanted) {
        for (std::optional<Pending> next = Pop(); next.has_value(); next = Pop()) {
            if (!wanted(next->confidence)) {
                Push(next->seed, next->confidence);
                return;
            }
            Follow(next->seed, next->confidence);
        }
    }

    WalkResult Take() {
        return std::move(result_);
    }

private:
    struct Pending {
        WalkSeed seed;
        Confidence confidence = Confidence::Guessed;
    };

    void Push(const WalkSeed& seed, Confidence confidence) {
        pending_.at(static_cast<std::size_t>(confidence)).push_back(seed);
    }

    // The most recent seed at the most confident level that has one.
    std::optional<Pending> Pop() {
        for (std::size_t level = 0; level < ConfidenceLevels; ++level) {
            std::vector<WalkSeed>& stack = pending_.at(level);
            if (!stack.empty()) {
                const WalkSeed seed = stack.back();
                stack.pop_back();
                return Pending{.seed = seed, .confidence = static_cast<Confidence>(level)};
            }
        }
        return std::nullopt;
    }

    void Follow(const WalkSeed& seed, Confidence confidence) {
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
        while (Step(address, seed, confidence)) {
            // Step advances the address, or returns false when the walk stops.
        }
    }

    // Decodes one instruction and acts on it. Returns whether the walk carries on
    // to the instruction after it.
    bool Step(std::uint32_t& address, const WalkSeed& seed, Confidence confidence) {
        if (!image_.Contains(address)) {
            return false;
        }

        const std::size_t offset = address - image_.base;
        const std::uint8_t mark = marks_.at(offset);

        if ((mark & OtherMarkFor(seed.instruction_set)) != 0) {
            result_.report.instruction_set_conflicts.push_back(address);
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

        FollowBranch(decoded, address, seed, confidence);
        FollowDispatch(decoded, address, seed, confidence);
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

    void FollowBranch(const arm::Instruction& decoded,
                      std::uint32_t address,
                      const WalkSeed& seed,
                      Confidence confidence) {
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

        if (arm::IsCall(decoded) || IsTailCallToAPrologue(decoded, target, set)) {
            RecordFunction(target, set, Provenance::Heuristic);
        }
        Push(WalkSeed{.address = target, .instruction_set = set, .source = Provenance::Heuristic},
             FollowedFrom(confidence));
    }

    // An unconditional branch to a push. A prologue runs once, on the way in, so
    // a branch to one is not a loop or a jump inside a function: it is a tail
    // call, and what it reaches is a function that nothing calls and so
    // nothing else names.
    [[nodiscard]] bool IsTailCallToAPrologue(const arm::Instruction& decoded,
                                             std::uint32_t target,
                                             arm::InstructionSet set) const {
        if (decoded.operation != arm::Operation::Branch ||
            decoded.condition != arm::Condition::Always) {
            return false;
        }
        const arm::Instruction first = DecodeAt(image_, target, set);
        return first.operation == arm::Operation::StoreMultiple &&
               first.first == arm::StackPointer && first.writes_back;
    }

    // A switch compiled to a table: the words after the load are data, and each
    // one is the address of a case, which is code reached by this path.
    void FollowDispatch(const arm::Instruction& decoded,
                        std::uint32_t address,
                        const WalkSeed& seed,
                        Confidence confidence) {
        if (!IsTableDispatch(decoded)) {
            return;
        }

        const std::optional<JumpTable> table =
            ReadJumpTable(image_, address, decoded, options_.code_begin, options_.code_end);
        if (!table.has_value() || TableOverlapsCode(*table)) {
            ++result_.report.jump_tables_rejected;
            return;
        }

        ++result_.report.jump_tables;
        result_.report.jump_table_entries += table->targets.size();

        const std::size_t first = table->address - image_.base;
        for (std::size_t byte = 0; byte < table->targets.size() * 4; ++byte) {
            std::uint8_t& mark = marks_.at(first + byte);
            mark = static_cast<std::uint8_t>(mark | MarkJumpTable);
        }

        for (const std::uint32_t target : table->targets) {
            Push(WalkSeed{.address = target,
                          .instruction_set = seed.instruction_set,
                          .source = Provenance::Heuristic},
                 FollowedFrom(confidence));
        }
    }

    // Whether any byte of the table has already been decoded as an instruction,
    // which would make the words after the load code and not a table.
    [[nodiscard]] bool TableOverlapsCode(const JumpTable& table) const {
        const std::size_t first = table.address - image_.base;
        for (std::size_t byte = 0; byte < table.targets.size() * 4; ++byte) {
            if ((marks_.at(first + byte) & (MarkArm | MarkThumb)) != 0) {
                return true;
            }
        }
        return false;
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
        Push(WalkSeed{.address = pointed, .instruction_set = set, .source = Provenance::Heuristic},
             Confidence::Guessed);

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
        constexpr std::uint8_t KindMarks = MarkArm | MarkThumb | MarkData | MarkJumpTable;

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
            } else if ((mark & MarkJumpTable) != 0) {
                region.kind = RegionKind::JumpTable;
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
    std::array<std::vector<WalkSeed>, ConfidenceLevels> pending_;
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
