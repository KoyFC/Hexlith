# The Vulkan backend, a library of its own so that only what draws links
# Vulkan and glslang. ADR 0020 and ADR 0022.
add_library(hexlith_vulkan STATIC)

add_library(Hexlith::Vulkan ALIAS hexlith_vulkan)

target_sources(
    hexlith_vulkan
    PRIVATE fragment_shader.cpp
            fragment_shader.h
            glsl_compiler.cpp
            glsl_compiler.h
            shader_cases.cpp
            shader_cases.h
            vulkan_backend.cpp
            vulkan_backend.h
            vulkan_context.cpp
            vulkan_context.h
)

target_link_libraries(hexlith_vulkan PUBLIC Hexlith::Application Hexlith::Ports Vulkan::Vulkan)

target_link_libraries(
    hexlith_vulkan PRIVATE Hexlith::Warnings glslang::glslang glslang::SPIRV
                           glslang::glslang-default-resource-limits
)
