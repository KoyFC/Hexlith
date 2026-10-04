#ifndef HEXLITH_DOMAIN_ARM_DECODE_THUMB_H
#define HEXLITH_DOMAIN_ARM_DECODE_THUMB_H

#include "domain/arm/instruction.h"

#include <cstdint>

namespace hexlith::domain::arm {

// Decodes one Thumb instruction.
//
// ARMv6K has Thumb-1: sixteen bits per instruction, except for the branch with
// link, which is a pair of halfwords and the only instruction here four bytes
// long. Thumb-2 belongs to ARMv6T2 and the ARM11 does not have it, so a word
// that would be a 32-bit Thumb-2 instruction on a later core is not one here.
//
// The second halfword is read only when the first is the prefix of a branch with
// link. A caller at the end of a buffer passes zero, which makes the pair
// decode as Undefined rather than as a branch to somewhere invented.
//
// The returned instruction carries its width, which the caller needs in order to
// know where the next one starts. A Thumb instruction stream cannot be walked
// backwards or entered at an arbitrary offset for that reason: the only way to
// know where instructions begin is to start somewhere known and follow.
[[nodiscard]] Instruction DecodeThumb(std::uint16_t first, std::uint16_t second);

}  // namespace hexlith::domain::arm

#endif  // HEXLITH_DOMAIN_ARM_DECODE_THUMB_H
