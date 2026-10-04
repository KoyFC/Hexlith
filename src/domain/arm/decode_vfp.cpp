#include "domain/arm/decode_vfp.h"

#include "domain/arm/condition.h"

#include <algorithm>
#include <initializer_list>

namespace hexlith::domain::arm {
namespace {

constexpr std::uint32_t Bits(std::uint32_t word, unsigned high, unsigned low) {
    const unsigned width = high - low + 1;
    const std::uint32_t mask = width == 32 ? ~0U : (1U << width) - 1U;
    return (word >> low) & mask;
}

constexpr bool Bit(std::uint32_t word, unsigned index) {
    return ((word >> index) & 1U) != 0;
}

constexpr bool AllZero(std::uint32_t word, unsigned high, unsigned low) {
    return Bits(word, high, low) == 0;
}

constexpr RegisterIndex CoreRegisterAt(std::uint32_t word, unsigned low) {
    return static_cast<RegisterIndex>(Bits(word, low + 3, low));
}

// VFPv2 has sixteen double registers. VFPv3 doubled that to thirty-two and
// found the fifth bit of the register number in the bit that extends a single
// register number, so a double with that bit set is a VFPv3 instruction.
constexpr unsigned DoubleRegisterCount = 16;
constexpr unsigned SingleRegisterCount = 32;

// A floating point register number is four bits of the word and one more bit
// elsewhere, and which end the extra bit goes on depends on the precision. A
// single register keeps it as the low bit, so s0 to s31 come out in order; a
// double keeps it as the high bit, which is what let VFPv3 add d16 to d31
// without a new encoding.
//
// Returns NoRegister for a double that needs the fifth bit, which VFPv2 does not
// have.
constexpr RegisterIndex FloatRegister(std::uint32_t four_bits, bool extra_bit, bool is_double) {
    if (!is_double) {
        return static_cast<RegisterIndex>((four_bits << 1U) | (extra_bit ? 1U : 0U));
    }
    if (extra_bit) {
        return NoRegister;
    }
    return static_cast<RegisterIndex>(four_bits);
}

// The three register fields of a data-processing instruction: Vd with D in
// bit 22, Vn with N in bit 7 and Vm with M in bit 5.
RegisterIndex DestinationField(std::uint32_t word, bool is_double) {
    return FloatRegister(Bits(word, 15, 12), Bit(word, 22), is_double);
}

RegisterIndex FirstField(std::uint32_t word, bool is_double) {
    return FloatRegister(Bits(word, 19, 16), Bit(word, 7), is_double);
}

RegisterIndex SecondField(std::uint32_t word, bool is_double) {
    return FloatRegister(Bits(word, 3, 0), Bit(word, 5), is_double);
}

Instruction Undefined(std::uint32_t word) {
    return Instruction{.encoding = word,
                       .operation = Operation::Undefined,
                       .condition = static_cast<Condition>(Bits(word, 31, 28)),
                       .instruction_set = InstructionSet::Arm,
                       .width = 4};
}

Instruction Start(std::uint32_t word, Operation operation) {
    Instruction instruction = Undefined(word);
    instruction.operation = operation;
    return instruction;
}

// Whether any register a field was supposed to name is one VFPv2 does not
// have, which makes the whole instruction a VFPv3 one.
bool AnyMissing(std::initializer_list<RegisterIndex> registers) {
    return std::ranges::any_of(registers, [](RegisterIndex index) { return index == NoRegister; });
}

// The nine operations with two sources, chosen by bits 23, 21 and 20 and then
// by bit 6. VFPv4's fused multiplies sit in the two rows of this table that
// VFPv2 leaves empty, and the other half of the division row is empty in every
// version.
Operation ThreeRegisterOperation(std::uint32_t word) {
    const std::uint32_t row = (Bits(word, 23, 23) << 2U) | Bits(word, 21, 20);
    const bool second_half = Bit(word, 6);

    switch (row) {
    case 0b000:
        return second_half ? Operation::VfpMultiplySubtract : Operation::VfpMultiplyAccumulate;
    case 0b001:
        return second_half ? Operation::VfpNegatedMultiplyAccumulate
                           : Operation::VfpNegatedMultiplySubtract;
    case 0b010:
        return second_half ? Operation::VfpNegatedMultiply : Operation::VfpMultiply;
    case 0b011:
        return second_half ? Operation::VfpSubtract : Operation::VfpAdd;
    case 0b100:
        return second_half ? Operation::Undefined : Operation::VfpDivide;
    default:
        return Operation::Undefined;
    }
}

Instruction DecodeThreeRegister(std::uint32_t word) {
    const Operation operation = ThreeRegisterOperation(word);
    if (operation == Operation::Undefined) {
        return Undefined(word);
    }

    const bool is_double = Bit(word, 8);
    Instruction instruction = Start(word, operation);
    instruction.vfp_destination_double = is_double;
    instruction.vfp_sources_double = is_double;
    instruction.vfp_destination = DestinationField(word, is_double);
    instruction.vfp_first = FirstField(word, is_double);
    instruction.vfp_second = SecondField(word, is_double);
    if (AnyMissing({instruction.vfp_destination, instruction.vfp_first, instruction.vfp_second})) {
        return Undefined(word);
    }
    return instruction;
}

// The shape every instruction with a destination and one source shares, which
// is all of them from here down except the comparisons. The first register
// field is an opcode in that row of the table, so it names nothing.
Instruction
OneSource(std::uint32_t word, Operation operation, bool destination_double, bool source_double) {
    Instruction instruction = Start(word, operation);
    instruction.vfp_destination_double = destination_double;
    instruction.vfp_sources_double = source_double;
    instruction.vfp_destination = DestinationField(word, destination_double);
    instruction.vfp_second = SecondField(word, source_double);
    if (AnyMissing({instruction.vfp_destination, instruction.vfp_second})) {
        return Undefined(word);
    }
    return instruction;
}

// A comparison, with a register or with zero, quiet or signalling. It writes
// nothing but the flags in FPSCR, so the destination field is its first source.
Instruction DecodeCompare(std::uint32_t word, bool with_zero) {
    const bool is_double = Bit(word, 8);
    if (with_zero && (Bit(word, 5) || !AllZero(word, 3, 0))) {
        return Undefined(word);
    }

    Instruction instruction =
        Start(word, Bit(word, 7) ? Operation::VfpCompareSignalling : Operation::VfpCompare);
    instruction.vfp_sources_double = is_double;
    instruction.vfp_first = DestinationField(word, is_double);
    instruction.vfp_second = with_zero ? NoRegister : SecondField(word, is_double);
    if (AnyMissing({instruction.vfp_first}) ||
        (!with_zero && AnyMissing({instruction.vfp_second}))) {
        return Undefined(word);
    }
    return instruction;
}

// Single to double or double to single. Bit 8 is the precision of the source,
// and the destination is the other one.
Instruction DecodeConvertPrecision(std::uint32_t word) {
    const bool from_double = Bit(word, 8);
    return OneSource(word, Operation::VfpConvertPrecision, !from_double, from_double);
}

// An integer held in a single register, converted to the precision bit 8 names.
// Bit 7 says whether the integer is signed.
Instruction DecodeConvertFromInteger(std::uint32_t word) {
    Instruction instruction =
        OneSource(word, Operation::VfpConvertFromInteger, Bit(word, 8), false);
    instruction.integer_signed = Bit(word, 7);
    return instruction;
}

// The precision bit 8 names, converted to an integer left in a single register.
// Bit 16 says whether the integer is signed, and bit 7 whether the conversion
// truncates or rounds in the mode FPSCR selects. A compiler converting a C cast
// truncates; the other form exists for code that wants the rounding mode.
Instruction DecodeConvertToInteger(std::uint32_t word) {
    Instruction instruction = OneSource(word, Operation::VfpConvertToInteger, false, Bit(word, 8));
    instruction.integer_signed = Bit(word, 16);
    instruction.rounds_towards_zero = Bit(word, 7);
    return instruction;
}

// The row of the data-processing table with bits 23, 21 and 20 all set, where
// the first register field is an opcode. Every row it leaves out belongs to a
// later version: immediate moves, half precision conversions and fixed point
// conversions arrived with VFPv3.
Instruction DecodeOtherDataProcessing(std::uint32_t word) {
    // A zero in bit 6 is an immediate move, which is VFPv3.
    if (!Bit(word, 6)) {
        return Undefined(word);
    }

    const std::uint32_t opcode = Bits(word, 19, 16);
    const bool high = Bit(word, 7);

    const bool is_double = Bit(word, 8);

    switch (opcode) {
    case 0b0000:
        return OneSource(
            word, high ? Operation::VfpAbsolute : Operation::VfpCopy, is_double, is_double);
    case 0b0001:
        return OneSource(
            word, high ? Operation::VfpSquareRoot : Operation::VfpNegate, is_double, is_double);
    case 0b0100:
        return DecodeCompare(word, false);
    case 0b0101:
        return DecodeCompare(word, true);
    case 0b0111:
        return high ? DecodeConvertPrecision(word) : Undefined(word);
    case 0b1000:
        return DecodeConvertFromInteger(word);
    case 0b1100:
    case 0b1101:
        return DecodeConvertToInteger(word);
    default:
        return Undefined(word);
    }
}

Instruction DecodeDataProcessing(std::uint32_t word) {
    const bool other = Bit(word, 23) && Bits(word, 21, 20) == 0b11;
    return other ? DecodeOtherDataProcessing(word) : DecodeThreeRegister(word);
}

// The system registers of the floating point unit, by the number the encoding
// gives them. Source: the VFP11 chapter of the ARM1176JZF-S technical reference
// manual, which lists FPSID, FPSCR, FPEXC, FPINST, FPINST2, MVFR0 and MVFR1 and
// nothing else. Only FPSCR is reachable from user mode, which is a question for
// whatever executes the instruction rather than for the decoder.
bool IsSystemRegister(std::uint32_t number) {
    switch (number) {
    case 0b0000:  // FPSID
    case 0b0001:  // FPSCR
    case 0b0110:  // MVFR1
    case 0b0111:  // MVFR0
    case 0b1000:  // FPEXC
    case 0b1001:  // FPINST
    case 0b1010:  // FPINST2
        return true;
    default:
        return false;
    }
}

// The two feature registers describe the unit and cannot be written. FPSID can
// be written, and the write is ignored.
bool IsReadOnlySystemRegister(std::uint32_t number) {
    return number == 0b0110 || number == 0b0111;
}

// A move between a general register and a whole single register, or a system
// register. Bit 8 clear.
Instruction DecodeSingleTransfer(std::uint32_t word) {
    const bool to_core = Bit(word, 20);
    const RegisterIndex core = CoreRegisterAt(word, 12);

    switch (Bits(word, 23, 21)) {
    case 0b000: {
        if (!AllZero(word, 6, 5) || !AllZero(word, 3, 0) || core == ProgramCounter) {
            return Undefined(word);
        }
        Instruction instruction =
            Start(word, to_core ? Operation::VfpMoveFromSingle : Operation::VfpMoveToSingle);
        instruction.destination = core;
        instruction.vfp_destination = FloatRegister(Bits(word, 19, 16), Bit(word, 7), false);
        return instruction;
    }
    case 0b111: {
        if (!AllZero(word, 7, 5) || !AllZero(word, 3, 0)) {
            return Undefined(word);
        }
        if (!IsSystemRegister(Bits(word, 19, 16)) ||
            (!to_core && IsReadOnlySystemRegister(Bits(word, 19, 16)))) {
            return Undefined(word);
        }
        Instruction instruction =
            Start(word, to_core ? Operation::VfpMoveFromSystem : Operation::VfpMoveToSystem);
        instruction.immediate = Bits(word, 19, 16);

        // A read of FPSCR into r15 is the one way to copy the floating point
        // comparison flags into the CPSR's, which is how a compare reaches a
        // conditional branch. It writes the flags and no register.
        if (core == ProgramCounter) {
            constexpr std::uint32_t StatusAndControl = 0b0001;
            if (!to_core || instruction.immediate != StatusAndControl) {
                return Undefined(word);
            }
            instruction.updates_flags = true;
            return instruction;
        }
        instruction.destination = core;
        return instruction;
    }
    default:
        return Undefined(word);
    }
}

// A move between a general register and one half of a double register. Bit 8
// set. Only the 32-bit form is VFPv2; the byte and halfword forms, and the
// duplicating moves in the same space, are Advanced SIMD.
Instruction DecodeHalfTransfer(std::uint32_t word) {
    const bool to_core = Bit(word, 20);
    const RegisterIndex core = CoreRegisterAt(word, 12);

    if (Bit(word, 23) || Bit(word, 22) || !AllZero(word, 6, 5) || !AllZero(word, 3, 0) ||
        core == ProgramCounter) {
        return Undefined(word);
    }

    const RegisterIndex double_register = FloatRegister(Bits(word, 19, 16), Bit(word, 7), true);
    if (double_register == NoRegister) {
        return Undefined(word);
    }

    Instruction instruction =
        Start(word, to_core ? Operation::VfpMoveFromHalf : Operation::VfpMoveToHalf);
    instruction.destination = core;
    instruction.vfp_destination = double_register;
    instruction.vfp_destination_double = true;
    instruction.immediate = Bit(word, 21) ? 1 : 0;
    return instruction;
}

Instruction DecodeRegisterTransfer(std::uint32_t word) {
    return Bit(word, 8) ? DecodeHalfTransfer(word) : DecodeSingleTransfer(word);
}

// Two general registers to or from two consecutive singles or one double. The
// encoding sits in the corner of the transfer space a coprocessor would use for
// MCRR and MRRC.
Instruction DecodePairTransfer(std::uint32_t word) {
    const bool to_core = Bit(word, 20);
    const bool is_double = Bit(word, 8);
    const RegisterIndex low = CoreRegisterAt(word, 12);
    const RegisterIndex high = CoreRegisterAt(word, 16);

    if (!AllZero(word, 7, 6) || !Bit(word, 4)) {
        return Undefined(word);
    }
    if (low == ProgramCounter || high == ProgramCounter || (to_core && low == high)) {
        return Undefined(word);
    }

    const RegisterIndex floating = FloatRegister(Bits(word, 3, 0), Bit(word, 5), is_double);

    // The pair of singles starts at the register named and takes the next one,
    // so it cannot start at s31.
    if (floating == NoRegister || (!is_double && floating == SingleRegisterCount - 1)) {
        return Undefined(word);
    }

    Instruction instruction =
        Start(word, to_core ? Operation::VfpMoveFromPair : Operation::VfpMoveToPair);
    instruction.destination = low;
    instruction.destination_high = high;
    instruction.vfp_destination = floating;
    instruction.vfp_destination_double = is_double;
    return instruction;
}

// One register to or from memory, at a base register plus or minus a scaled
// eight-bit offset. Nothing is written back.
Instruction DecodeSingleMemoryTransfer(std::uint32_t word) {
    const bool is_double = Bit(word, 8);
    const RegisterIndex floating = DestinationField(word, is_double);
    if (floating == NoRegister) {
        return Undefined(word);
    }

    Instruction instruction = Start(word, Bit(word, 20) ? Operation::VfpLoad : Operation::VfpStore);
    instruction.first = CoreRegisterAt(word, 16);
    instruction.vfp_destination = floating;
    instruction.vfp_destination_double = is_double;
    instruction.operand = OperandForm::Immediate;
    instruction.immediate = Bits(word, 7, 0) * 4;
    instruction.preindexed = true;
    instruction.add_offset = Bit(word, 23);
    return instruction;
}

// A run of consecutive registers to or from memory, incrementing after or
// decrementing before, the two modes a stack needs.
//
// The eight-bit field counts words. In double precision an odd count is the
// FLDMX and FSTMX form, which moves the doubles the even part names and one
// word more, so that a routine saving registers it knows nothing about can
// record the format it used. It is deprecated and still part of VFPv2.
Instruction DecodeMultipleMemoryTransfer(std::uint32_t word) {
    const bool is_double = Bit(word, 8);
    const bool writes_back = Bit(word, 21);
    const RegisterIndex base = CoreRegisterAt(word, 16);
    const std::uint32_t words = Bits(word, 7, 0);
    const std::uint32_t count = is_double ? words / 2 : words;
    const RegisterIndex first = DestinationField(word, is_double);
    const unsigned bank = is_double ? DoubleRegisterCount : SingleRegisterCount;

    if (first == NoRegister || count == 0 || first + count > bank) {
        return Undefined(word);
    }
    if (writes_back && base == ProgramCounter) {
        return Undefined(word);
    }

    Instruction instruction =
        Start(word, Bit(word, 20) ? Operation::VfpLoadMultiple : Operation::VfpStoreMultiple);
    instruction.first = base;
    instruction.vfp_destination = first;
    instruction.vfp_destination_double = is_double;
    instruction.vfp_count = static_cast<std::uint8_t>(count);
    instruction.immediate = words * 4;
    instruction.preindexed = Bit(word, 24);
    instruction.add_offset = Bit(word, 23);
    instruction.writes_back = writes_back;
    return instruction;
}

// The transfer space, by the P, U and W bits. Of the eight combinations two are
// not instructions: decrementing after does not exist, and neither does
// incrementing before with write-back.
Instruction DecodeMemoryTransfer(std::uint32_t word) {
    // P and U both clear with D set and W clear is where the pair transfers
    // live, the same corner MCRR and MRRC use.
    if (Bits(word, 24, 21) == 0b0010) {
        return DecodePairTransfer(word);
    }

    const bool preindexed = Bit(word, 24);
    const bool add = Bit(word, 23);
    const bool writes_back = Bit(word, 21);

    if (preindexed && !writes_back) {
        return DecodeSingleMemoryTransfer(word);
    }
    if (!preindexed && add) {
        return DecodeMultipleMemoryTransfer(word);
    }
    if (preindexed && !add && writes_back) {
        return DecodeMultipleMemoryTransfer(word);
    }
    return Undefined(word);
}

}  // namespace

Instruction DecodeVfp(std::uint32_t word) {
    if (Bits(word, 27, 25) == 0b110) {
        return DecodeMemoryTransfer(word);
    }
    return Bit(word, 4) ? DecodeRegisterTransfer(word) : DecodeDataProcessing(word);
}

}  // namespace hexlith::domain::arm
