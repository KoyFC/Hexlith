#include "domain/analysis/jump_table.h"

#include "domain/arm/decode_arm.h"

namespace hexlith::domain::analysis {
namespace {

namespace arm = domain::arm;

constexpr std::uint32_t WordSize = 4;

// What the program counter reads in ARM, which is where the table starts
// relative to the load.
constexpr std::uint32_t TableOffset = 8;

constexpr unsigned TableIndexShift = 2;

std::uint32_t ReadWord(const WalkImage& image, std::uint32_t address) {
    return core::ReadU32At(image.bytes, address - image.base).ValueOr(0);
}

// How many entries the comparison before the load allows, when there is one.
std::optional<std::uint32_t>
BoundBefore(const WalkImage& image, std::uint32_t address, const arm::Instruction& dispatch) {
    if (address < image.base + WordSize) {
        return std::nullopt;
    }
    const arm::Instruction before = arm::DecodeArm(ReadWord(image, address - WordSize));
    const bool compares_the_index =
        before.operation == arm::Operation::Cmp && before.condition == arm::Condition::Always &&
        before.first == dispatch.second && before.operand == arm::OperandForm::Immediate;
    if (!compares_the_index) {
        return std::nullopt;
    }
    switch (dispatch.condition) {
    case arm::Condition::LowerOrSame:
        return before.immediate + 1;
    case arm::Condition::CarryClear:
        return before.immediate;
    default:
        return std::nullopt;
    }
}

bool IsCodeAddress(std::uint32_t value, std::uint32_t code_begin, std::uint32_t code_end) {
    return value >= code_begin && value < code_end && (value % WordSize) == 0;
}

}  // namespace

bool IsTableDispatch(const arm::Instruction& instruction) {
    return instruction.operation == arm::Operation::LoadWord &&
           instruction.instruction_set == arm::InstructionSet::Arm &&
           instruction.destination == arm::ProgramCounter &&
           instruction.first == arm::ProgramCounter &&
           instruction.operand == arm::OperandForm::ShiftedRegister &&
           instruction.shift == arm::ShiftKind::LogicalLeft &&
           instruction.shift_amount == TableIndexShift &&
           instruction.shift_register == arm::NoRegister && instruction.preindexed &&
           instruction.add_offset && !instruction.writes_back;
}

std::optional<JumpTable> ReadJumpTable(const WalkImage& image,
                                       std::uint32_t address,
                                       const arm::Instruction& dispatch,
                                       std::uint32_t code_begin,
                                       std::uint32_t code_end) {
    if (!IsTableDispatch(dispatch)) {
        return std::nullopt;
    }

    JumpTable table{.address = address + TableOffset, .targets = {}};
    const std::optional<std::uint32_t> bound = BoundBefore(image, address, dispatch);
    const std::uint32_t limit =
        bound.has_value() ? *bound : static_cast<std::uint32_t>(MaximumUnboundedEntries);

    for (std::uint32_t entry = 0; entry < limit; ++entry) {
        const std::uint32_t at = table.address + (entry * WordSize);
        if (!image.Contains(at) || !image.Contains(at + WordSize - 1)) {
            break;
        }
        const std::uint32_t value = ReadWord(image, at);
        if (!IsCodeAddress(value, code_begin, code_end)) {
            break;
        }
        table.targets.push_back(value);
    }

    // A comparison that bounds the table is a promise about how many entries
    // there are, so a table that stops short of it is not the table it was
    // taken for. Without one, the plausible words are all there is to go on, and
    // one is not enough to call a table.
    constexpr std::size_t MinimumUnbounded = 2;
    const bool complete = bound.has_value() ? table.targets.size() == *bound && *bound > 0
                                            : table.targets.size() >= MinimumUnbounded;
    if (!complete) {
        return std::nullopt;
    }
    return table;
}

}  // namespace hexlith::domain::analysis
