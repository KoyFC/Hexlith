#ifndef HEXLITH_ADAPTERS_VULKAN_FRAGMENT_SHADER_H
#define HEXLITH_ADAPTERS_VULKAN_FRAGMENT_SHADER_H

#include <string_view>

namespace hexlith::adapters::vulkan {

// The GLSL of the backend's two shaders, after the #version line and the
// float control lines. ADR 0022.
extern const std::string_view VertexShaderSource;
extern const std::string_view FragmentShaderSource;

}  // namespace hexlith::adapters::vulkan

#endif  // HEXLITH_ADAPTERS_VULKAN_FRAGMENT_SHADER_H
