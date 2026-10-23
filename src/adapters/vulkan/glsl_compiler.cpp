#include "adapters/vulkan/glsl_compiler.h"

#include <array>
#include <format>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <mutex>
#include <string>

namespace hexlith::adapters::vulkan {
namespace {

// glslang keeps process-wide state that has to be set up once.
void InitialiseOnce() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

EShLanguage Language(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:
        return EShLangVertex;
    case ShaderStage::Fragment:
        return EShLangFragment;
    default:
        return EShLangCompute;
    }
}

}  // namespace

core::Result<std::vector<std::uint32_t>> CompileGlsl(std::string_view source, ShaderStage stage) {
    constexpr int Version = 450;
    InitialiseOnce();
    const EShLanguage language = Language(stage);
    glslang::TShader shader(language);
    const std::string text{source};
    const std::array<const char*, 1> strings = {text.c_str()};
    shader.setStrings(strings.data(), 1);
    shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, Version);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), Version, false, messages)) {
        return core::Error{std::format("GLSL does not compile: {}", shader.getInfoLog())};
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        return core::Error{std::format("GLSL does not link: {}", program.getInfoLog())};
    }
    std::vector<std::uint32_t> spirv;
    glslang::SpvOptions options;
    options.validate = true;
    glslang::GlslangToSpv(*program.getIntermediate(language), spirv, &options);
    return spirv;
}

std::string FloatControlLines(const FloatControls& floats) {
    // SPIR-V's numbers: the capabilities DenormPreserve 4464 and
    // SignedZeroInfNanPreserve 4466, the execution modes 4459 and 4461, each
    // with the float width as its operand. Source: the SPIR-V specification,
    // 3.6 and 3.31.
    std::string lines = "#extension GL_EXT_spirv_intrinsics : require\n";
    if (floats.keeps_special_values) {
        lines += "spirv_execution_mode(capabilities = [4466], 4461, 32);\n";
    }
    if (floats.keeps_denormals) {
        lines += "spirv_execution_mode(capabilities = [4464], 4459, 32);\n";
    }
    return lines;
}

}  // namespace hexlith::adapters::vulkan
