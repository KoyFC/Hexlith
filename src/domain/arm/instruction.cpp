#include "domain/arm/instruction.h"

namespace hexlith::domain::arm {
namespace {

// How far ahead of the instruction the program counter reads. Two instructions
// in ARM and one in Thumb, which is what the pipeline exposed when the
// encodings were fixed and what every displacement is counted from since.
constexpr std::uint32_t ArmPipelineOffset = 8;
constexpr std::uint32_t ThumbPipelineOffset = 4;

std::uint32_t PipelineOffset(InstructionSet set) {
    return set == InstructionSet::Arm ? ArmPipelineOffset : ThumbPipelineOffset;
}

bool TransfersSingleWord(Operation operation) {
    switch (operation) {
    case Operation::LoadWord:
    case Operation::LoadByte:
    case Operation::LoadHalf:
    case Operation::LoadSignedByte:
    case Operation::LoadSignedHalf:
    case Operation::LoadDouble:
    case Operation::StoreWord:
    case Operation::StoreByte:
    case Operation::StoreHalf:
    case Operation::StoreDouble:
        return true;
    default:
        return false;
    }
}

}  // namespace

std::string_view ShiftName(ShiftKind kind) {
    switch (kind) {
    case ShiftKind::LogicalLeft:
        return "lsl";
    case ShiftKind::LogicalRight:
        return "lsr";
    case ShiftKind::ArithmeticRight:
        return "asr";
    case ShiftKind::RotateRight:
        return "ror";
    case ShiftKind::RotateRightExtended:
        return "rrx";
    }
    return "?";
}

std::string ParallelMnemonic(ParallelPrefix prefix, ParallelShape shape) {
    std::string name;
    switch (prefix) {
    case ParallelPrefix::Signed:
        name = "s";
        break;
    case ParallelPrefix::SignedSaturating:
        name = "q";
        break;
    case ParallelPrefix::SignedHalving:
        name = "sh";
        break;
    case ParallelPrefix::Unsigned:
        name = "u";
        break;
    case ParallelPrefix::UnsignedSaturating:
        name = "uq";
        break;
    case ParallelPrefix::UnsignedHalving:
        name = "uh";
        break;
    }

    switch (shape) {
    case ParallelShape::Add16:
        return name + "add16";
    case ParallelShape::AddSubtractExchange:
        return name + "asx";
    case ParallelShape::SubtractAddExchange:
        return name + "sax";
    case ParallelShape::Subtract16:
        return name + "sub16";
    case ParallelShape::Add8:
        return name + "add8";
    case ParallelShape::Subtract8:
        return name + "sub8";
    }
    return name;
}

bool WritesDestination(Operation operation) {
    switch (operation) {
    // The four comparisons exist to set flags and have no destination.
    case Operation::Tst:
    case Operation::Teq:
    case Operation::Cmp:
    case Operation::Cmn:
    // A store reads its destination field as a source.
    case Operation::StoreWord:
    case Operation::StoreByte:
    case Operation::StoreHalf:
    case Operation::StoreDouble:
    case Operation::StoreWordUnprivileged:
    case Operation::StoreByteUnprivileged:
    case Operation::StoreMultiple:
    case Operation::StoreCoprocessor:
    case Operation::StoreReturnState:
    // These write somewhere other than a general register, or nowhere.
    case Operation::Undefined:
    case Operation::Branch:
    case Operation::BranchLink:
    case Operation::BranchExchange:
    case Operation::BranchLinkExchangeRegister:
    case Operation::BranchLinkExchangeImmediate:
    case Operation::MoveToStatus:
    case Operation::MoveToCoprocessor:
    case Operation::MoveToCoprocessorPair:
    case Operation::CoprocessorDataOperation:
    case Operation::SoftwareInterrupt:
    case Operation::Breakpoint:
    case Operation::SecureMonitorCall:
    case Operation::Preload:
    case Operation::NoOperation:
    case Operation::Yield:
    case Operation::WaitForEvent:
    case Operation::WaitForInterrupt:
    case Operation::SendEvent:
    case Operation::SetEndianness:
    case Operation::ChangeProcessorState:
    case Operation::ClearExclusive:
    case Operation::ReturnFromException:
    case Operation::ParallelArithmetic:
    // Floating point writes floating point registers, memory or FPSCR. The four
    // moves that write a general register are the exception.
    case Operation::VfpMultiplyAccumulate:
    case Operation::VfpMultiplySubtract:
    case Operation::VfpNegatedMultiplyAccumulate:
    case Operation::VfpNegatedMultiplySubtract:
    case Operation::VfpMultiply:
    case Operation::VfpNegatedMultiply:
    case Operation::VfpAdd:
    case Operation::VfpSubtract:
    case Operation::VfpDivide:
    case Operation::VfpCopy:
    case Operation::VfpAbsolute:
    case Operation::VfpNegate:
    case Operation::VfpSquareRoot:
    case Operation::VfpCompare:
    case Operation::VfpCompareSignalling:
    case Operation::VfpConvertPrecision:
    case Operation::VfpConvertFromInteger:
    case Operation::VfpConvertToInteger:
    case Operation::VfpMoveToSingle:
    case Operation::VfpMoveToHalf:
    case Operation::VfpMoveToPair:
    case Operation::VfpMoveToSystem:
    case Operation::VfpLoad:
    case Operation::VfpStore:
    case Operation::VfpLoadMultiple:
    case Operation::VfpStoreMultiple:
        return false;
    default:
        return true;
    }
}

bool WritesProgramCounter(const Instruction& instruction) {
    switch (instruction.operation) {
    case Operation::Branch:
    case Operation::BranchLink:
    case Operation::BranchExchange:
    case Operation::BranchLinkExchangeRegister:
    case Operation::BranchLinkExchangeImmediate:
    case Operation::ReturnFromException:
        return true;
    case Operation::LoadMultiple:
        return Holds(instruction.registers, ProgramCounter);
    default:
        break;
    }

    if (WritesDestination(instruction.operation) && instruction.destination == ProgramCounter) {
        return true;
    }

    // A transfer that writes back leaves the address in its base register, and
    // the base can be r15 as easily as anything else.
    return instruction.writes_back && instruction.first == ProgramCounter &&
           TransfersSingleWord(instruction.operation);
}

bool HasKnownTarget(const Instruction& instruction) {
    switch (instruction.operation) {
    case Operation::Branch:
    case Operation::BranchLink:
    case Operation::BranchLinkExchangeImmediate:
        return true;
    default:
        return false;
    }
}

std::uint32_t BranchTarget(const Instruction& instruction, std::uint32_t address) {
    const std::uint32_t origin = address + PipelineOffset(instruction.instruction_set);
    const std::uint32_t target = origin + static_cast<std::uint32_t>(instruction.branch_offset);

    // A branch from Thumb that exchanges into ARM lands on a word boundary, and
    // the architecture rounds down to reach one rather than refusing. It has to:
    // a Thumb instruction can sit at any even address, so the program counter it
    // reads is not word aligned half the time, and the displacement cannot make
    // up for it.
    const bool exchanges_into_arm =
        instruction.operation == Operation::BranchLinkExchangeImmediate &&
        instruction.instruction_set == InstructionSet::Thumb;
    return exchanges_into_arm ? target & ~3U : target;
}

bool IsCall(const Instruction& instruction) {
    switch (instruction.operation) {
    case Operation::BranchLink:
    case Operation::BranchLinkExchangeRegister:
    case Operation::BranchLinkExchangeImmediate:
        return true;
    default:
        return false;
    }
}

bool IsReturn(const Instruction& instruction) {
    if (instruction.operation == Operation::BranchExchange) {
        return instruction.second == LinkRegister;
    }

    if (instruction.operation == Operation::Mov && instruction.destination == ProgramCounter) {
        return instruction.operand == OperandForm::Register && instruction.second == LinkRegister;
    }

    // A function that saved the link register returns by loading it straight
    // into the program counter, which is what makes a pop a return rather than
    // a branch to wherever the stack points.
    if (instruction.operation == Operation::LoadMultiple) {
        return Holds(instruction.registers, ProgramCounter) && instruction.first == StackPointer;
    }

    return false;
}

bool ContinuesToTheNextInstruction(const Instruction& instruction) {
    if (!WritesProgramCounter(instruction)) {
        return true;
    }

    // A call writes the program counter and comes back, so the instruction
    // after it is reachable. That it might not come back, because the function
    // called never returns, is not something the instruction can say.
    if (IsCall(instruction)) {
        return true;
    }

    // A condition that can fail leaves the following instruction reachable, and
    // one that cannot does not.
    return IsConditional(instruction.condition);
}

std::optional<std::uint32_t> ProgramCounterRelativeAddress(const Instruction& instruction,
                                                           std::uint32_t address) {
    // A floating point constant is loaded from a pool the same way an integer
    // one is, by a single transfer relative to the program counter.
    if (!TransfersSingleWord(instruction.operation) &&
        instruction.operation != Operation::VfpLoad) {
        return std::nullopt;
    }
    if (instruction.first != ProgramCounter || instruction.operand != OperandForm::Immediate) {
        return std::nullopt;
    }

    // The program counter a Thumb instruction reads is rounded down to a
    // multiple of four, because a literal pool holds words and a Thumb
    // instruction can sit at any even address. Forgetting it reads two bytes into
    // the wrong word for half the instructions in a function.
    std::uint32_t origin = address + PipelineOffset(instruction.instruction_set);
    if (instruction.instruction_set == InstructionSet::Thumb) {
        origin &= ~3U;
    }

    // A post-indexed transfer reads the base before the offset is applied, so
    // the address is the program counter itself.
    if (!instruction.preindexed) {
        return origin;
    }

    return instruction.add_offset ? origin + instruction.immediate : origin - instruction.immediate;
}

}  // namespace hexlith::domain::arm
