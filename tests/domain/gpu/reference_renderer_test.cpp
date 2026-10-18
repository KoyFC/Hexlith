#include "domain/gpu/reference_renderer.h"

#include <bit>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::gpu {
namespace {

constexpr std::uint32_t Side = 16;
constexpr std::uint32_t ColorAddress = GpuMemory::VramBase;
constexpr std::uint32_t VertexAddress = GpuMemory::VramBase + 0x10000;
constexpr std::uint32_t Eight24 = 0x420000;

// MOV, GBATEK's opcode 13h in format 1, and END, 22h.
constexpr std::uint32_t Mov(std::uint32_t destination, std::uint32_t source) {
    return (0x13U << 26) | (destination << 21) | (source << 12);
}

constexpr std::uint32_t End = 0x22U << 26;
// The operand descriptor that writes xyzw unswizzled.
constexpr std::uint32_t Plain = 0xF | (0x1BU << 5) | (0x1BU << 14) | (0x1BU << 23);

// A vertex shader that copies its two inputs to its two outputs, position
// and colour, and an array of vertices of two float4 attributes each.
struct Fixture {
    Fixture() {
        const std::uint16_t vs = reg::VertexShaderBase;
        Set(vs + reg::shader::CodeTransferIndex, 0);
        for (const std::uint32_t word : {Mov(0x0, 0x0), Mov(0x1, 0x1), End}) {
            Set(vs + reg::shader::CodeTransferDataFirst, word);
        }
        Set(vs + reg::shader::DescriptorIndex, 0);
        Set(vs + reg::shader::DescriptorDataFirst, Plain);
        Set(vs + reg::shader::InputBufferConfig, 1);
        Set(vs + reg::shader::AttributePermutationLow, 0x10);
        Set(vs + reg::shader::OutputMask, 0x3);
        Set(0x04F, 2);
        Set(0x050, 0x03020100);
        Set(0x051, 0x0B0A0908);

        // Two float4 attributes in one array, 32 bytes a vertex.
        Set(reg::AttributeBufferBase, VertexAddress >> 3);
        Set(reg::AttributeFormatLow, 0xFF);
        Set(reg::AttributeFormatHigh, 1U << 28);
        Set(reg::AttributeBufferFirst + 1, 0x10);
        Set(reg::AttributeBufferFirst + 2, (32U << 16) | (2U << 28));

        Set(0x041, Eight24);
        Set(0x043, Eight24);
        Set(0x107, 0xF00);
        Set(0x102, 0x3);
        Set(0x113, 0xF);
        Set(0x117, 0x0);
        Set(0x11D, ColorAddress >> 3);
        Set(0x11E, Side | ((Side - 1) << 12));
        Set(0x1C6, 1);
    }

    void Set(std::uint16_t id, std::uint32_t value) {
        state.Write(id, value, 0xF);
    }

    void AddVertex(float x, float y, Vec4 color) {
        for (const float value : {x, y, -0.5F, 1.0F, color[0], color[1], color[2], color[3]}) {
            const auto bits = std::bit_cast<std::uint32_t>(value);
            for (unsigned shift = 0; shift < 32; shift += 8) {
                vertices.push_back(static_cast<std::uint8_t>(bits >> shift));
            }
        }
    }

    void Draw() {
        ASSERT_TRUE(memory.Write(VertexAddress, vertices));
        Set(reg::VertexCount, static_cast<std::uint32_t>(vertices.size() / 32));
        renderer.Draw(state, false);
    }

    [[nodiscard]] Color Pixel(std::uint32_t x, std::uint32_t y) const {
        const std::size_t offset = TiledOffset(x, Side - 1 - y, Side, 4);
        return DecodePixel(PixelFormat::Rgba8,
                           memory.Bytes(ColorAddress + static_cast<std::uint32_t>(offset), 4));
    }

    PicaState state;
    GpuMemory memory;
    ReferenceRenderer renderer{memory, Sampling::AzaharSoftware};
    std::vector<std::uint8_t> vertices;
};

constexpr Vec4 Green{0, 1, 0, 1};
constexpr Color GreenPixel{.red = 0, .green = 255, .blue = 0, .alpha = 255};

TEST(ReferenceRenderer, ShadesLoadsAndDrawsAnArrayOfVertices) {
    Fixture fixture;
    fixture.AddVertex(-1, -1, Green);
    fixture.AddVertex(1, -1, Green);
    fixture.AddVertex(-1, 1, Green);
    fixture.Draw();
    EXPECT_EQ(fixture.Pixel(1, 1), GreenPixel);
    EXPECT_EQ(fixture.Pixel(14, 14), Color{}) << "outside the triangle";
    EXPECT_EQ(fixture.renderer.Statistics().vertex_shader_runs, 3U);
    EXPECT_FALSE(fixture.renderer.Statistics().unsupported_shader);
}

TEST(ReferenceRenderer, ReusesAShadedVertexInAnIndexedDraw) {
    Fixture fixture;
    fixture.AddVertex(-1, -1, Green);
    fixture.AddVertex(1, -1, Green);
    fixture.AddVertex(-1, 1, Green);
    fixture.AddVertex(1, 1, Green);
    // Byte indices after the vertices, from the attribute base.
    const std::vector<std::uint8_t> indices = {0, 1, 2, 2, 1, 3};
    constexpr std::uint32_t IndexOffset = 0x100;
    ASSERT_TRUE(fixture.memory.Write(VertexAddress + IndexOffset, indices));
    fixture.Set(reg::IndexList, IndexOffset);
    ASSERT_TRUE(fixture.memory.Write(VertexAddress, fixture.vertices));
    fixture.Set(reg::VertexCount, 6);
    fixture.renderer.Draw(fixture.state, true);
    EXPECT_EQ(fixture.Pixel(1, 1), GreenPixel);
    EXPECT_EQ(fixture.Pixel(14, 14), GreenPixel);
    EXPECT_EQ(fixture.renderer.Statistics().vertex_shader_runs, 4U) << "six indices, four vertices";
}

TEST(ReferenceRenderer, StartsAStripAgainWhenTold) {
    Fixture fixture;
    fixture.Set(reg::PrimitiveConfig, 1U << 8);
    fixture.renderer.RestartPrimitive(fixture.state);
    fixture.AddVertex(-1, -1, Green);
    fixture.AddVertex(1, -1, Green);
    fixture.AddVertex(-1, 1, Green);
    fixture.AddVertex(1, 1, Green);
    fixture.Draw();
    EXPECT_EQ(fixture.Pixel(14, 14), GreenPixel) << "the strip's second triangle";
    EXPECT_EQ(fixture.renderer.Rasterization().triangles, 2U);
}

}  // namespace
}  // namespace hexlith::domain::gpu
