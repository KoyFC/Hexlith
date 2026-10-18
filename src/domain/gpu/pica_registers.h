#ifndef HEXLITH_DOMAIN_GPU_PICA_REGISTERS_H
#define HEXLITH_DOMAIN_GPU_PICA_REGISTERS_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace hexlith::domain::gpu {

// The PICA200's internal registers, by the IDs a command list names them
// with. Names and IDs are GBATEK's, 3DS GPU Internal Register List Summary;
// bit positions are cited where a field is decoded. docs/formats/
// pica-registers.md lists the ones Hexlith gives a meaning to.
constexpr std::size_t RegisterCount = 0x400;
using RegisterFile = std::array<std::uint32_t, RegisterCount>;

namespace reg {

// Finalize Interrupt registers.
constexpr std::uint16_t IrqRequestFirst = 0x010;
constexpr std::uint16_t IrqRequestLast = 0x01F;

// Geometry Pipeline registers.
constexpr std::uint16_t AttributeBufferBase = 0x200;
constexpr std::uint16_t AttributeFormatLow = 0x201;
constexpr std::uint16_t AttributeFormatHigh = 0x202;
constexpr std::uint16_t AttributeBufferFirst = 0x203;
constexpr std::uint16_t IndexList = 0x227;
constexpr std::uint16_t VertexCount = 0x228;
constexpr std::uint16_t GeometryStageConfig = 0x229;
constexpr std::uint16_t FirstIndex = 0x22A;
constexpr std::uint16_t DrawArrays = 0x22E;
constexpr std::uint16_t DrawElements = 0x22F;
constexpr std::uint16_t FixedAttributeIndex = 0x232;
constexpr std::uint16_t FixedAttributeDataFirst = 0x233;
constexpr std::uint16_t FixedAttributeDataLast = 0x235;
constexpr std::uint16_t CommandBufferSize0 = 0x238;
constexpr std::uint16_t CommandBufferAddress0 = 0x23A;
constexpr std::uint16_t CommandBufferJump0 = 0x23C;
constexpr std::uint16_t CommandBufferJump1 = 0x23D;
constexpr std::uint16_t VertexShaderAttributeCount = 0x242;
constexpr std::uint16_t VertexShaderCommonMode = 0x244;
constexpr std::uint16_t PrimitiveConfig = 0x25E;
constexpr std::uint16_t RestartPrimitive = 0x25F;

// Fragment Lighting registers: the index and table of the next LUT word, and
// eight mirrored data ports.
constexpr std::uint16_t LightingLutIndex = 0x1C5;
constexpr std::uint16_t LightingLutDataFirst = 0x1C8;
constexpr std::uint16_t LightingLutDataLast = 0x1CF;

// Texturing registers (Environment): the fog LUT's index and data ports.
constexpr std::uint16_t FogLutIndex = 0x0E6;
constexpr std::uint16_t FogLutDataFirst = 0x0E8;
constexpr std::uint16_t FogLutDataLast = 0x0EF;

// Texturing registers (Procedural Texture): its LUTs' index and data ports.
constexpr std::uint16_t ProcTexLutIndex = 0x0AF;
constexpr std::uint16_t ProcTexLutDataFirst = 0x0B0;
constexpr std::uint16_t ProcTexLutDataLast = 0x0B7;

// Shader registers. Each unit's block has the same layout from its base:
// the geometry shader's at 0280h, the vertex shader's at 02B0h.
constexpr std::uint16_t GeometryShaderBase = 0x280;
constexpr std::uint16_t VertexShaderBase = 0x2B0;

namespace shader {
constexpr std::uint16_t BoolUniform = 0x00;
constexpr std::uint16_t IntUniformFirst = 0x01;
constexpr std::uint16_t IntUniformLast = 0x04;
constexpr std::uint16_t InputBufferConfig = 0x09;
constexpr std::uint16_t EntryPoint = 0x0A;
constexpr std::uint16_t AttributePermutationLow = 0x0B;
constexpr std::uint16_t AttributePermutationHigh = 0x0C;
constexpr std::uint16_t OutputMask = 0x0D;
constexpr std::uint16_t CodeTransferEnd = 0x0F;
constexpr std::uint16_t FloatUniformIndex = 0x10;
constexpr std::uint16_t FloatUniformDataFirst = 0x11;
constexpr std::uint16_t FloatUniformDataLast = 0x18;
constexpr std::uint16_t CodeTransferIndex = 0x1B;
constexpr std::uint16_t CodeTransferDataFirst = 0x1C;
constexpr std::uint16_t CodeTransferDataLast = 0x23;
constexpr std::uint16_t DescriptorIndex = 0x25;
constexpr std::uint16_t DescriptorDataFirst = 0x26;
constexpr std::uint16_t DescriptorDataLast = 0x2D;
// The size of a unit's block.
constexpr std::uint16_t BlockSize = 0x30;
}  // namespace shader

}  // namespace reg

// A command header's byte mask, one bit for each byte of the value, spread
// into a mask of the bits to write.
constexpr std::uint32_t ExpandByteMask(std::uint32_t byte_mask) {
    std::uint32_t mask = 0;
    for (unsigned byte = 0; byte < 4; ++byte) {
        if ((byte_mask & (1U << byte)) != 0) {
            mask |= 0xFFU << (8 * byte);
        }
    }
    return mask;
}

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_PICA_REGISTERS_H
