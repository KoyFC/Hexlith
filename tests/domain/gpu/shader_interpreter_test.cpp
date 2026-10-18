#include "domain/gpu/shader_arithmetic.h"
#include "domain/gpu/shader_interpreter.h"

#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <memory>

namespace hexlith::domain::gpu {
namespace {

// Instruction encodings, from GBATEK, 3DS GPU Shader Instruction Set - Blurp.
// Registers: v0 is 00h, r0 10h, c0 20h; outputs o0 00h, temporaries r0 10h.
constexpr std::uint32_t End = 0x22U << 26;
constexpr std::uint32_t Nop = 0x21U << 26;

// Format 1: opcode, destination, source 1 (seven bits), source 2 (five
// bits), operand descriptor, relative index for source 1.
constexpr std::uint32_t Format1(std::uint32_t op,
                                std::uint32_t dst,
                                std::uint32_t src1,
                                std::uint32_t src2,
                                std::uint32_t desc,
                                std::uint32_t index = 0) {
    return (op << 26) | (dst << 21) | (index << 19) | (src1 << 12) | (src2 << 7) | desc;
}

// Format 5, MAD: destination, sources 1 (five bits), 2 (seven), 3 (five).
constexpr std::uint32_t Mad(std::uint32_t dst,
                            std::uint32_t src1,
                            std::uint32_t src2,
                            std::uint32_t src3,
                            std::uint32_t desc) {
    return (0x7U << 29) | (dst << 24) | (src1 << 17) | (src2 << 10) | (src3 << 5) | desc;
}

// Formats 2 and 3: opcode, target, count, and the condition or uniform bits.
constexpr std::uint32_t
Flow(std::uint32_t op, std::uint32_t target, std::uint32_t count, std::uint32_t extra = 0) {
    return (op << 26) | extra | (target << 10) | count;
}

// An operand descriptor writing xyzw with every source unswizzled: the
// selectors 0, 1, 2, 3 (1Bh) for each, mask 0Fh.
constexpr std::uint32_t Plain = 0xF | (0x1BU << 5) | (0x1BU << 14) | (0x1BU << 23);

struct Fixture {
    Fixture() : unit(std::make_unique<ShaderUnitState>()) {
        unit->descriptors.at(0) = Plain;
    }

    void Load(std::initializer_list<std::uint32_t> program) const {
        std::size_t at = 0;
        for (const std::uint32_t word : program) {
            unit->code.at(at++) = word;
        }
    }

    ShaderRunResult Run(GeometryEmitter* emitter = nullptr) {
        return RunShader(*unit, registers, {.entry = 0, .emitter = emitter});
    }

    std::unique_ptr<ShaderUnitState> unit;
    ShaderRegisters registers;
};

constexpr float Infinity = std::numeric_limits<float>::infinity();
const float NotANumber = std::numeric_limits<float>::quiet_NaN();

TEST(ShaderArithmetic, MakesZeroOfZeroTimesInfinity) {
    EXPECT_EQ(ShaderMultiply(0.0F, Infinity), 0.0F);
    EXPECT_TRUE(std::isnan(ShaderMultiply(NotANumber, 1.0F))) << "a NaN factor stays";
}

TEST(ShaderArithmetic, PicksASideWhenANaNIsCompared) {
    EXPECT_TRUE(std::isnan(ShaderMax(0.0F, NotANumber)));
    EXPECT_EQ(ShaderMax(NotANumber, 0.0F), 0.0F);
    EXPECT_TRUE(std::isnan(ShaderMin(0.0F, NotANumber)));
    EXPECT_EQ(ShaderMin(NotANumber, 0.0F), 0.0F);
}

TEST(ShaderArithmetic, TruncatesForTheAddressRegisters) {
    EXPECT_EQ(ShaderTruncate(2.7F), 2);
    EXPECT_EQ(ShaderTruncate(-2.7F), -2);
    EXPECT_EQ(ShaderTruncate(NotANumber), std::numeric_limits<std::int32_t>::min());
    EXPECT_EQ(ShaderTruncate(1e10F), std::numeric_limits<std::int32_t>::min());
}

TEST(ShaderArithmetic, RoundsToFloat24) {
    EXPECT_EQ(RoundToFloat24(1.0F + std::ldexp(1.0F, -17)), 1.0F) << "a tie goes to even";
    EXPECT_EQ(RoundToFloat24(1.0F + (3 * std::ldexp(1.0F, -17))), 1.0F + std::ldexp(1.0F, -15));
    EXPECT_EQ(RoundToFloat24(std::ldexp(1.0F, -70)), 0.0F) << "too small is flushed";
    EXPECT_EQ(RoundToFloat24(std::ldexp(1.0F, 70)), Infinity) << "too large is infinite";
}

TEST(RunShader, AddsWithTheDescriptorsSwizzleMaskAndNegation) {
    Fixture shader;
    // Source 1 read .wzyx and negated; only x and y written.
    shader.unit->descriptors.at(1) = 0xC | (1U << 4) | (0xE4U << 5) | (0x1BU << 14);
    shader.Load({Format1(0x00, 0x10, 0x00, 0x01, 1), End});
    shader.registers.inputs.at(0) = {1, 2, 3, 4};
    shader.registers.inputs.at(1) = {10, 20, 30, 40};
    shader.registers.temporaries.at(0) = {7, 7, 7, 7};
    shader.Run();
    EXPECT_EQ(shader.registers.temporaries.at(0), (Vec4{6, 17, 7, 7}));
}

TEST(RunShader, MultipliesAndAddsWithoutFusing) {
    Fixture shader;
    shader.Load({Mad(0x00, 0x00, 0x01, 0x02, 0), End});
    shader.registers.inputs.at(0) = {2, 0, 3, 1};
    shader.registers.inputs.at(1) = {3, Infinity, 3, 1};
    shader.registers.inputs.at(2) = {1, 1, 1, 1};
    shader.Run();
    EXPECT_EQ(shader.registers.outputs[0].at(0), (Vec4{7, 1, 10, 2})) << "0 * inf + 1 is 1";
}

TEST(RunShader, ReadsOnesPastTheLastUniform) {
    Fixture shader;
    // c94 relative to a0.x, which holds 5.
    shader.Load({Format1(0x13, 0x10, 0x20 + 94, 0, 0, 1), End});
    shader.registers.address.at(0) = 5;
    shader.Run();
    EXPECT_EQ(shader.registers.temporaries.at(0), (Vec4{1, 1, 1, 1}));
}

TEST(RunShader, IgnoresAnOffsetOutsideASignedByte) {
    Fixture shader;
    shader.unit->float_uniforms.at(3) = {9, 9, 9, 9};
    shader.Load({Format1(0x13, 0x10, 0x20 + 3, 0, 0, 1), End});
    shader.registers.address.at(0) = 200;
    shader.Run();
    EXPECT_EQ(shader.registers.temporaries.at(0), (Vec4{9, 9, 9, 9}));
}

TEST(RunShader, SumsADotProductFromAPositiveZero) {
    Fixture shader;
    shader.Load({Format1(0x02, 0x10, 0x00, 0x01, 0), End});
    shader.registers.inputs.at(0) = {-0.0F, 0, 0, 0};
    shader.registers.inputs.at(1) = {1, 1, 1, 1};
    shader.Run();
    EXPECT_FALSE(std::signbit(shader.registers.temporaries.at(0).at(0)));
}

TEST(RunShader, MovesToTheAddressRegisters) {
    Fixture shader;
    shader.Load({Format1(0x12, 0, 0x00, 0, 0), End});
    shader.registers.inputs.at(0) = {-3.9F, 4.9F, 0, 0};
    shader.Run();
    EXPECT_EQ(shader.registers.address.at(0), -3);
    EXPECT_EQ(shader.registers.address.at(1), 4);
}

TEST(RunShader, LoopsAndStepsTheLoopCounter) {
    Fixture shader;
    // i0: three iterations (x = 2), aL from 1 stepping by 2. The body adds v0
    // to r0.
    shader.unit->int_uniforms.at(0) = 2 | (1 << 8) | (2 << 16);
    shader.Load({Flow(0x29, 1, 0), Format1(0x00, 0x10, 0x10, 0x00, 0), End});
    shader.registers.inputs.at(0) = {1, 1, 1, 1};
    shader.Run();
    EXPECT_EQ(shader.registers.temporaries.at(0), (Vec4{3, 3, 3, 3}));
    EXPECT_EQ(shader.registers.address.at(2), 7) << "1 + 2 + 2 + 2";
}

TEST(RunShader, TakesTheElseOfAnIf) {
    Fixture shader;
    // IFC on cmp.x == 1 (condition "just x", reference x 1): the if part is
    // 1, the else part 2 to 3. cmp.x is false.
    shader.Load({Flow(0x28, 2, 1, (2U << 22) | (1U << 25)),
                 Format1(0x13, 0x10, 0x00, 0, 0),
                 Format1(0x13, 0x11, 0x00, 0, 0),
                 End});
    shader.registers.inputs.at(0) = {5, 5, 5, 5};
    shader.Run();
    EXPECT_EQ(shader.registers.temporaries.at(0), (Vec4{}));
    EXPECT_EQ(shader.registers.temporaries.at(1), (Vec4{5, 5, 5, 5}));
}

TEST(RunShader, ReturnsFromACall) {
    Fixture shader;
    // CALL 3 for one instruction, then END; at 3 copy v0 to r0.
    shader.Load({Flow(0x24, 3, 1), End, Nop, Format1(0x13, 0x10, 0x00, 0, 0), End});
    shader.registers.inputs.at(0) = {2, 2, 2, 2};
    const ShaderRunResult result = shader.Run();
    EXPECT_EQ(shader.registers.temporaries.at(0), (Vec4{2, 2, 2, 2}));
    EXPECT_EQ(result.instructions, 3U) << "CALL, the called MOV, END";
}

TEST(RunShader, InvertsJmpuOnTheCountsLowBit) {
    Fixture shader;
    // JMPU on b0, inverted: jumps when b0 is false.
    shader.Load({Flow(0x2D, 2, 1), Format1(0x13, 0x10, 0x00, 0, 0), End});
    shader.registers.inputs.at(0) = {1, 1, 1, 1};
    shader.Run();
    EXPECT_EQ(shader.registers.temporaries.at(0), (Vec4{})) << "b0 is false, so it jumped";
}

TEST(RunShader, EmitsAPrimitiveWithItsOutputsPacked) {
    Fixture shader;
    // Outputs o1 and o3 only; vertices 0, 1, then 2 with a primitive.
    GeometryEmitter emitter{0b1010};
    const auto set_emit = [](std::uint32_t vertex, bool primitive) {
        return (0x2BU << 26) | (vertex << 24) | (primitive ? 1U << 23 : 0U);
    };
    constexpr std::uint32_t Emit = 0x2AU << 26;
    shader.Load({set_emit(0, false), Emit, set_emit(1, false), Emit, set_emit(2, true), Emit, End});
    shader.registers.outputs[0].at(3) = {3, 3, 3, 3};
    shader.registers.outputs[1].at(3) = {4, 4, 4, 4};
    const ShaderRunResult result = shader.Run(&emitter);
    EXPECT_FALSE(result.unsupported);
    ASSERT_EQ(emitter.Primitives().size(), 1U);
    EXPECT_EQ(emitter.Primitives().front().vertices.at(0).at(1), (Vec4{3, 3, 3, 3})) << "o3 second";
    EXPECT_EQ(emitter.Primitives().front().vertices.at(1).at(1), (Vec4{4, 4, 4, 4}))
        << "EMIT switches the output bank";
}

TEST(RunShader, ReportsWhatItDoesNotImplement) {
    Fixture shader;
    shader.Load({Format1(0x04, 0x10, 0x00, 0x01, 0), End});
    EXPECT_TRUE(shader.Run().unsupported) << "DST";
    Fixture vertex;
    vertex.Load({0x2AU << 26, End});
    EXPECT_TRUE(vertex.Run().unsupported) << "EMIT with no emitter";
}

TEST(RunShader, StopsAProgramThatNeverEnds) {
    Fixture shader;
    shader.Load({Flow(0x2C, 0, 0, 0)});
    // JMPC with condition OR on references 0: cmp starts false, so it holds.
    const ShaderRunResult result = RunShader(*shader.unit, shader.registers, {.limit = 100});
    EXPECT_TRUE(result.stopped);
}

}  // namespace
}  // namespace hexlith::domain::gpu
