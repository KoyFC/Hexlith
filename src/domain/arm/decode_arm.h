#ifndef HEXLITH_DOMAIN_ARM_DECODE_ARM_H
#define HEXLITH_DOMAIN_ARM_DECODE_ARM_H

#include "domain/arm/instruction.h"

#include <cstdint>

namespace hexlith::domain::arm {

// Decodes one ARM instruction word.
//
// Never fails. A word that is not a valid ARMv6K encoding comes back with
// operation Undefined and its bits intact, because a pass over a megabyte of
// mixed code and data asks this question of data constantly and a failure it
// had to handle at every step would be noise.
//
// The decoder is total in the other direction too: every one of the four
// billion words gets an answer, and the answer for the ones ARMv6K does not
// define is Undefined. That is what makes it usable for telling code from data.
//
// Pure in the word: nothing here needs the address the word came from. The
// queries that do, such as where a branch goes, take the address separately.
[[nodiscard]] Instruction DecodeArm(std::uint32_t word);

}  // namespace hexlith::domain::arm

#endif  // HEXLITH_DOMAIN_ARM_DECODE_ARM_H
