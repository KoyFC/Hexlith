#ifndef HEXLITH_DOMAIN_GPU_FRAGMENT_STAGE_H
#define HEXLITH_DOMAIN_GPU_FRAGMENT_STAGE_H

#include "domain/gpu/gpu_memory.h"
#include "domain/gpu/pica_state.h"
#include "domain/gpu/pixel_formats.h"

#include <array>
#include <cstdint>

namespace hexlith::domain::gpu {

// What happens to a fragment once the rasterizer has interpolated it:
// texturing, fragment lighting, the texture combiners, fog, and the tests
// and blending of the output merger. docs/pica-rasterizer.md has the source
// of each rule.

// How texels are fetched. Hardware filters as the texture unit's registers
// say: bilinear for magnification and minification when set, mipmaps chosen
// by the level of detail. AzaharSoftware takes the nearest texel of the base
// level whatever the registers say, as Azahar's software rasterizer does,
// so that the reference can be compared with it pixel for pixel. ADR 0019.
enum class Sampling : std::uint8_t { Hardware, AzaharSoftware };

// A texture coordinate pair and how fast it changes from one pixel to the
// next, which the level of detail needs.
struct TextureCoordinate {
    float u = 0.0F;
    float v = 0.0F;
    float du_dx = 0.0F;
    float dv_dx = 0.0F;
    float du_dy = 0.0F;
    float dv_dy = 0.0F;
};

// The colours of texture units 0, 1 and 2, and the procedural texture's
// fourth. A unit that is disabled gives zero; `unsupported` is set when a
// unit asks for something not implemented: cube maps, shadow textures, the
// procedural texture.
std::array<Color, 4> SampleTextures(const RegisterFile& registers,
                                    const GpuMemory& memory,
                                    const std::array<TextureCoordinate, 3>& coordinates,
                                    float texcoord0_w,
                                    Sampling sampling,
                                    bool& unsupported);

// The primary and secondary colours fragment lighting gives, from the
// interpolated normal quaternion, which need not be normalised, and the view
// vector.
std::array<Color, 2> LightFragment(const RegisterFile& registers,
                                   const PicaState& state,
                                   const Vec4& quaternion,
                                   const std::array<float, 3>& view,
                                   const std::array<Color, 4>& textures);

// The six texture combiner stages.
Color CombineTextures(const RegisterFile& registers,
                      const std::array<Color, 4>& textures,
                      const Color& primary,
                      const std::array<Color, 2>& lighting);

// Fog, from the fragment's depth, when the fog mode asks for it.
void ApplyFog(const RegisterFile& registers, const PicaState& state, float depth, Color& color);

// GPUREG_FRAGOP_ALPHA_TEST.
bool PassesAlphaTest(const RegisterFile& registers, std::uint8_t alpha);

// The output merger's blending or logic operation, then the colour write
// mask, against the colour the buffer holds.
Color MergeColor(const RegisterFile& registers, const Color& source, const Color& destination);

// GPUREG_COMPARE functions, shared by the alpha, stencil and depth tests:
// 0 never, 1 always, 2 equal, 3 not equal, 4 less, 5 less or equal, 6
// greater, 7 greater or equal. Source: GBATEK, Framebuffer registers.
bool Compare(std::uint32_t function, std::uint32_t left, std::uint32_t right);

// GPUREG_STENCIL_OP's actions: keep, zero, replace, saturating increment and
// decrement, invert, wrapping increment and decrement.
std::uint8_t StencilAction(std::uint32_t action, std::uint8_t old_value, std::uint8_t reference);

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_FRAGMENT_STAGE_H
