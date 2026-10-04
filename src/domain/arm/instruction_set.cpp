#include "domain/arm/instruction_set.h"

namespace hexlith::domain::arm {

std::string_view Describe(InstructionSet set) {
    switch (set) {
    case InstructionSet::Arm:
        return "arm";
    case InstructionSet::Thumb:
        return "thumb";
    }
    return "unknown";
}

std::optional<InstructionSet> ParseInstructionSet(std::string_view text) {
    if (text == "arm") {
        return InstructionSet::Arm;
    }
    if (text == "thumb") {
        return InstructionSet::Thumb;
    }
    return std::nullopt;
}

}  // namespace hexlith::domain::arm
