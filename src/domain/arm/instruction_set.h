#ifndef HEXLITH_DOMAIN_ARM_INSTRUCTION_SET_H
#define HEXLITH_DOMAIN_ARM_INSTRUCTION_SET_H

#include <cstdint>
#include <optional>
#include <string_view>

namespace hexlith::domain::arm {

// Which of the two instruction encodings a stretch of code is written in.
//
// The ARM11 decodes both, and which one is in force at a given address is not
// a property of the address: it is a property of the path that reached it.
// The low bit of the target of a BX or of a function pointer carries it, and
// nothing in the image states it. So it has to be recorded alongside every
// range the analysis classifies as code, and a wrong answer decodes garbage
// that still looks like instructions.
//
// ThumbEE and Jazelle are not here. ARMv6K has no ThumbEE, and Jazelle on the
// ARM11 is the trivial implementation that cannot enter Java state, so no
// code in the title can be in either.
enum class InstructionSet : std::uint8_t { Arm, Thumb };

std::string_view Describe(InstructionSet set);

std::optional<InstructionSet> ParseInstructionSet(std::string_view text);

// The width in bytes of the address alignment the encoding requires. A branch
// target that is not aligned to it is a decoding error rather than a jump
// into the middle of an instruction.
//
// Thumb is 2 and not 4 although a 32-bit Thumb-2 instruction is four bytes
// long, because it is two halfwords and may sit at any even address.
[[nodiscard]] constexpr unsigned Alignment(InstructionSet set) {
    return set == InstructionSet::Arm ? 4U : 2U;
}

}  // namespace hexlith::domain::arm

#endif  // HEXLITH_DOMAIN_ARM_INSTRUCTION_SET_H
