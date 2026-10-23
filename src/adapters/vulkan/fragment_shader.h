#ifndef HEXLITH_ADAPTERS_VULKAN_FRAGMENT_SHADER_H
#define HEXLITH_ADAPTERS_VULKAN_FRAGMENT_SHADER_H

#include <string_view>

namespace hexlith::adapters::vulkan {

// The GLSL of the backend's shaders, in pieces put together after the
// #version line and the float control lines (ADR 0022):
//
//   vertices shaded on the CPU: Common, Multiply, VertexOutput, VertexShader
//   vertices shaded on the GPU: Common, ShadedVertexInput, a translation of
//                               the program (domain::gpu::TranslateShader),
//                               VertexOutput, ShadedVertexMain
//   fragments:                  Common, Multiply, FragmentShader

// The layout of each draw's words, the push constants, and register access.
extern const std::string_view CommonSource;
extern const std::string_view MultiplySource;
// Placing a shaded vertex on the PICA's grid and handing it on.
extern const std::string_view VertexOutputSource;
extern const std::string_view VertexShaderSource;
extern const std::string_view ShadedVertexInputSource;
extern const std::string_view ShadedVertexMainSource;
extern const std::string_view FragmentShaderSource;

}  // namespace hexlith::adapters::vulkan

#endif  // HEXLITH_ADAPTERS_VULKAN_FRAGMENT_SHADER_H
