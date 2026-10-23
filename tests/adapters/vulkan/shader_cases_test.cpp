#include "adapters/vulkan/shader_cases.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace hexlith::adapters::vulkan {
namespace {

namespace gpu = domain::gpu;

// GBATEK's encodings, as the interpreter's tests write them.
constexpr std::uint32_t End = 0x22U << 26;

constexpr std::uint32_t
Format1(std::uint32_t op, std::uint32_t dst, std::uint32_t src1, std::uint32_t src2) {
    return (op << 26) | (dst << 21) | (src1 << 12) | (src2 << 7);
}

// LOOP over i0, its body the next instruction.
constexpr std::uint32_t Loop(std::uint32_t last) {
    return (0x29U << 26) | (last << 10);
}

constexpr std::uint32_t Add = 0x00;
constexpr std::uint32_t Mul = 0x08;
constexpr std::uint32_t Max = 0x0C;

// A program of additions, products and a loop, with no division or
// transcendental function, whose results every Vulkan device must give to
// the bit.
std::unique_ptr<gpu::ShaderUnitState> Program() {
    auto unit = std::make_unique<gpu::ShaderUnitState>();
    unit->descriptors.at(0) = 0xF | (0x1BU << 5) | (0x1BU << 14) | (0x1BU << 23);
    const std::array<std::uint32_t, 6> code = {
        Format1(Mul, 0x10, 0x00, 0x01),  // r0 = v0 * v1
        Loop(2),
        Format1(Add, 0x10, 0x10, 0x00),  // r0 += v0, i0.x + 1 times
        Format1(Max, 0x01, 0x10, 0x02),  // o1 = max(r0, v2)
        Format1(Add, 0x00, 0x20, 0x10),  // o0 = c0 + r0
        End,
    };
    std::ranges::copy(code, unit->code.begin());
    return unit;
}

// No Vulkan device, as on a machine without drivers, is not a failure.
std::unique_ptr<VulkanContext> Context() {
    auto context = VulkanContext::Create({});
    if (!context) {
        return nullptr;
    }
    return std::move(context.Value());
}

// Three cases that loop one to three times.
std::vector<ShaderCaseState> Cases() {
    std::vector<ShaderCaseState> cases(3);
    for (std::size_t at = 0; at < cases.size(); ++at) {
        const auto value = static_cast<float>(at) + 0.5F;
        cases[at].registers.inputs[0] = {value, -value, 0.0F, 1e30F};
        cases[at].registers.inputs[1] = {2.0F, 3.0F, 0.0F, 1e30F};
        cases[at].registers.inputs[2] = {1.0F, 1.0F, -1.0F, 0.0F};
        cases[at].float_uniforms[0] = {0.25F, 0.5F, 0.75F, 1.0F};
        cases[at].int_uniforms[0] = static_cast<std::uint32_t>(at);
    }
    return cases;
}

ShaderCaseState Interpret(const gpu::ShaderUnitState& program, ShaderCaseState state) {
    gpu::ShaderUnitState unit = program;
    unit.float_uniforms = state.float_uniforms;
    unit.int_uniforms = state.int_uniforms;
    state.unsupported = gpu::RunShader(unit, state.registers, {.entry = 0}).unsupported;
    return state;
}

void ExpectSame(const ShaderCaseState& translated, const ShaderCaseState& interpreted) {
    EXPECT_TRUE(translated.ended);
    EXPECT_EQ(translated.registers.outputs[0], interpreted.registers.outputs[0]);
    EXPECT_EQ(translated.registers.temporaries[0], interpreted.registers.temporaries[0]);
    EXPECT_EQ(translated.registers.address, interpreted.registers.address);
}

TEST(RunVertexShaderCases, AgreesWithTheInterpreterToTheBit) {
    const std::unique_ptr<VulkanContext> context = Context();
    if (!context) {
        GTEST_SKIP() << "no Vulkan device";
    }
    const auto program = Program();
    std::vector<ShaderCaseState> cases = Cases();
    ASSERT_TRUE(RunVertexShaderCases(*context, *program, 0, cases, 1000).Ok());
    for (std::size_t at = 0; at < cases.size(); ++at) {
        ExpectSame(cases[at], Interpret(*program, Cases()[at]));
    }
    EXPECT_EQ(context->ValidationMessages(), 0U) << context->FirstValidationMessage();
}

}  // namespace
}  // namespace hexlith::adapters::vulkan
