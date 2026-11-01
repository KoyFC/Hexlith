#ifndef HEXLITH_DOMAIN_ARM_PRIMITIVES_BRANCH_H
#define HEXLITH_DOMAIN_ARM_PRIMITIVES_BRANCH_H

#include <cstdint>

// What writing the program counter does, and what reading it gives back.
namespace hexlith::domain::arm::primitives {

// How far ahead of an instruction r15 reads: two instructions in ARM, one in
// Thumb, for the pipeline reason instruction.h gives.
constexpr std::uint32_t ArmReadOffset = 8;
constexpr std::uint32_t ThumbReadOffset = 4;

// The value an instruction at `address` reads from r15.
[[nodiscard]] constexpr std::uint32_t ReadProgramCounter(std::uint32_t address, bool thumb) {
    return address + (thumb ? ThumbReadOffset : ArmReadOffset);
}

// The same, rounded down to a word, which is how Thumb reads r15 to take the
// address of a literal: a Thumb instruction can sit at any halfword.
[[nodiscard]] constexpr std::uint32_t ReadAlignedProgramCounter(std::uint32_t address, bool thumb) {
    constexpr std::uint32_t WordMask = ~3U;
    const std::uint32_t value = ReadProgramCounter(address, thumb);
    return thumb ? value & WordMask : value;
}

// BranchWritePC: a branch that keeps the instruction set. The bits below the
// instruction size are ignored, which is what makes a Thumb `mov pc, rX` with an
// odd value land on the even address below it.
[[nodiscard]] constexpr std::uint32_t BranchWriteTarget(std::uint32_t target, bool thumb) {
    constexpr std::uint32_t ArmMask = ~3U;
    constexpr std::uint32_t ThumbMask = ~1U;
    return target & (thumb ? ThumbMask : ArmMask);
}

// BXWritePC: the low bit of the target chooses the instruction set.
[[nodiscard]] constexpr bool ExchangeToThumb(std::uint32_t target) {
    constexpr std::uint32_t ThumbBit = 1;
    return (target & ThumbBit) != 0;
}

// An ARM target with bit 1 set is not word aligned, and the architecture calls
// an exchange to it unpredictable.
[[nodiscard]] constexpr bool ExchangeTargetUnpredictable(std::uint32_t target) {
    constexpr std::uint32_t HalfwordBit = 2;
    return !ExchangeToThumb(target) && (target & HalfwordBit) != 0;
}

// What a data-processing instruction does when it writes r15 is a plain branch
// in both instruction sets: ARMv7 made the ARM form interwork, and the ARM11
// predates it. An ARM target that is not word aligned is unpredictable.
[[nodiscard]] constexpr bool ComputedBranchUnpredictable(std::uint32_t target, bool thumb) {
    constexpr std::uint32_t LowBits = 3;
    return !thumb && (target & LowBits) != 0;
}

}  // namespace hexlith::domain::arm::primitives

#endif  // HEXLITH_DOMAIN_ARM_PRIMITIVES_BRANCH_H
