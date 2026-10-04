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

    // The SADD16 to UHSUB8 family: six prefixes over eight shapes, 48
    // encodings that do the same thing to different lane widths.
    //
    // Recognised as one operation rather than enumerated. The analysis needs to
    // know that the word is an instruction and that it does not write the
    // program counter, and nothing in this project decodes lanes yet. The
    // encoding is kept on the instruction, so filling this in later is reading
    // a field rather than finding the instructions again.
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
};

// The mnemonic an assembler spells, without the condition suffix and without
// the operand syntax.
std::string_view Mnemonic(Operation operation);

}  // namespace hexlith::domain::arm

#endif  // HEXLITH_DOMAIN_ARM_OPERATION_H
