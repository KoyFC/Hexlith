#include "domain/gpu/vertex_stage.h"

#include <bit>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::gpu {
namespace {

constexpr std::uint32_t Base = GpuMemory::VramBase;

void WriteFloat(GpuMemory& memory, std::uint32_t address, float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const std::vector<std::uint8_t> bytes = {static_cast<std::uint8_t>(bits),
                                             static_cast<std::uint8_t>(bits >> 8),
                                             static_cast<std::uint8_t>(bits >> 16),
                                             static_cast<std::uint8_t>(bits >> 24)};
    ASSERT_TRUE(memory.Write(address, bytes));
}

// One array: attribute 0 as three floats, four bytes of padding, attribute 1
// as two unsigned bytes. GBATEK's attribute format nibble is the type in
// bits 0 and 1 (3 float, 1 unsigned byte) and the count less one above.
RegisterFile TwoAttributeLayout(std::uint32_t stride) {
    RegisterFile registers{};
    registers.at(reg::AttributeBufferBase) = Base >> 3;
    registers.at(reg::AttributeFormatLow) = (3U | (2U << 2)) | ((1U | (1U << 2)) << 4);
    registers.at(reg::AttributeFormatHigh) = 1U << 28;
    registers.at(reg::AttributeBufferFirst) = 0;
    // Components: attribute 0, padding of four bytes (Ch), attribute 1.
    registers.at(reg::AttributeBufferFirst + 1) = 0x0U | (0xCU << 4) | (0x1U << 8);
    registers.at(reg::AttributeBufferFirst + 2) = (stride << 16) | (3U << 28);
    return registers;
}

TEST(VertexLoader, ReadsAttributesPastPaddingAtTheirStride) {
    GpuMemory memory;
    constexpr std::uint32_t Stride = 20;
    const RegisterFile registers = TwoAttributeLayout(Stride);
    WriteFloat(memory, Base + Stride + 0, 1.5F);
    WriteFloat(memory, Base + Stride + 4, -2.0F);
    WriteFloat(memory, Base + Stride + 8, 3.0F);
    ASSERT_TRUE(memory.Write(Base + Stride + 16, std::vector<std::uint8_t>{200, 7}));

    const VertexLoader loader(registers);
    const VertexLoader::Inputs inputs = loader.Load(memory, 1, {});
    EXPECT_EQ(inputs[0], (Vec4{1.5F, -2.0F, 3.0F, 1.0F})) << "w defaults to one";
    EXPECT_EQ(inputs[1], (Vec4{200.0F, 7.0F, 0.0F, 1.0F})) << "bytes are not normalised";
}

TEST(VertexLoader, TakesAFixedAttributeInsteadOfTheArray) {
    const GpuMemory memory;
    RegisterFile registers = TwoAttributeLayout(20);
    registers.at(reg::AttributeFormatHigh) |= 1U << (49 - 32);
    std::array<Vec4, PicaState::FixedAttributes> fixed{};
    fixed[1] = {9, 8, 7, 6};
    const VertexLoader loader(registers);
    EXPECT_EQ(loader.Load(memory, 0, fixed)[1], (Vec4{9, 8, 7, 6}));
}

TEST(PlaceInputs, PutsEachAttributeInTheRegisterThePermutationNames) {
    RegisterFile registers{};
    const std::uint16_t base = reg::VertexShaderBase;
    registers.at(base + reg::shader::InputBufferConfig) = 1;
    registers.at(base + reg::shader::AttributePermutationLow) = 0x05 | (0x02 << 4);
    VertexLoader::Inputs attributes{};
    attributes[0] = {1, 1, 1, 1};
    attributes[1] = {2, 2, 2, 2};
    ShaderRegisters shader;
    PlaceInputs(registers, base, attributes, shader);
    EXPECT_EQ(shader.inputs[5], (Vec4{1, 1, 1, 1}));
    EXPECT_EQ(shader.inputs[2], (Vec4{2, 2, 2, 2}));
}

TEST(PackOutputs, PacksTheEnabledOutputsInOrder) {
    ShaderRegisters::Bank outputs{};
    outputs[1] = {1, 1, 1, 1};
    outputs[4] = {4, 4, 4, 4};
    const ShaderRegisters::Bank packed = PackOutputs(0b10010, outputs);
    EXPECT_EQ(packed[0], (Vec4{1, 1, 1, 1}));
    EXPECT_EQ(packed[1], (Vec4{4, 4, 4, 4}));
}

TEST(MakeOutputVertex, MapsSemanticsAndKeepsTheColourInRange) {
    RegisterFile registers{};
    constexpr std::uint16_t OutputMapTotal = 0x04F;
    constexpr std::uint16_t FirstOutputMap = 0x050;
    registers.at(OutputMapTotal) = 2;
    registers.at(FirstOutputMap) = 0x03020100;
    registers.at(FirstOutputMap + 1) = 0x0B0A0908;
    ShaderRegisters::Bank packed{};
    packed[0] = {0.5F, -0.5F, 0.25F, 1.0F};
    packed[1] = {-0.5F, 2.0F, 0.25F, 1.0F};
    const OutputVertex vertex = MakeOutputVertex(registers, packed);
    EXPECT_EQ(vertex.values[OutputVertex::Position + 1], -0.5F);
    EXPECT_EQ(vertex.values[OutputVertex::Color], 0.5F) << "made positive";
    EXPECT_EQ(vertex.values[OutputVertex::Color + 1], 1.0F) << "kept to one";
    EXPECT_EQ(vertex.values[OutputVertex::Texcoord0], 1.0F) << "a semantic nothing gives";
}

OutputVertex Numbered(float number) {
    OutputVertex vertex;
    vertex.values[0] = number;
    return vertex;
}

std::vector<std::array<float, 3>> Assemble(PrimitiveAssembler& assembler, int count) {
    std::vector<std::array<float, 3>> triangles;
    for (int at = 0; at < count; ++at) {
        if (const auto triangle = assembler.Submit(Numbered(static_cast<float>(at)))) {
            triangles.push_back(
                {(*triangle)[0].values[0], (*triangle)[1].values[0], (*triangle)[2].values[0]});
        }
    }
    return triangles;
}

TEST(PrimitiveAssembler, AlternatesTheVertexAStripReplaces) {
    PrimitiveAssembler assembler;
    assembler.Restart(PrimitiveAssembler::Topology::Strip);
    const std::vector<std::array<float, 3>> expected = {{0, 1, 2}, {2, 1, 3}, {2, 3, 4}};
    EXPECT_EQ(Assemble(assembler, 5), expected);
}

TEST(PrimitiveAssembler, KeepsTheFirstVertexOfAFan) {
    PrimitiveAssembler assembler;
    assembler.Restart(PrimitiveAssembler::Topology::Fan);
    const std::vector<std::array<float, 3>> expected = {{0, 1, 2}, {0, 2, 3}};
    EXPECT_EQ(Assemble(assembler, 4), expected);
}

TEST(PrimitiveAssembler, ReversesAGeometryShaderTriangleOnce) {
    PrimitiveAssembler assembler;
    assembler.Restart(PrimitiveAssembler::Topology::Shader);
    assembler.ReverseNextWinding();
    const std::vector<std::array<float, 3>> expected = {{1, 0, 2}, {3, 4, 5}};
    EXPECT_EQ(Assemble(assembler, 6), expected);
}

}  // namespace
}  // namespace hexlith::domain::gpu
