#ifndef HEXLITH_DOMAIN_GPU_SHADER_ENCODING_H
#define HEXLITH_DOMAIN_GPU_SHADER_ENCODING_H

#include <array>
#include <cstddef>
#include <cstdint>

// How a PICA200 shader instruction and its operand descriptor are laid out.
// The interpreter reads instructions through this to run them and the GLSL
// translation to write them out, so the two cannot read a field differently.
// Source: GBATEK, 3DS GPU Shader Instruction Set: Opcode Summary, Instruction
// formats, Operand descriptors and Conditions.
namespace hexlith::domain::gpu::shader {

// Six bits from bit 26. CMP takes two opcodes and MAD and MADI eight each,
// their low bits being part of the operands.
namespace opcode {
constexpr std::uint32_t Add = 0x00;
constexpr std::uint32_t Dp3 = 0x01;
constexpr std::uint32_t Dp4 = 0x02;
constexpr std::uint32_t Dph = 0x03;
constexpr std::uint32_t Ex2 = 0x05;
constexpr std::uint32_t Lg2 = 0x06;
constexpr std::uint32_t Mul = 0x08;
constexpr std::uint32_t Sge = 0x09;
constexpr std::uint32_t Slt = 0x0A;
constexpr std::uint32_t Flr = 0x0B;
constexpr std::uint32_t Max = 0x0C;
constexpr std::uint32_t Min = 0x0D;
constexpr std::uint32_t Rcp = 0x0E;
constexpr std::uint32_t Rsq = 0x0F;
constexpr std::uint32_t Mova = 0x12;
constexpr std::uint32_t Mov = 0x13;
constexpr std::uint32_t Dphi = 0x18;
constexpr std::uint32_t Sgei = 0x1A;
constexpr std::uint32_t Slti = 0x1B;
constexpr std::uint32_t Break = 0x20;
constexpr std::uint32_t Nop = 0x21;
constexpr std::uint32_t End = 0x22;
constexpr std::uint32_t Breakc = 0x23;
constexpr std::uint32_t Call = 0x24;
constexpr std::uint32_t Callc = 0x25;
constexpr std::uint32_t Callu = 0x26;
constexpr std::uint32_t Ifu = 0x27;
constexpr std::uint32_t Ifc = 0x28;
constexpr std::uint32_t Loop = 0x29;
constexpr std::uint32_t Emit = 0x2A;
constexpr std::uint32_t SetEmit = 0x2B;
constexpr std::uint32_t Jmpc = 0x2C;
constexpr std::uint32_t Jmpu = 0x2D;
constexpr std::uint32_t CmpPair = 0x2E >> 1;
constexpr std::uint32_t MadiGroup = 0x30 >> 3;
constexpr std::uint32_t MadGroup = 0x38 >> 3;
}  // namespace opcode

constexpr std::uint32_t Field(std::uint32_t word, unsigned low, unsigned width) {
    return (word >> low) & ((1U << width) - 1);
}

constexpr std::uint32_t OpcodeOf(std::uint32_t word) {
    return Field(word, 26, 6);
}

constexpr bool IsMultiplyAdd(std::uint32_t word) {
    return Field(word, 29, 3) == opcode::MadGroup || Field(word, 29, 3) == opcode::MadiGroup;
}

constexpr bool IsCompare(std::uint32_t word) {
    return Field(word, 27, 5) == opcode::CmpPair;
}

// A source register, and the address register that offsets it: 0 for none,
// then a0.x, a0.y and aL.
struct Source {
    std::uint32_t index = 0;
    std::uint32_t relative = 0;
};

// The operands of the formats that compute: which descriptor applies, the
// register written, and up to three sources. Only one source of each format
// is seven bits wide and can be offset.
struct Operands {
    std::uint32_t descriptor = 0;
    std::uint32_t destination = 0;
    std::array<Source, 3> sources{};
};

// Format 1, or 1i for the inverted opcodes, whose second source is the
// seven-bit one. CMP is format 1c, laid out as format 1 with no destination.
constexpr Operands ArithmeticOperands(std::uint32_t word) {
    const std::uint32_t code = OpcodeOf(word);
    const bool inverted = code == opcode::Dphi || code == opcode::Sgei || code == opcode::Slti;
    const std::uint32_t relative = Field(word, 19, 2);
    Operands operands{.descriptor = Field(word, 0, 7), .destination = Field(word, 21, 5)};
    operands.sources.at(0) = inverted ? Source{.index = Field(word, 14, 5)}
                                      : Source{.index = Field(word, 12, 7), .relative = relative};
    operands.sources.at(1) = inverted ? Source{.index = Field(word, 7, 7), .relative = relative}
                                      : Source{.index = Field(word, 7, 5)};
    return operands;
}

// Format 5 for MAD, 5i for MADI, whose third source is the seven-bit one.
constexpr Operands MultiplyAddOperands(std::uint32_t word) {
    const bool inverted = Field(word, 29, 3) == opcode::MadiGroup;
    const std::uint32_t relative = Field(word, 22, 2);
    Operands operands{.descriptor = Field(word, 0, 5), .destination = Field(word, 24, 5)};
    operands.sources.at(0) = Source{.index = Field(word, 17, 5)};
    operands.sources.at(1) = inverted ? Source{.index = Field(word, 12, 5)}
                                      : Source{.index = Field(word, 10, 7), .relative = relative};
    operands.sources.at(2) = inverted ? Source{.index = Field(word, 5, 7), .relative = relative}
                                      : Source{.index = Field(word, 5, 5)};
    return operands;
}

// Format 1c's operators, for x in bits 24 to 26 and for y in 21 to 23.
constexpr std::array<std::uint32_t, 2> CompareOperators(std::uint32_t word) {
    return {Field(word, 24, 3), Field(word, 21, 3)};
}

// Format 2 and 3's code range: the instructions from `target` on, `count` of
// them.
struct Range {
    std::uint32_t target = 0;
    std::uint32_t count = 0;
};

constexpr Range RangeOf(std::uint32_t word) {
    return {.target = Field(word, 10, 12), .count = Field(word, 0, 8)};
}

// Format 2's condition: each flag compared with its reference bit, the two
// results combined by the operator: 0 either, 1 both, 2 x alone, 3 y alone.
struct Condition {
    bool x_reference = false;
    bool y_reference = false;
    std::uint32_t combine = 0;
};

constexpr Condition ConditionOf(std::uint32_t word) {
    return {.x_reference = Field(word, 25, 1) != 0,
            .y_reference = Field(word, 24, 1) != 0,
            .combine = Field(word, 22, 2)};
}

// Format 3's bool uniform, and LOOP's integer uniform.
constexpr std::uint32_t BoolUniformOf(std::uint32_t word) {
    return Field(word, 22, 4);
}

constexpr std::uint32_t IntUniformOf(std::uint32_t word) {
    return Field(word, 22, 2);
}

// Format 4, SETEMIT: the vertex, whether it completes a primitive, and
// whether that primitive's winding is reversed.
struct EmitSetup {
    std::uint32_t vertex = 0;
    bool primitive = false;
    bool reversed = false;
};

constexpr EmitSetup EmitSetupOf(std::uint32_t word) {
    return {.vertex = Field(word, 24, 2),
            .primitive = Field(word, 23, 1) != 0,
            .reversed = Field(word, 22, 1) != 0};
}

// An operand descriptor: the destination mask in bits 0 to 3 with x highest,
// then for each source a negation bit and four two-bit selectors, component 0
// highest.
constexpr bool DescriptorWrites(std::uint32_t descriptor, std::size_t component) {
    return Field(descriptor, static_cast<unsigned>(3 - component), 1) != 0;
}

constexpr bool DescriptorNegates(std::uint32_t descriptor, std::size_t source) {
    constexpr std::array<unsigned, 3> NegateBits = {4, 13, 22};
    return Field(descriptor, NegateBits.at(source), 1) != 0;
}

// The component of the source register that `component` of the operand reads.
constexpr std::uint32_t
DescriptorSelector(std::uint32_t descriptor, std::size_t source, std::size_t component) {
    constexpr std::array<unsigned, 3> FirstSelector = {11, 20, 29};
    return Field(descriptor, FirstSelector.at(source) - (2 * static_cast<unsigned>(component)), 2);
}

}  // namespace hexlith::domain::gpu::shader

#endif  // HEXLITH_DOMAIN_GPU_SHADER_ENCODING_H
