#ifndef HEXLITH_DOMAIN_ARM_OPERATION_H
#define HEXLITH_DOMAIN_ARM_OPERATION_H

#include <cstdint>
#include <string_view>

namespace hexlith::domain::arm {

// What an instruction does, named once for both encodings.
//
// ARM and Thumb are two spellings of one instruction set, so a decoder for each
// produces values from this enumeration and everything above them is written
// once. A Thumb `push` and an ARM `stmdb sp!` arrive as StoreMultiple with the
// same fields, which is what lets the analysis and the lifter stay encoding
// agnostic.
//
// Undefined is a value and not a failure. A classification pass over four
// megabytes meets data constantly, and asking "did this decode" is the question
// it exists to answer.
enum class Operation : std::uint8_t {
    Undefined,

    // Data processing, in encoding order.
    And,
    Eor,
    Sub,
    Rsb,
    Add,
    Adc,
    Sbc,
    Rsc,
    Tst,
    Teq,
    Cmp,
    Cmn,
    Orr,
    Mov,
    Bic,
    Mvn,

    // Multiply.
    Mul,
    Mla,
    Umull,
    Umlal,
    Umaal,
    Smull,
    Smlal,

    // Halfword and word-by-halfword multiply, the DSP group.
    SmulHalf,
    SmlaHalf,
    SmlalHalf,
    SmulwHalf,
    SmlawHalf,

    // Dual multiply and the most significant word multiplies.
    Smuad,
    Smusd,
    Smlad,
    Smlsd,
    Smlald,
    Smlsld,
    Smmul,
    Smmla,
    Smmls,

    // Saturating arithmetic.
    Qadd,
    Qsub,
    QdoubleAdd,
    QdoubleSub,
    Ssat,
    Ssat16,
    Usat,
    Usat16,

    // The SADD16 to UHSUB8 family: six prefixes over six lane shapes, 36
    // instructions that do the same thing to different lane widths.
    //
    // One operation rather than thirty-six. The prefix and the shape are fields
    // of the instruction, because an implementation of them is one loop over the
    // lanes with the arithmetic chosen by the prefix, and enumerating the
    // product would write that loop thirty-six times.
    ParallelArithmetic,

    // Packing, extension, reversal, selection.
    Pkhbt,
    Pkhtb,
    Sxtab,
    Sxtab16,
    Sxtah,
    Sxtb,
    Sxtb16,
    Sxth,
    Uxtab,
    Uxtab16,
    Uxtah,
    Uxtb,
    Uxtb16,
    Uxth,
    Rev,
    Rev16,
    Revsh,
    Sel,
    Usad8,
    Usada8,
    CountLeadingZeros,

    // Branches. The two exchanging forms are what make the instruction set of
    // an address a property of the path that reached it.
    Branch,
    BranchLink,
    BranchExchange,
    BranchLinkExchangeRegister,
    BranchLinkExchangeImmediate,

    // Single data transfer.
    LoadWord,
    LoadByte,
    StoreWord,
    StoreByte,
    LoadWordUnprivileged,
    LoadByteUnprivileged,
    StoreWordUnprivileged,
    StoreByteUnprivileged,
    LoadHalf,
    StoreHalf,
    LoadSignedByte,
    LoadSignedHalf,
    LoadDouble,
    StoreDouble,

    // Block transfer.
    LoadMultiple,
    StoreMultiple,

    // Synchronisation.
    Swap,
    SwapByte,
    LoadExclusive,
    StoreExclusive,
    LoadExclusiveByte,
    StoreExclusiveByte,
    LoadExclusiveHalf,
    StoreExclusiveHalf,
    LoadExclusiveDouble,
    StoreExclusiveDouble,
    ClearExclusive,

    // Status register access.
    MoveFromStatus,
    MoveToStatus,

    // Coprocessor. On the ARM11 these reach the MMU and cache control through
    // CP15, which the kernel reimplementation will have to answer for.
    CoprocessorDataOperation,
    MoveToCoprocessor,
    MoveFromCoprocessor,
    MoveToCoprocessorPair,
    MoveFromCoprocessorPair,
    LoadCoprocessor,
    StoreCoprocessor,

    // System, hints and memory ordering.
    SoftwareInterrupt,
    Breakpoint,
    SecureMonitorCall,
    Preload,
    NoOperation,
    Yield,
    WaitForEvent,
    WaitForInterrupt,
    SendEvent,
    SetEndianness,
    ChangeProcessorState,
    StoreReturnState,
    ReturnFromException,

    // Floating point, VFPv2. The architecture encodes it as coprocessor
    // instructions on coprocessors 10 and 11, single and double precision, and
    // a word in that space decodes as one of these or as nothing. Named after
    // the unified assembler mnemonics, which is what a disassembler prints.
    //
    // Arithmetic, in the order the encoding table gives it.
    VfpMultiplyAccumulate,
    VfpMultiplySubtract,
    VfpNegatedMultiplyAccumulate,
    VfpNegatedMultiplySubtract,
    VfpMultiply,
    VfpNegatedMultiply,
    VfpAdd,
    VfpSubtract,
    VfpDivide,

    // One source, and the comparisons.
    VfpCopy,
    VfpAbsolute,
    VfpNegate,
    VfpSquareRoot,
    VfpCompare,
    VfpCompareSignalling,
    VfpConvertPrecision,
    VfpConvertFromInteger,
    VfpConvertToInteger,

    // Moves between the general registers and the floating point ones: a single
    // register, half of a double, a pair, and the system registers.
    VfpMoveToSingle,
    VfpMoveFromSingle,
    VfpMoveToHalf,
    VfpMoveFromHalf,
    VfpMoveToPair,
    VfpMoveFromPair,
    VfpMoveToSystem,
    VfpMoveFromSystem,

    // Transfers to and from memory.
    VfpLoad,
    VfpStore,
    VfpLoadMultiple,
    VfpStoreMultiple,
};

// Whether the operation belongs to the floating point unit.
//
// The floating point operations are declared as one block at the end of the
// enumeration, and this relies on it: a new one goes inside the block.
[[nodiscard]] constexpr bool IsFloatingPoint(Operation operation) {
    return operation >= Operation::VfpMultiplyAccumulate &&
           operation <= Operation::VfpStoreMultiple;
}

// The mnemonic an assembler spells, without the condition suffix and without
// the operand syntax.
std::string_view Mnemonic(Operation operation);

}  // namespace hexlith::domain::arm

#endif  // HEXLITH_DOMAIN_ARM_OPERATION_H
