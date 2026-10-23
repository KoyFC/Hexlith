#ifndef HEXLITH_ADAPTERS_VULKAN_SHADER_CASES_H
#define HEXLITH_ADAPTERS_VULKAN_SHADER_CASES_H

#include "adapters/vulkan/vulkan_context.h"
#include "core/result.h"
#include "domain/gpu/pica_state.h"
#include "domain/gpu/shader_interpreter.h"

#include <cstdint>
#include <span>

namespace hexlith::adapters::vulkan {

// One run of a vertex shader program: the uniforms it sees and its
// registers, which the run leaves as the program does.
struct ShaderCaseState {
    std::array<domain::gpu::Vec4, domain::gpu::ShaderUnitState::FloatUniforms> float_uniforms{};
    std::uint16_t bool_uniforms = 0;
    std::array<std::uint32_t, domain::gpu::ShaderUnitState::IntUniforms> int_uniforms{};
    domain::gpu::ShaderRegisters registers;
    // Whether the program ended within the limit, and whether it reached
    // something the translation does not implement.
    bool ended = false;
    bool unsupported = false;
};

// Runs the translation of a program, its code and descriptors taken from
// the unit, on the GPU as a compute shader, once for each case. It is how the
// translation is checked against the interpreter (docs/pica-shaders.md).
core::Status RunVertexShaderCases(VulkanContext& context,
                                  const domain::gpu::ShaderUnitState& program,
                                  std::uint32_t entry,
                                  std::span<ShaderCaseState> cases,
                                  std::uint32_t limit);

}  // namespace hexlith::adapters::vulkan

#endif  // HEXLITH_ADAPTERS_VULKAN_SHADER_CASES_H
