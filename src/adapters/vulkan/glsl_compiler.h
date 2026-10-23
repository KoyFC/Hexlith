#ifndef HEXLITH_ADAPTERS_VULKAN_GLSL_COMPILER_H
#define HEXLITH_ADAPTERS_VULKAN_GLSL_COMPILER_H

#include "adapters/vulkan/vulkan_context.h"
#include "core/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::adapters::vulkan {

enum class ShaderStage : std::uint8_t { Vertex, Fragment, Compute };

// GLSL 4.50 to SPIR-V 1.5 for Vulkan 1.2, with glslang. A failure carries
// glslang's log. ADR 0020.
core::Result<std::vector<std::uint32_t>> CompileGlsl(std::string_view source, ShaderStage stage);

// The lines after #version that ask for float arithmetic as the PICA's
// translation needs it, as far as the device promises it: signed zeros,
// infinities and NaNs kept, and denormals kept. They name SPIR-V's
// SignedZeroInfNanPreserve and DenormPreserve execution modes for 32-bit
// floats by number.
std::string FloatControlLines(const FloatControls& floats);

}  // namespace hexlith::adapters::vulkan

#endif  // HEXLITH_ADAPTERS_VULKAN_GLSL_COMPILER_H
