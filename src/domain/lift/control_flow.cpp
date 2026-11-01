#include "domain/lift/control_flow.h"

#include "domain/analysis/jump_table.h"
#include "domain/arm/decode_arm.h"
#include "domain/arm/decode_thumb.h"

#include <algorithm>
#include <map>
#include <set>

namespace hexlith::domain::lift {
namespace {

namespace arm = domain::arm;

// What the program counter reads in ARM, which is where a jump table starts
// relative to the load that goes through it.
constexpr std::uint32_t TableOffset = 8;
constexpr std::uint32_t WordBytes = 4;

// Encoding 0b1111 is not a condition that fails: it marks the instructions that
// are always executed, BLX to an immediate among them.
arm::Condition ConditionOf(const arm::Instruction& instruction) {
    return instruction.condition == arm::Condition::Unconditional ? arm::Condition::Always
                                                                  : instruction.condition;
}

arm::Instruction Fetch(const CodeImage& image, std::uint32_t address, arm::InstructionSet set) {
    if (!image.Contains(address)) {
        return arm::Instruction{};
    }
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

// A return, including the form arm::IsReturn leaves out: a single pop of the
// return address, which the assembler spells `pop {pc}` and the architecture
// encodes as a post-indexed load through the stack pointer.
bool IsReturning(const arm::Instruction& instruction) {
    if (arm::IsReturn(instruction)) {
        return true;
    }
    return instruction.operation == arm::Operation::LoadWord &&
           instruction.destination == arm::ProgramCounter &&
           instruction.first == arm::StackPointer && !instruction.preindexed &&
           instruction.add_offset;
}

const analysis::Region* JumpTableAt(std::span<const analysis::Region> regions,
                                    std::uint32_t address) {
    const auto after = std::ranges::upper_bound(
        regions, address, {}, [](const analysis::Region& region) { return region.address; });
    if (after == regions.begin()) {
        return nullptr;
    }
    const analysis::Region& candidate = *std::prev(after);
    const bool starts_here = candidate.address == address;
    return starts_here && candidate.kind == analysis::RegionKind::JumpTable ? &candidate : nullptr;
}

class Builder {
public:
    Builder(const CodeImage& image,
            std::uint32_t entry,
            arm::InstructionSet set,
            const FlowContext& context)
        : image_(image), entry_(entry), set_(set), context_(context) {}

    Function Build() {
        Reach(entry_);
        while (!work_.empty() && instructions_.size() < MaximumInstructions) {
            const std::uint32_t address = work_.back();
            work_.pop_back();
            Scan(address);
        }
        return Assemble();
    }

private:
    void Reach(std::uint32_t address) {
        if (leaders_.insert(address).second) {
            work_.push_back(address);
        }
    }

    // Reads instructions from an address until one ends the block or the path
    // joins one already read.
    void Scan(std::uint32_t address) {
        while (true) {
            if (instructions_.contains(address) || traps_.contains(address)) {
                leaders_.insert(address);
                return;
            }
            if (instructions_.size() >= MaximumInstructions) {
                MakeTrap(address);
                return;
            }

            const arm::Instruction decoded = Fetch(image_, address, set_);
            if (!arm::IsDefined(decoded) || decoded.operation == arm::Operation::Breakpoint) {
                MakeTrap(address);
                return;
            }

            instructions_.emplace(address,
                                  PlacedInstruction{.address = address, .instruction = decoded});
            const std::optional<Exit> exit = Classify(address, decoded);
            if (exit.has_value()) {
                exits_.emplace(address, *exit);
                return;
            }
            address += decoded.width;
        }
    }

    // The interpreter takes over here and says why it cannot go on.
    void MakeTrap(std::uint32_t address) {
        Exit exit;
        exit.kind = ExitKind::Trap;
        exit.target = address;
        traps_.emplace(address, exit);
        leaders_.insert(address);
    }

    // How the instruction ends its block, or nothing when it does not.
    std::optional<Exit> Classify(std::uint32_t address, const arm::Instruction& decoded) {
        const std::uint32_t next = address + decoded.width;
        const arm::Condition condition = ConditionOf(decoded);
        const bool conditional = arm::IsConditional(condition);

        Exit exit;
        exit.condition = condition;

        if (decoded.operation == arm::Operation::SoftwareInterrupt) {
            exit.kind = ExitKind::SystemCall;
            exit.next = Continue(next);
            return exit;
        }
        if (!arm::WritesProgramCounter(decoded)) {
            return std::nullopt;
        }

        switch (decoded.operation) {
        case arm::Operation::Branch:
            return Branch(address, decoded, exit, conditional);
        case arm::Operation::BranchLink:
        case arm::Operation::BranchLinkExchangeImmediate:
            exit.kind = ExitKind::Call;
            exit.target = arm::BranchTarget(decoded, address);
            exit.target_set = decoded.operation == arm::Operation::BranchLinkExchangeImmediate
                                  ? Other(decoded.instruction_set)
                                  : decoded.instruction_set;
            exit.next = Continue(next);
            return exit;
        case arm::Operation::BranchLinkExchangeRegister:
            exit.kind = ExitKind::IndirectCall;
            exit.next = Continue(next);
            return exit;
        default:
            break;
        }

        if (analysis::IsTableDispatch(decoded)) {
            if (std::optional<Exit> table = Table(address, exit, conditional, next)) {
                return table;
            }
        }
        if (std::optional<Exit> veneer = Veneer(address, decoded, exit, conditional, next)) {
            return veneer;
        }
        exit.kind = IsReturning(decoded) ? ExitKind::Return : ExitKind::IndirectJump;
        exit.next = conditional ? Continue(next) : 0;
        return exit;
    }

    std::optional<Exit>
    Branch(std::uint32_t address, const arm::Instruction& decoded, Exit exit, bool conditional) {
        const std::uint32_t target = arm::BranchTarget(decoded, address);
        const std::uint32_t next = address + decoded.width;
        exit.target = target;
        exit.target_set = decoded.instruction_set;
        exit.next = conditional ? Continue(next) : 0;

        const bool another_function =
            target != entry_ && context_.entries != nullptr && context_.entries->contains(target);
        if (another_function || !image_.Contains(target)) {
            exit.kind = ExitKind::TailCall;
            return exit;
        }
        exit.kind = ExitKind::Jump;
        Reach(target);
        return exit;
    }

    // A jump through a literal in the code, `ldr pc, [pc, #-4]` with the address
    // in the word after it. The compiler's way to reach a function too far away
    // for a branch. The image settles where it goes, so it is a branch with a
    // constant target and not an indirect jump.
    std::optional<Exit> Veneer(std::uint32_t address,
                               const arm::Instruction& decoded,
                               Exit exit,
                               bool conditional,
                               std::uint32_t next) {
        if (!context_.literals_are_constant || decoded.operation != arm::Operation::LoadWord ||
            decoded.destination != arm::ProgramCounter) {
            return std::nullopt;
        }
        const std::optional<std::uint32_t> at =
            arm::ProgramCounterRelativeAddress(decoded, address);
        if (!at.has_value() || !image_.Contains(*at)) {
            return std::nullopt;
        }
        const core::Result<std::uint32_t> word = core::ReadU32At(image_.bytes, *at - image_.base);
        if (!word) {
            return std::nullopt;
        }

        constexpr std::uint32_t ThumbBit = 1;
        const bool thumb = (word.Value() & ThumbBit) != 0;
        exit.target = word.Value() & ~ThumbBit;
        exit.target_set = thumb ? arm::InstructionSet::Thumb : arm::InstructionSet::Arm;
        exit.next = conditional ? Continue(next) : 0;

        // The same reasoning as a branch: another function's entry or anywhere
        // outside the image is a tail call, and anything else is part of this
        // function.
        const bool inside = exit.target_set == set_ && image_.Contains(exit.target) &&
                            (exit.target == entry_ || context_.entries == nullptr ||
                             !context_.entries->contains(exit.target));
        if (inside) {
            exit.kind = ExitKind::Jump;
            Reach(exit.target);
        } else {
            exit.kind = ExitKind::TailCall;
        }
        return exit;
    }

    // The cases of a switch, from the table region the analysis found. Without
    // one the load is an indirect jump like any other.
    std::optional<Exit>
    Table(std::uint32_t address, Exit exit, bool conditional, std::uint32_t next) {
        const analysis::Region* region = JumpTableAt(context_.regions, address + TableOffset);
        if (region == nullptr) {
            return std::nullopt;
        }
        exit.kind = ExitKind::Switch;
        exit.table = region->address;
        for (std::uint32_t at = region->address; at < region->address + region->size;
             at += WordBytes) {
            const core::Result<std::uint32_t> word =
                core::ReadU32At(image_.bytes, at - image_.base);
            if (!word) {
                return std::nullopt;
            }
            exit.cases.push_back(word.Value());
            if (image_.Contains(word.Value())) {
                Reach(word.Value());
            }
        }
        exit.next = conditional ? Continue(next) : 0;
        return exit;
    }

    // Marks where control continues after an exit as the start of a block and
    // answers the address.
    std::uint32_t Continue(std::uint32_t address) {
        Reach(address);
        return address;
    }

    static arm::InstructionSet Other(arm::InstructionSet set) {
        return set == arm::InstructionSet::Arm ? arm::InstructionSet::Thumb
                                               : arm::InstructionSet::Arm;
    }

    // Cuts what was read into blocks: one at every leader, ending at the next
    // leader or at an instruction that has an exit.
    Function Assemble() {
        Function function;
        function.entry = entry_;
        function.instruction_set = set_;

        for (const std::uint32_t start : leaders_) {
            if (const auto trap = traps_.find(start); trap != traps_.end()) {
                function.blocks.push_back(BasicBlock{
                    .start = start, .end = start, .instructions = {}, .exit = trap->second});
                continue;
            }
            if (!instructions_.contains(start)) {
                continue;
            }
            function.blocks.push_back(CutBlock(start));
        }
        return function;
    }

    [[nodiscard]] BasicBlock CutBlock(std::uint32_t start) const {
        BasicBlock block;
        block.start = start;

        std::uint32_t address = start;
        while (true) {
            const PlacedInstruction& placed = instructions_.at(address);
            block.instructions.push_back(placed);
            address += placed.instruction.width;
            if (const auto exit = exits_.find(placed.address); exit != exits_.end()) {
                block.exit = exit->second;
                break;
            }
            if (leaders_.contains(address) || !instructions_.contains(address)) {
                block.exit.kind = ExitKind::Fallthrough;
                block.exit.next = address;
                break;
            }
        }
        block.end = address;
        return block;
    }

    CodeImage image_;
    std::uint32_t entry_;
    arm::InstructionSet set_;
    FlowContext context_;

    std::map<std::uint32_t, PlacedInstruction> instructions_;
    std::map<std::uint32_t, Exit> exits_;
    std::map<std::uint32_t, Exit> traps_;
    std::set<std::uint32_t> leaders_;
    std::vector<std::uint32_t> work_;
};

}  // namespace

const BasicBlock* Function::BlockAt(std::uint32_t address) const {
    const auto found = std::ranges::lower_bound(
        blocks, address, {}, [](const BasicBlock& block) { return block.start; });
    return found != blocks.end() && found->start == address ? &*found : nullptr;
}

std::size_t Function::InstructionCount() const {
    std::size_t count = 0;
    for (const BasicBlock& block : blocks) {
        count += block.instructions.size();
    }
    return count;
}

Function BuildFunction(const CodeImage& image,
                       std::uint32_t entry,
                       arm::InstructionSet instruction_set,
                       const FlowContext& context) {
    return Builder{image, entry, instruction_set, context}.Build();
}

}  // namespace hexlith::domain::lift
