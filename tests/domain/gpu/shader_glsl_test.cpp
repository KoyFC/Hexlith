#include "domain/gpu/shader_glsl.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>

namespace hexlith::domain::gpu {
namespace {

// GBATEK's encodings: END 22h, NOP 21h, EMIT 2Ah, JMPU 2Dh in format 3
// (target in bits 10 to 21), MOV 13h in format 1.
constexpr std::uint32_t End = 0x22U << 26;
constexpr std::uint32_t Nop = 0x21U << 26;
constexpr std::uint32_t Emit = 0x2AU << 26;

constexpr std::uint32_t Mov(std::uint32_t destination, std::uint32_t source) {
    return (0x13U << 26) | (destination << 21) | (source << 12);
}

constexpr std::uint32_t JumpUnless(std::uint32_t target) {
    return (0x2DU << 26) | (target << 10) | 1U;
}

std::unique_ptr<ShaderUnitState> Unit(std::initializer_list<std::uint32_t> program,
                                      std::uint32_t at = 0) {
    auto unit = std::make_unique<ShaderUnitState>();
    for (const std::uint32_t word : program) {
        unit->code.at(at++) = word;
    }
    // Write every component, every source unswizzled.
    unit->descriptors.at(0) = 0xF | (0x1BU << 5) | (0x1BU << 14) | (0x1BU << 23);
    return unit;
}

bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

TEST(TranslateShader, GivesACaseToEachInstructionUpToEnd) {
    const auto unit = Unit({Mov(1, 0), End, Mov(2, 0)});
    const GlslShader shader = TranslateShader(*unit, 0);
    EXPECT_TRUE(Has(shader.source, "case 0u:"));
    EXPECT_TRUE(Has(shader.source, "pica_o[1].x = result.x;"));
    EXPECT_TRUE(Has(shader.source, "case 1u:"));
    EXPECT_FALSE(Has(shader.source, "case 2u:")) << "nothing reaches past END";
    EXPECT_FALSE(shader.unsupported);
}

TEST(TranslateShader, StartsAtTheEntryPoint) {
    const auto unit = Unit({Mov(1, 0), End}, 40);
    const GlslShader shader = TranslateShader(*unit, 40);
    EXPECT_TRUE(Has(shader.source, "uint counter = 40u;"));
    EXPECT_FALSE(Has(shader.source, "case 0u:"));
}

TEST(TranslateShader, ReachesAJumpsTargetAndLandsOneShort) {
    const auto unit = Unit({JumpUnless(3), End, Nop, End});
    const GlslShader shader = TranslateShader(*unit, 0);
    EXPECT_TRUE(Has(shader.source, "case 3u:"));
    EXPECT_TRUE(Has(shader.source, "counter = 3u - 1u;"));
}

TEST(TranslateShader, SaysWhenAVertexShaderWouldEmit) {
    const auto unit = Unit({Emit, End});
    EXPECT_TRUE(TranslateShader(*unit, 0).unsupported);
}

}  // namespace
}  // namespace hexlith::domain::gpu
