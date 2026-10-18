#include "domain/gpu/fragment_stage.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::gpu {
namespace {

// Texture combiner registers, GBATEK, Texturing registers (Environment).
constexpr std::array<std::uint16_t, 6> Stages = {0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8};
constexpr std::uint16_t BufferUpdate = 0x0E0;
constexpr std::uint16_t BufferColor = 0x0FD;

// Sources by number: 0 primary colour, 3 texture 0, Dh the buffer, Eh the
// constant, Fh the previous stage. Operations: 0 replace, 1 modulate.
constexpr std::uint32_t Primary = 0x0;
constexpr std::uint32_t Texture0 = 0x3;
constexpr std::uint32_t Buffer = 0xD;
constexpr std::uint32_t Constant = 0xE;
constexpr std::uint32_t Previous = 0xF;

std::uint32_t Sources(std::uint32_t first, std::uint32_t second = Primary) {
    return first | (second << 4) | (first << 16) | (second << 20);
}

std::uint32_t Operation(std::uint32_t operation) {
    return operation | (operation << 16);
}

// Every stage passes the one before it on.
RegisterFile PassThrough() {
    RegisterFile registers{};
    for (const std::uint16_t stage : Stages) {
        registers.at(stage) = Sources(Previous);
    }
    return registers;
}

constexpr Color Red{.red = 255, .green = 0, .blue = 0, .alpha = 255};
constexpr Color Grey{.red = 128, .green = 128, .blue = 128, .alpha = 64};

TEST(CombineTextures, ReplacesWithATexture) {
    RegisterFile registers = PassThrough();
    registers.at(Stages[0]) = Sources(Texture0);
    const std::array<Color, 4> textures = {Red, {}, {}, {}};
    EXPECT_EQ(CombineTextures(registers, textures, Grey, {}), Red);
}

TEST(CombineTextures, ModulatesOverTwoHundredAndFiftyFive) {
    RegisterFile registers = PassThrough();
    registers.at(Stages[0]) = Sources(Primary, Constant);
    registers.at(Stages[0] + 2) = Operation(1);
    registers.at(Stages[0] + 3) = 0x80FF40FF;
    const Color result = CombineTextures(registers, {}, Grey, {});
    EXPECT_EQ(result,
              (Color{.red = 128, .green = 64 * 128 / 255, .blue = 128, .alpha = 128 * 64 / 255}));
}

TEST(CombineTextures, ReadsTheBufferOneStageLate) {
    RegisterFile registers = PassThrough();
    // Stage 0 replaces with the primary colour and writes the buffer; stage 1
    // still reads the buffer's initial colour, stage 2 what stage 0 wrote.
    registers.at(Stages[0]) = Sources(Primary);
    registers.at(BufferUpdate) = (1U << 8) | (1U << 12);
    registers.at(BufferColor) = 0x01020304;
    registers.at(Stages[1]) = Sources(Buffer);
    EXPECT_EQ(CombineTextures(registers, {}, Grey, {}),
              (Color{.red = 4, .green = 3, .blue = 2, .alpha = 1}));
    registers.at(Stages[1]) = Sources(Previous);
    registers.at(Stages[2]) = Sources(Buffer);
    EXPECT_EQ(CombineTextures(registers, {}, Grey, {}), Grey);
}

TEST(CombineTextures, ScalesAndSaturates) {
    RegisterFile registers = PassThrough();
    registers.at(Stages[0]) = Sources(Primary);
    registers.at(Stages[0] + 4) = 1U | (2U << 16);
    EXPECT_EQ(CombineTextures(registers, {}, Grey, {}),
              (Color{.red = 255, .green = 255, .blue = 255, .alpha = 255}));
}

TEST(CombineTextures, TakesTheThirdSourceWhereTheFirstStageNamesThePrevious) {
    RegisterFile registers = PassThrough();
    registers.at(Stages[0]) = Previous | (Constant << 8) | (Constant << 16);
    registers.at(Stages[0] + 3) = 0xFF0000FF;
    EXPECT_EQ(CombineTextures(registers, {}, Grey, {}), Red);
}

constexpr std::uint16_t ColorOperation = 0x100;
constexpr std::uint16_t BlendFunction = 0x101;
constexpr std::uint16_t LogicOperation = 0x102;
constexpr std::uint16_t DepthColorMask = 0x107;

TEST(MergeColor, BlendsBySourceAlpha) {
    RegisterFile registers{};
    registers.at(ColorOperation) = 1U << 8;
    // Add, source alpha and one minus source alpha for colour and alpha.
    registers.at(BlendFunction) = (6U << 16) | (7U << 20) | (6U << 24) | (7U << 28);
    registers.at(DepthColorMask) = 0xF00;
    const Color source{.red = 255, .green = 0, .blue = 0, .alpha = 51};
    const Color destination{.red = 0, .green = 255, .blue = 0, .alpha = 255};
    EXPECT_EQ(
        MergeColor(registers, source, destination),
        (Color{.red = 51, .green = 204, .blue = 0, .alpha = ((51 * 51) + (255 * 204)) / 255}));
}

TEST(MergeColor, AppliesALogicOperationAndTheWriteMask) {
    RegisterFile registers{};
    registers.at(LogicOperation) = 0xB;  // XOR
    registers.at(DepthColorMask) = 0x700;
    const Color source{.red = 0x0F, .green = 0xF0, .blue = 0xFF, .alpha = 0xFF};
    const Color destination{.red = 0xFF, .green = 0xFF, .blue = 0xFF, .alpha = 0x12};
    EXPECT_EQ(MergeColor(registers, source, destination),
              (Color{.red = 0xF0, .green = 0x0F, .blue = 0x00, .alpha = 0x12}))
        << "alpha masked off";
}

TEST(StencilAction, SaturatesOrWraps) {
    EXPECT_EQ(StencilAction(3, 255, 0), 255);
    EXPECT_EQ(StencilAction(4, 0, 0), 0);
    EXPECT_EQ(StencilAction(6, 255, 0), 0);
    EXPECT_EQ(StencilAction(7, 0, 0), 255);
    EXPECT_EQ(StencilAction(2, 9, 42), 42);
    EXPECT_EQ(StencilAction(5, 0x0F, 0), 0xF0);
}

TEST(PassesAlphaTest, ComparesWithTheReference) {
    RegisterFile registers{};
    constexpr std::uint16_t AlphaTest = 0x104;
    registers.at(AlphaTest) = 1U | (6U << 4) | (100U << 8);  // greater than 100
    EXPECT_FALSE(PassesAlphaTest(registers, 100));
    EXPECT_TRUE(PassesAlphaTest(registers, 101));
    registers.at(AlphaTest) = 0;
    EXPECT_TRUE(PassesAlphaTest(registers, 0)) << "disabled";
}

// An 8x8 RGBA8 texture whose texel at (x, y), counting stored rows, has red
// x and green y. Texture coordinates count rows from the bottom.
struct TextureFixture {
    static constexpr std::uint32_t Address = GpuMemory::VramBase;

    TextureFixture() {
        std::vector<std::uint8_t> bytes(TextureBytes(TextureFormat::Rgba8, 8, 8));
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                const std::size_t at = TiledOffset(x, y, 8, 4);
                // Memory order is alpha, blue, green, red.
                bytes[at] = 255;
                bytes[at + 2] = static_cast<std::uint8_t>(y);
                bytes[at + 3] = static_cast<std::uint8_t>(x);
            }
        }
        EXPECT_TRUE(memory.Write(Address, bytes));
        registers.at(0x080) = 1;
        registers.at(0x082) = (8U << 16) | 8U;
        registers.at(0x085) = Address >> 3;
    }

    [[nodiscard]] Color Sample(float u, float v) const {
        std::array<TextureCoordinate, 3> coordinates{};
        coordinates[0].u = u;
        coordinates[0].v = v;
        bool unsupported = false;
        const Color color = SampleTextures(
            registers, memory, coordinates, 1.0F, Sampling::AzaharSoftware, unsupported)[0];
        EXPECT_FALSE(unsupported);
        return color;
    }

    GpuMemory memory;
    RegisterFile registers{};
};

TEST(SampleTextures, TakesTheNearestTexelCountingRowsFromTheBottom) {
    const TextureFixture fixture;
    EXPECT_EQ(fixture.Sample(0.0F, 0.0F), (Color{.red = 0, .green = 7, .blue = 0, .alpha = 255}));
    EXPECT_EQ(fixture.Sample(0.99F, 0.99F), (Color{.red = 7, .green = 0, .blue = 0, .alpha = 255}));
    EXPECT_EQ(fixture.Sample(0.3F, 0.5F), (Color{.red = 2, .green = 3, .blue = 0, .alpha = 255}));
}

TEST(SampleTextures, ClampsRepeatsMirrorsOrGivesTheBorder) {
    TextureFixture fixture;
    constexpr std::uint16_t Parameters = 0x083;
    constexpr std::uint16_t BorderColor = 0x081;
    // Wrap s in bits 12 to 14: 0 clamp to the edge.
    EXPECT_EQ(fixture.Sample(1.5F, 0.5F).red, 7);
    fixture.registers.at(Parameters) = 2U << 12;  // repeat
    EXPECT_EQ(fixture.Sample(1.25F, 0.5F).red, 2);
    fixture.registers.at(Parameters) = 3U << 12;  // mirrored repeat
    EXPECT_EQ(fixture.Sample(1.25F, 0.5F).red, 5);
    fixture.registers.at(Parameters) = 1U << 12;  // the border
    fixture.registers.at(BorderColor) = 0x11223344;
    EXPECT_EQ(fixture.Sample(1.25F, 0.5F),
              (Color{.red = 0x44, .green = 0x33, .blue = 0x22, .alpha = 0x11}));
}

TEST(SampleTextures, GivesOpaqueBlackForATextureAtAddressZero) {
    TextureFixture fixture;
    fixture.registers.at(0x085) = 0;
    EXPECT_EQ(fixture.Sample(0.5F, 0.5F), (Color{.red = 0, .green = 0, .blue = 0, .alpha = 255}));
}

TEST(SampleTextures, FiltersBetweenTexelsInHardwareMode) {
    TextureFixture fixture;
    fixture.registers.at(0x083) = 1U << 1;  // linear magnification
    std::array<TextureCoordinate, 3> coordinates{};
    // Halfway between the centres of texels 2 and 3 of the row.
    coordinates[0].u = 3.0F / 8.0F;
    coordinates[0].v = 4.5F / 8.0F;
    coordinates[0].du_dx = 0.5F / 8.0F;
    coordinates[0].dv_dy = 0.5F / 8.0F;
    bool unsupported = false;
    const Color color = SampleTextures(
        fixture.registers, fixture.memory, coordinates, 1.0F, Sampling::Hardware, unsupported)[0];
    EXPECT_EQ(color.red, 3) << "2.5 rounds to the nearest";
    EXPECT_EQ(color.green, 3);
}

}  // namespace
}  // namespace hexlith::domain::gpu
