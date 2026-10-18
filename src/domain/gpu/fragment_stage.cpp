#include "domain/gpu/fragment_stage.h"

#include "domain/gpu/float24.h"
#include "domain/gpu/shader_arithmetic.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

namespace hexlith::domain::gpu {
namespace {

constexpr std::uint32_t Field(std::uint32_t word, unsigned low, unsigned width) {
    return (word >> low) & ((1U << width) - 1);
}

constexpr std::uint8_t Full = 0xFF;

std::array<std::uint8_t, 4> Channels(const Color& color) {
    return {color.red, color.green, color.blue, color.alpha};
}

Color FromChannels(const std::array<std::uint32_t, 4>& channels) {
    return Color{.red = static_cast<std::uint8_t>(channels[0]),
                 .green = static_cast<std::uint8_t>(channels[1]),
                 .blue = static_cast<std::uint8_t>(channels[2]),
                 .alpha = static_cast<std::uint8_t>(channels[3])};
}

Color ColorRegister(std::uint32_t value) {
    return Color{.red = static_cast<std::uint8_t>(Field(value, 0, 8)),
                 .green = static_cast<std::uint8_t>(Field(value, 8, 8)),
                 .blue = static_cast<std::uint8_t>(Field(value, 16, 8)),
                 .alpha = static_cast<std::uint8_t>(Field(value, 24, 8))};
}

// ---------------------------------------------------------------------------
// Texturing. Source: GBATEK, Texturing registers (Generic Textures); Azahar's
// RasterizerSoftware::TextureColor and GetWrappedTexCoord for the nearest
// sampling and the wrap modes past 3.

struct TextureUnit {
    std::uint16_t border;
    std::uint16_t dimensions;
    std::uint16_t parameters;
    std::uint16_t levels;
    std::uint16_t address;
    std::uint16_t format;
};

constexpr std::array<TextureUnit, 3> Units = {{
    {.border = 0x081,
     .dimensions = 0x082,
     .parameters = 0x083,
     .levels = 0x084,
     .address = 0x085,
     .format = 0x08E},
    {.border = 0x091,
     .dimensions = 0x092,
     .parameters = 0x093,
     .levels = 0x094,
     .address = 0x095,
     .format = 0x096},
    {.border = 0x099,
     .dimensions = 0x09A,
     .parameters = 0x09B,
     .levels = 0x09C,
     .address = 0x09D,
     .format = 0x09E},
}};

constexpr std::uint16_t TextureConfig = 0x080;

enum class Wrap : std::uint8_t {
    ClampToEdge = 0,
    ClampToBorder = 1,
    Repeat = 2,
    MirroredRepeat = 3,
    // As Azahar has them: clamp to the edge for a positive coordinate and
    // repeat for a negative; clamp to the border likewise; 6 and 7 repeat.
    ClampToEdgeOrRepeat = 4,
    ClampToBorderOrRepeat = 5,
};

std::int32_t Modulo(std::int32_t value, std::uint32_t size) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(value) % size);
}

// Whether a coordinate falls on the border colour instead of a texel.
bool OnBorder(std::uint32_t wrap, std::int32_t value, std::uint32_t size) {
    if (wrap == static_cast<std::uint32_t>(Wrap::ClampToBorder)) {
        return value < 0 || std::cmp_greater_equal(value, size);
    }
    if (wrap == static_cast<std::uint32_t>(Wrap::ClampToBorderOrRepeat)) {
        return std::cmp_greater_equal(value, size);
    }
    return false;
}

std::int32_t Wrapped(std::uint32_t wrap, std::int32_t value, std::uint32_t size) {
    const auto last = static_cast<std::int32_t>(size) - 1;
    switch (static_cast<Wrap>(wrap)) {
    case Wrap::ClampToEdgeOrRepeat:
        if (value < 0) {
            return Modulo(value, size);
        }
        return std::clamp(value, 0, last);
    case Wrap::ClampToEdge:
        return std::clamp(value, 0, last);
    case Wrap::ClampToBorder:
        return value;
    case Wrap::MirroredRepeat: {
        std::uint32_t coordinate = static_cast<std::uint32_t>(value) % (2 * size);
        if (coordinate >= size) {
            coordinate = (2 * size) - 1 - coordinate;
        }
        return static_cast<std::int32_t>(coordinate);
    }
    default:
        return Modulo(value, size);
    }
}

struct TextureDescription {
    std::uint32_t address = 0;
    TextureFormat format = TextureFormat::Rgba8;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t wrap_s = 0;
    std::uint32_t wrap_t = 0;
    Color border;
};

// The texel at (s, t) of a level, t counted from the bottom as texture
// coordinates are, or the border colour.
Color Fetch(const GpuMemory& memory,
            const TextureDescription& texture,
            std::int32_t s,
            std::int32_t t) {
    if (OnBorder(texture.wrap_s, s, texture.width) || OnBorder(texture.wrap_t, t, texture.height)) {
        return texture.border;
    }
    const std::int32_t x = Wrapped(texture.wrap_s, s, texture.width);
    const std::int32_t y =
        static_cast<std::int32_t>(texture.height) - 1 - Wrapped(texture.wrap_t, t, texture.height);
    const auto size =
        static_cast<std::uint32_t>(TextureBytes(texture.format, texture.width, texture.height));
    const std::span<const std::uint8_t> bytes = memory.Bytes(texture.address, size);
    if (bytes.size() != size || x < 0 || y < 0 || std::cmp_greater_equal(x, texture.width) ||
        std::cmp_greater_equal(y, texture.height)) {
        return Color{};
    }
    return DecodeTexel(bytes,
                       texture.format,
                       texture.width,
                       static_cast<std::uint32_t>(x),
                       static_cast<std::uint32_t>(y));
}

// Four texels weighted by where the sample falls between their centres.
Color Bilinear(const GpuMemory& memory, const TextureDescription& texture, float u, float v) {
    const float x = (u * static_cast<float>(texture.width)) - 0.5F;
    const float y = (v * static_cast<float>(texture.height)) - 0.5F;
    const float x0 = std::floor(x);
    const float y0 = std::floor(y);
    const float fx = x - x0;
    const float fy = y - y0;
    const auto s = static_cast<std::int32_t>(x0);
    const auto t = static_cast<std::int32_t>(y0);
    const std::array<std::uint8_t, 4> a = Channels(Fetch(memory, texture, s, t));
    const std::array<std::uint8_t, 4> b = Channels(Fetch(memory, texture, s + 1, t));
    const std::array<std::uint8_t, 4> c = Channels(Fetch(memory, texture, s, t + 1));
    const std::array<std::uint8_t, 4> d = Channels(Fetch(memory, texture, s + 1, t + 1));
    std::array<std::uint32_t, 4> result{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        const float top =
            (static_cast<float>(a[channel]) * (1.0F - fx)) + (static_cast<float>(b[channel]) * fx);
        const float bottom =
            (static_cast<float>(c[channel]) * (1.0F - fx)) + (static_cast<float>(d[channel]) * fx);
        result[channel] =
            static_cast<std::uint32_t>(std::lround((top * (1.0F - fy)) + (bottom * fy)));
    }
    return FromChannels(result);
}

Color Nearest(const GpuMemory& memory, const TextureDescription& texture, float u, float v) {
    return Fetch(memory,
                 texture,
                 static_cast<std::int32_t>(std::floor(u * static_cast<float>(texture.width))),
                 static_cast<std::int32_t>(std::floor(v * static_cast<float>(texture.height))));
}

// A mipmap level, its sides halved and its texels after the levels before.
TextureDescription Level(const TextureDescription& base, std::uint32_t level) {
    TextureDescription texture = base;
    for (std::uint32_t at = 0; at < level; ++at) {
        texture.address +=
            static_cast<std::uint32_t>(TextureBytes(texture.format, texture.width, texture.height));
        texture.width = std::max(texture.width / 2, 8U);
        texture.height = std::max(texture.height / 2, 8U);
    }
    return texture;
}

Color Blend(const Color& first, const Color& second, float weight) {
    const std::array<std::uint8_t, 4> a = Channels(first);
    const std::array<std::uint8_t, 4> b = Channels(second);
    std::array<std::uint32_t, 4> result{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        result[channel] = static_cast<std::uint32_t>(
            std::lround((static_cast<float>(a[channel]) * (1.0F - weight)) +
                        (static_cast<float>(b[channel]) * weight)));
    }
    return FromChannels(result);
}

// The hardware's filtering as the parameters describe it. The level of
// detail is the larger rate of change in texels, as a power of two, plus
// GPUREG_TEXUNITi_LOD's bias, within its least and greatest level. How
// precisely the hardware computes any of this is not known.
Color SampleFiltered(const GpuMemory& memory,
                     const TextureDescription& base,
                     std::uint32_t parameters,
                     std::uint32_t levels,
                     const TextureCoordinate& coordinate) {
    const auto width = static_cast<float>(base.width);
    const auto height = static_cast<float>(base.height);
    const float rate_x = std::hypot(coordinate.du_dx * width, coordinate.dv_dx * height);
    const float rate_y = std::hypot(coordinate.du_dy * width, coordinate.dv_dy * height);
    const float rate = std::max(rate_x, rate_y);
    // fixed1.4.8
    constexpr float BiasScale = 256.0F;
    const auto bias_bits = static_cast<std::int32_t>(Field(levels, 0, 13) << 19U) >> 19;
    const float bias = static_cast<float>(bias_bits) / BiasScale;
    const auto least = static_cast<float>(Field(levels, 24, 4));
    const auto greatest = static_cast<float>(Field(levels, 16, 4));
    const float lod =
        std::clamp((rate > 0.0F ? std::log2(rate) : -INFINITY) + bias, least, greatest);

    const bool magnify = lod <= 0.0F;
    const bool linear = Field(parameters, magnify ? 1 : 2, 1) != 0;
    const auto sample = [&](std::uint32_t level) {
        const TextureDescription texture = Level(base, level);
        return linear ? Bilinear(memory, texture, coordinate.u, coordinate.v)
                      : Nearest(memory, texture, coordinate.u, coordinate.v);
    };
    if (magnify) {
        return sample(0);
    }
    const bool mip_linear = Field(parameters, 24, 1) != 0;
    if (!mip_linear) {
        return sample(static_cast<std::uint32_t>(std::floor(lod + 0.5F)));
    }
    const auto lower = static_cast<std::uint32_t>(std::floor(lod));
    const auto upper = std::min(lower + 1, static_cast<std::uint32_t>(greatest));
    return Blend(sample(lower), sample(upper), lod - std::floor(lod));
}

// ---------------------------------------------------------------------------
// The texture combiners. Source: GBATEK, Texturing registers (Environment),
// for the registers; Azahar's WriteTevConfig and sw_texturing.cpp for the
// arithmetic, for the buffer's one stage of delay, and for the first stage
// reading its third source where its first or second names the previous
// stage.

constexpr std::array<std::uint16_t, 6> StageBases = {0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8};
constexpr std::uint16_t CombinerBufferUpdate = 0x0E0;
constexpr std::uint16_t CombinerBufferColor = 0x0FD;

namespace source {
constexpr std::uint32_t PrimaryColor = 0x0;
constexpr std::uint32_t PrimaryFragment = 0x1;
constexpr std::uint32_t SecondaryFragment = 0x2;
constexpr std::uint32_t Texture0 = 0x3;
constexpr std::uint32_t Texture3 = 0x6;
constexpr std::uint32_t PreviousBuffer = 0xD;
constexpr std::uint32_t Constant = 0xE;
constexpr std::uint32_t Previous = 0xF;
}  // namespace source

std::array<std::uint32_t, 3> ColorModifier(std::uint32_t modifier, const Color& value) {
    const std::array<std::uint8_t, 4> c = Channels(value);
    const auto all = [](std::uint32_t channel) {
        return std::array<std::uint32_t, 3>{channel, channel, channel};
    };
    switch (modifier) {
    case 0x0:
        return {c[0], c[1], c[2]};
    case 0x1:
        return {Full - c[0] + 0U, Full - c[1] + 0U, Full - c[2] + 0U};
    case 0x2:
        return all(c[3]);
    case 0x3:
        return all(Full - c[3]);
    case 0x4:
        return all(c[0]);
    case 0x5:
        return all(Full - c[0]);
    case 0x8:
        return all(c[1]);
    case 0x9:
        return all(Full - c[1]);
    case 0xC:
        return all(c[2]);
    case 0xD:
        return all(Full - c[2]);
    default:
        return {};
    }
}

std::uint32_t AlphaModifier(std::uint32_t modifier, const Color& value) {
    const std::array<std::uint8_t, 4> c = Channels(value);
    // Alpha, red, green, blue, each followed by its complement.
    constexpr std::array<std::size_t, 4> Order = {3, 0, 1, 2};
    const std::uint32_t channel = c.at(Order.at((modifier >> 1U) & 3U));
    return (modifier & 1U) != 0 ? Full - channel : channel;
}

// One channel of an operation that works a channel at a time.
std::uint32_t
CombineChannel(std::uint32_t operation, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    switch (operation) {
    case 0:
        return a;
    case 1:
        return (a * b) / Full;
    case 2:
        return std::min(a + b, std::uint32_t{Full});
    case 3:
        return static_cast<std::uint32_t>(
            std::clamp(static_cast<std::int32_t>(a + b) - 128, 0, int{Full}));
    case 4:
        return ((a * c) + (b * (Full - c))) / Full;
    case 5:
        return a > b ? a - b : 0;
    case 8:
        return std::min(((a * b) + (Full * c)) / Full, std::uint32_t{Full});
    case 9:
        return (std::min(a + b, std::uint32_t{Full}) * c) / Full;
    default:
        return 0;
    }
}

// DOT3: the inputs as signed values from -1 to 1, multiplied a channel at a
// time to 1/256 and summed. Azahar says this is within 3 of the hardware.
std::uint32_t Dot3(const std::array<std::array<std::uint32_t, 3>, 3>& inputs) {
    std::int32_t sum = 0;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto a = static_cast<std::int32_t>(inputs[0][channel] * 2) - Full;
        const auto b = static_cast<std::int32_t>(inputs[1][channel] * 2) - Full;
        sum += ((a * b) + 128) / 256;
    }
    return static_cast<std::uint32_t>(std::clamp(sum, 0, int{Full}));
}

// What a combiner stage can take its sources from.
struct CombinerInputs {
    const std::array<Color, 4>* textures = nullptr;
    Color primary;
    std::array<Color, 2> lighting;
    Color buffer;
    Color previous;
};

Color StageSource(const CombinerInputs& inputs, const Color& constant, std::uint32_t which) {
    switch (which) {
    case source::PrimaryColor:
        return inputs.primary;
    case source::PrimaryFragment:
        return inputs.lighting[0];
    case source::SecondaryFragment:
        return inputs.lighting[1];
    case source::PreviousBuffer:
        return inputs.buffer;
    case source::Constant:
        return constant;
    case source::Previous:
        return inputs.previous;
    default:
        if (which >= source::Texture0 && which <= source::Texture3) {
            return inputs.textures->at(which - source::Texture0);
        }
        return Color{};
    }
}

// One stage: three sources each for colour and alpha, each through its
// modifier, combined by the stage's operations and scaled.
Color RunStage(const RegisterFile& registers, std::size_t stage, const CombinerInputs& inputs) {
    const std::uint16_t base = StageBases.at(stage);
    const std::uint32_t sources = registers.at(base);
    const std::uint32_t operands = registers.at(base + 1U);
    const std::uint32_t operations = registers.at(base + 2U);
    const Color constant = ColorRegister(registers.at(base + 3U));
    const std::uint32_t scales = registers.at(base + 4U);

    std::array<std::uint32_t, 3> color_sources = {
        Field(sources, 0, 4), Field(sources, 4, 4), Field(sources, 8, 4)};
    for (std::size_t at = 0; stage == 0 && at < 2; ++at) {
        if (color_sources.at(at) == source::Previous) {
            color_sources.at(at) = color_sources[2];
        }
    }
    std::array<std::array<std::uint32_t, 3>, 3> color_inputs{};
    std::array<std::uint32_t, 3> alpha_inputs{};
    for (std::size_t at = 0; at < 3; ++at) {
        const auto shift = 4 * static_cast<unsigned>(at);
        color_inputs.at(at) = ColorModifier(Field(operands, shift, 4),
                                            StageSource(inputs, constant, color_sources.at(at)));
        alpha_inputs.at(at) =
            AlphaModifier(Field(operands, 12 + shift, 3),
                          StageSource(inputs, constant, Field(sources, 16 + shift, 4)));
    }

    const std::uint32_t color_operation = Field(operations, 0, 4);
    std::array<std::uint32_t, 3> color{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        color.at(channel) = color_operation == 6 || color_operation == 7
                                ? Dot3(color_inputs)
                                : CombineChannel(color_operation,
                                                 color_inputs[0].at(channel),
                                                 color_inputs[1].at(channel),
                                                 color_inputs[2].at(channel));
    }
    // DOT3_RGBA puts its result in alpha as well.
    const std::uint32_t alpha =
        color_operation == 7
            ? color[0]
            : CombineChannel(
                  Field(operations, 16, 4), alpha_inputs[0], alpha_inputs[1], alpha_inputs[2]);

    // Scales of 1, 2 and 4; the fourth value is 1.
    const auto multiplier = [](std::uint32_t scale) { return scale < 3 ? 1U << scale : 1U; };
    const std::uint32_t color_scale = multiplier(Field(scales, 0, 2));
    const std::uint32_t alpha_scale = multiplier(Field(scales, 16, 2));
    return FromChannels({std::min(color[0] * color_scale, std::uint32_t{Full}),
                         std::min(color[1] * color_scale, std::uint32_t{Full}),
                         std::min(color[2] * color_scale, std::uint32_t{Full}),
                         std::min(alpha * alpha_scale, std::uint32_t{Full})});
}

// ---------------------------------------------------------------------------
// Fragment lighting. Source: GBATEK, Fragment Lighting registers; Azahar's
// ComputeFragmentsColors for the arithmetic.

using Vec3 = std::array<float, 3>;

Vec3 Add(const Vec3& a, const Vec3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Vec3 Scale(const Vec3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}

Vec3 Multiply(const Vec3& a, const Vec3& b) {
    return {a[0] * b[0], a[1] * b[1], a[2] * b[2]};
}

float Dot(const Vec3& a, const Vec3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {(a[1] * b[2]) - (a[2] * b[1]),
            (a[2] * b[0]) - (a[0] * b[2]),
            (a[0] * b[1]) - (a[1] * b[0])};
}

float Length(const Vec3& a) {
    return std::sqrt(Dot(a, a));
}

Vec3 Normalized(const Vec3& a) {
    const float length = Length(a);
    return {a[0] / length, a[1] / length, a[2] / length};
}

// v + 2 (q.xyz x (q.xyz x v + q.w v)), the rotation by a unit quaternion.
Vec3 Rotate(const Vec4& q, const Vec3& v) {
    const Vec3 axis{q[0], q[1], q[2]};
    return Add(v, Scale(Cross(axis, Add(Cross(axis, v), Scale(v, q[3]))), 2.0F));
}

// A light colour register: blue, green and red in ten bits each, 255 being
// one.
Vec3 LightColor(std::uint32_t value) {
    constexpr float One = 255.0F;
    return {static_cast<float>(Field(value, 20, 10)) / One,
            static_cast<float>(Field(value, 10, 10)) / One,
            static_cast<float>(Field(value, 0, 10)) / One};
}

namespace lighting {
constexpr std::uint16_t FirstLight = 0x140;
constexpr std::uint16_t LightStride = 0x10;
constexpr std::uint16_t Specular0 = 0x0;
constexpr std::uint16_t Specular1 = 0x1;
constexpr std::uint16_t Diffuse = 0x2;
constexpr std::uint16_t Ambient = 0x3;
constexpr std::uint16_t VectorLow = 0x4;
constexpr std::uint16_t VectorHigh = 0x5;
constexpr std::uint16_t SpotLow = 0x6;
constexpr std::uint16_t SpotHigh = 0x7;
constexpr std::uint16_t Config = 0x9;
constexpr std::uint16_t AttenuationBias = 0xA;
constexpr std::uint16_t AttenuationScale = 0xB;
constexpr std::uint16_t GlobalAmbient = 0x1C0;
constexpr std::uint16_t LightCount = 0x1C2;
constexpr std::uint16_t Config0 = 0x1C3;
constexpr std::uint16_t Config1 = 0x1C4;
constexpr std::uint16_t InputAbsolute = 0x1D0;
constexpr std::uint16_t InputSelect = 0x1D1;
constexpr std::uint16_t InputScale = 0x1D2;
constexpr std::uint16_t Permutation = 0x1D9;

// LUT numbers; each also names the nibble of its input, absolute and scale
// fields, and its bit in GPUREG_LIGHTING_CONFIG1 is 16 plus its number.
constexpr std::uint32_t D0 = 0;
constexpr std::uint32_t D1 = 1;
constexpr std::uint32_t Fresnel = 3;
constexpr std::uint32_t ReflectBlue = 4;
constexpr std::uint32_t ReflectGreen = 5;
constexpr std::uint32_t ReflectRed = 6;
constexpr std::uint32_t Spot = 8;
constexpr std::uint32_t Distance = 16;
// The input, absolute and scale fields of SP are its own; of the others, by
// LUT number but the reflections, which follow FR.
constexpr std::uint32_t SpotField = 2;
// The configuration that provides all seven tables, GBATEK's configuration 7.
constexpr std::uint32_t Configuration7 = 8;
}  // namespace lighting

// Which LUTs a light environment configuration provides. GBATEK's table:
// configurations 0 to 6 at their own values, configuration 7 at 8.
bool LutAvailable(std::uint32_t configuration, std::uint32_t lut) {
    switch (lut) {
    case lighting::D0:
        return configuration != 1;
    case lighting::D1:
        return configuration != 0 && configuration != 1 && configuration != 5;
    case lighting::Spot:
        return configuration != 2 && configuration != 3;
    case lighting::Fresnel:
        return configuration != 0 && configuration != 2 && configuration != 4;
    case lighting::ReflectRed:
        return configuration != 3;
    default:
        return configuration == 4 || configuration == 5 ||
               configuration == lighting::Configuration7;
    }
}

// A LUT entry: a value of twelve fraction bits, and the difference to the
// next with a sign bit above it. Source: GBATEK, GPUREG_LIGHTING_LUT_DATA.
float LookUp(const PicaState& state, std::uint32_t lut, std::uint32_t index, float delta) {
    constexpr float ValueScale = 4095.0F;
    constexpr float DifferenceScale = 2047.0F;
    const std::uint32_t entry = state.LightingLutData().at(lut).at(index);
    const float difference = static_cast<float>(Field(entry, 12, 11)) / DifferenceScale;
    return (static_cast<float>(Field(entry, 0, 12)) / ValueScale) +
           ((Field(entry, 23, 1) != 0 ? -difference : difference) * delta);
}

float LutScale(std::uint32_t scale) {
    switch (scale) {
    case 0:
        return 1.0F;
    case 1:
        return 2.0F;
    case 2:
        return 4.0F;
    case 3:
        return 8.0F;
    case 6:
        return 0.25F;
    case 7:
        return 0.5F;
    default:
        return 0.0F;
    }
}

// What a light adds to: the primary and secondary colours, alpha last.
struct LightSums {
    std::array<float, 4> diffuse{0.0F, 0.0F, 0.0F, 1.0F};
    std::array<float, 4> specular{0.0F, 0.0F, 0.0F, 1.0F};
};

// The vectors one light's tables can take their input from.
struct LightVectors {
    Vec3 light{};
    Vec3 view{};
    Vec3 half{};
    float distance = 0.0F;
};

// The lighting of one fragment: its surface, then each light in turn.
class LightingModel {
public:
    LightingModel(const RegisterFile& registers,
                  const PicaState& state,
                  const Vec4& quaternion,
                  const std::array<Color, 4>& textures)
        : registers_(&registers),
          state_(&state),
          config0_(registers.at(lighting::Config0)),
          config1_(registers.at(lighting::Config1)),
          configuration_(Field(config0_, 4, 4)) {
        ReadShadow(textures);
        ReadSurface(quaternion, textures);
    }

    void AddLight(std::uint32_t slot, bool last, const Vec3& view, LightSums& sums) const;
    [[nodiscard]] std::array<Color, 2> Finish(LightSums sums) const;

private:
    [[nodiscard]] static bool Bit(std::uint32_t word, unsigned bit) {
        return Field(word, bit, 1) != 0;
    }

    [[nodiscard]] std::uint32_t LightRegister(std::uint32_t number, std::uint16_t offset) const {
        return registers_->at(static_cast<std::uint16_t>(
            lighting::FirstLight + (lighting::LightStride * number) + offset));
    }

    [[nodiscard]] bool Enabled(std::uint32_t lut) const {
        return Field(config1_, 16 + lut, 1) == 0 && LutAvailable(configuration_, lut);
    }

    void ReadShadow(const std::array<Color, 4>& textures);
    void ReadSurface(const Vec4& quaternion, const std::array<Color, 4>& textures);
    [[nodiscard]] float
    LutInput(std::uint32_t select, std::uint32_t number, const LightVectors& vectors) const;
    [[nodiscard]] float LutValue(std::uint32_t field,
                                 std::uint32_t table,
                                 std::uint32_t number,
                                 const LightVectors& vectors) const;
    [[nodiscard]] float DistanceAttenuation(std::uint32_t number, float distance) const;
    [[nodiscard]] std::array<Vec3, 2> Speculars(std::uint32_t number,
                                                const LightVectors& vectors) const;
    void ApplyFresnel(std::uint32_t number, const LightVectors& vectors, LightSums& sums) const;

    const RegisterFile* registers_;
    const PicaState* state_;
    std::uint32_t config0_;
    std::uint32_t config1_;
    std::uint32_t configuration_;
    std::array<float, 4> shadow_{1.0F, 1.0F, 1.0F, 1.0F};
    Vec3 normal_{};
    Vec3 tangent_{};
};

// The shadow texture's colour, inverted if asked, when shadows are on.
void LightingModel::ReadShadow(const std::array<Color, 4>& textures) {
    constexpr float One = 255.0F;
    if (!Bit(config0_, 0)) {
        return;
    }
    const std::array<std::uint8_t, 4> texel = Channels(textures.at(Field(config0_, 24, 2)));
    for (std::size_t channel = 0; channel < 4; ++channel) {
        shadow_.at(channel) = static_cast<float>(texel.at(channel)) / One;
        if (Bit(config0_, 18)) {
            shadow_.at(channel) = 1.0F - shadow_.at(channel);
        }
    }
}

// The surface's normal and tangent, from a bump map if there is one, turned
// by the interpolated quaternion made a unit one. The bump map's mode 1
// perturbs the normal, 2 the tangent; Azahar leaves both zero for the fourth
// mode, which nobody has described.
void LightingModel::ReadSurface(const Vec4& quaternion, const std::array<Color, 4>& textures) {
    Vec3 surface_normal{0.0F, 0.0F, 1.0F};
    Vec3 surface_tangent{1.0F, 0.0F, 0.0F};
    const std::uint32_t bump_mode = Field(config0_, 28, 2);
    if (bump_mode != 0) {
        const std::array<std::uint8_t, 4> bump = Channels(textures.at(Field(config0_, 22, 2)));
        // The texel from 0 to 255 read as -1 to 1.
        constexpr float HalfRange = 127.5F;
        Vec3 perturbation{(static_cast<float>(bump[0]) / HalfRange) - 1.0F,
                          (static_cast<float>(bump[1]) / HalfRange) - 1.0F,
                          (static_cast<float>(bump[2]) / HalfRange) - 1.0F};
        if (bump_mode == 1 && !Bit(config0_, 30)) {
            const float z_square =
                1.0F - ((perturbation[0] * perturbation[0]) + (perturbation[1] * perturbation[1]));
            perturbation[2] = std::sqrt(std::max(z_square, 0.0F));
        }
        switch (bump_mode) {
        case 1:
            surface_normal = perturbation;
            break;
        case 2:
            surface_tangent = perturbation;
            break;
        default:
            surface_normal = {};
            surface_tangent = {};
            break;
        }
    }
    const float length =
        std::sqrt((quaternion[0] * quaternion[0]) + (quaternion[1] * quaternion[1]) +
                  (quaternion[2] * quaternion[2]) + (quaternion[3] * quaternion[3]));
    const Vec4 unit{quaternion[0] / length,
                    quaternion[1] / length,
                    quaternion[2] / length,
                    quaternion[3] / length};
    normal_ = Rotate(unit, surface_normal);
    tangent_ = Rotate(unit, surface_tangent);
}

// GPUREG_LIGHTING_LUTINPUT_SELECT's inputs: 0 N.H, 1 V.H, 2 N.V, 3 L.N, 4
// -L.P with P the spot direction, 5 the cosine of phi, which only
// configuration 7 provides.
float LightingModel::LutInput(std::uint32_t select,
                              std::uint32_t number,
                              const LightVectors& vectors) const {
    switch (select) {
    case 0:
        return Dot(normal_, Normalized(vectors.half));
    case 1:
        return Dot(vectors.view, Normalized(vectors.half));
    case 2:
        return Dot(normal_, vectors.view);
    case 3:
        return Dot(vectors.light, normal_);
    case 4: {
        // fixed1.1.11.
        constexpr float SpotScale = 2047.0F;
        const auto signed13 = [](std::uint32_t value) {
            return static_cast<float>(static_cast<std::int32_t>(value << 19U) >> 19);
        };
        const std::uint32_t low = LightRegister(number, lighting::SpotLow);
        const Vec3 spot{signed13(Field(low, 0, 13)) / SpotScale,
                        signed13(Field(low, 16, 13)) / SpotScale,
                        signed13(Field(LightRegister(number, lighting::SpotHigh), 0, 13)) /
                            SpotScale};
        return Dot(vectors.light, spot);
    }
    case 5: {
        if (configuration_ != lighting::Configuration7) {
            return 0.0F;
        }
        const Vec3 half = Normalized(vectors.half);
        return Dot(Add(half, Scale(normal_, -Dot(normal_, half))), tangent_);
    }
    default:
        return 0.0F;
    }
}

// A table's value for its input: from 0 to 1 over 256 entries when the
// input is taken as its absolute value (or kept to zero, for a light that is
// not two-sided), from -1 to 1 over the entries as signed bytes otherwise,
// with the difference to the next entry interpolated, then scaled.
float LightingModel::LutValue(std::uint32_t field,
                              std::uint32_t table,
                              std::uint32_t number,
                              const LightVectors& vectors) const {
    constexpr float Entries = 256.0F;
    constexpr float HalfEntries = 128.0F;
    float input =
        LutInput(Field(registers_->at(lighting::InputSelect), 4 * field, 3), number, vectors);
    std::uint32_t index = 0;
    float delta = 0.0F;
    if (!Bit(registers_->at(lighting::InputAbsolute), (4 * field) + 1)) {
        input = Bit(LightRegister(number, lighting::Config), 1) ? std::abs(input)
                                                                : std::max(input, 0.0F);
        index = TruncateToUnsigned(std::clamp(std::floor(input * Entries), 0.0F, Entries - 1.0F));
        delta = (input * Entries) - static_cast<float>(index);
    } else {
        const std::int32_t signed_index = ShaderTruncate(
            std::clamp(std::floor(input * HalfEntries), -HalfEntries, HalfEntries - 1.0F));
        delta = (input * HalfEntries) - static_cast<float>(signed_index);
        index = static_cast<std::uint32_t>(signed_index) & Full;
    }
    return LutScale(Field(registers_->at(lighting::InputScale), 4 * field, 3)) *
           LookUp(*state_, table, index, delta);
}

// The distance attenuation table, indexed by the light's distance times its
// scale plus its bias, both float1.7.12.
float LightingModel::DistanceAttenuation(std::uint32_t number, float distance) const {
    constexpr unsigned Float20Bits = 20;
    constexpr float Entries = 256.0F;
    if (Bit(config1_, 24 + number)) {
        return 1.0F;
    }
    const float scale =
        Float20ToFloat32(Field(LightRegister(number, lighting::AttenuationScale), 0, Float20Bits));
    const float bias =
        Float20ToFloat32(Field(LightRegister(number, lighting::AttenuationBias), 0, Float20Bits));
    const float sample = std::clamp((scale * distance) + bias, 0.0F, 1.0F);
    const std::uint32_t index =
        TruncateToUnsigned(std::clamp(std::floor(sample * Entries), 0.0F, Entries - 1.0F));
    return LookUp(*state_,
                  lighting::Distance + number,
                  index,
                  (sample * Entries) - static_cast<float>(index));
}

// The light's two specular colours through the distribution and reflection
// tables. Green and blue reflect as red does unless their own tables are on.
std::array<Vec3, 2> LightingModel::Speculars(std::uint32_t number,
                                             const LightVectors& vectors) const {
    namespace l = lighting;
    const auto value = [&](std::uint32_t lut) {
        return Enabled(lut) ? LutValue(lut, lut, number, vectors) : 1.0F;
    };
    Vec3 reflection{};
    reflection[0] = value(l::ReflectRed);
    reflection[1] = Enabled(l::ReflectGreen) ? value(l::ReflectGreen) : reflection[0];
    reflection[2] = Enabled(l::ReflectBlue) ? value(l::ReflectBlue) : reflection[0];
    return {
        Scale(LightColor(LightRegister(number, l::Specular0)), value(l::D0)),
        Multiply(Scale(reflection, value(l::D1)), LightColor(LightRegister(number, l::Specular1)))};
}

// The Fresnel table replaces either alpha or both, as GPUREG_LIGHTING_CONFIG0
// says.
void LightingModel::ApplyFresnel(std::uint32_t number,
                                 const LightVectors& vectors,
                                 LightSums& sums) const {
    if (!Enabled(lighting::Fresnel)) {
        return;
    }
    const float fresnel = LutValue(lighting::Fresnel, lighting::Fresnel, number, vectors);
    sums.diffuse[3] = Bit(config0_, 2) ? fresnel : sums.diffuse[3];
    sums.specular[3] = Bit(config0_, 3) ? fresnel : sums.specular[3];
}

void LightingModel::AddLight(std::uint32_t slot,
                             bool last,
                             const Vec3& view,
                             LightSums& sums) const {
    namespace l = lighting;
    const std::uint32_t number = Field(registers_->at(l::Permutation), 4 * slot, 3);
    const std::uint32_t light_config = LightRegister(number, l::Config);
    const std::uint32_t vector_low = LightRegister(number, l::VectorLow);
    const Vec3 position{Float16ToFloat32(Field(vector_low, 0, 16)),
                        Float16ToFloat32(Field(vector_low, 16, 16)),
                        Float16ToFloat32(Field(LightRegister(number, l::VectorHigh), 0, 16))};
    // A directional light's vector is its position; a positional light's
    // runs from the fragment, which the view vector locates.
    LightVectors vectors;
    vectors.light = Bit(light_config, 0) ? position : Add(position, view);
    vectors.distance = Length(vectors.light);
    vectors.light = Normalized(vectors.light);
    vectors.view = Normalized(view);
    vectors.half = Add(vectors.view, vectors.light);
    const float distance_attenuation = DistanceAttenuation(number, vectors.distance);

    float spot_attenuation = 1.0F;
    if (!Bit(config1_, 8 + number) && LutAvailable(configuration_, l::Spot)) {
        spot_attenuation = LutValue(l::SpotField, l::Spot + number, number, vectors);
    }
    auto [specular_0, specular_1] = Speculars(number, vectors);
    // Only the last light applies the Fresnel factor, to the alphas.
    if (last) {
        ApplyFresnel(number, vectors, sums);
    }

    float dot_product = Dot(vectors.light, normal_);
    dot_product = Bit(light_config, 1) ? std::abs(dot_product) : std::max(dot_product, 0.0F);
    const float clamp_highlights = Bit(config0_, 27) && dot_product == 0.0F ? 0.0F : 1.0F;
    if (Bit(light_config, 2) || Bit(light_config, 3)) {
        const float half_length = Dot(vectors.half, vectors.half);
        const float geometric =
            half_length == 0.0F ? 0.0F : std::min(dot_product / half_length, 1.0F);
        specular_0 = Bit(light_config, 2) ? Scale(specular_0, geometric) : specular_0;
        specular_1 = Bit(light_config, 3) ? Scale(specular_1, geometric) : specular_1;
    }
    const bool shadowed = !Bit(config1_, number);
    const Vec3 shadow{shadow_[0], shadow_[1], shadow_[2]};
    const Vec3 unshadowed{1.0F, 1.0F, 1.0F};
    const Vec3 primary_shadow = Bit(config0_, 16) && shadowed ? shadow : unshadowed;
    const Vec3 secondary_shadow = Bit(config0_, 17) && shadowed ? shadow : unshadowed;
    const Vec3 diffuse =
        Scale(Scale(Add(Multiply(Scale(LightColor(LightRegister(number, l::Diffuse)), dot_product),
                                 primary_shadow),
                        LightColor(LightRegister(number, l::Ambient))),
                    distance_attenuation),
              spot_attenuation);
    const Vec3 specular = Multiply(
        Scale(Scale(Scale(Add(specular_0, specular_1), clamp_highlights), distance_attenuation),
              spot_attenuation),
        secondary_shadow);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        sums.diffuse.at(channel) += diffuse.at(channel);
        sums.specular.at(channel) += specular.at(channel);
    }
}

// The shadow's alpha, the global ambient, and the sums kept to one and made
// colours.
std::array<Color, 2> LightingModel::Finish(LightSums sums) const {
    if (Bit(config0_, 19)) {
        sums.diffuse[3] *= Bit(config0_, 2) ? shadow_[3] : 1.0F;
        sums.specular[3] *= Bit(config0_, 3) ? shadow_[3] : 1.0F;
    }
    const Vec3 ambient = LightColor(registers_->at(lighting::GlobalAmbient));
    for (std::size_t channel = 0; channel < 3; ++channel) {
        sums.diffuse.at(channel) += ambient.at(channel);
    }
    const auto to_color = [](const std::array<float, 4>& sum) {
        std::array<std::uint32_t, 4> channels{};
        for (std::size_t channel = 0; channel < 4; ++channel) {
            channels.at(channel) =
                TruncateToUnsigned(std::clamp(sum.at(channel), 0.0F, 1.0F) * 255.0F) & Full;
        }
        return FromChannels(channels);
    };
    return {to_color(sums.diffuse), to_color(sums.specular)};
}

// ---------------------------------------------------------------------------
// The output merger. Source: GBATEK, Framebuffer registers; Azahar's
// PixelColor and EvaluateBlendEquation for the arithmetic.

// GPUREG_BLEND_FUNC's factors, as a byte that 255 makes one.
std::uint32_t BlendFactor(std::uint32_t which,
                          std::size_t channel,
                          const std::array<std::uint8_t, 4>& src,
                          const std::array<std::uint8_t, 4>& dst,
                          const std::array<std::uint8_t, 4>& constant) {
    constexpr std::size_t Alpha = 3;
    switch (which) {
    case 0x0:
        return 0;
    case 0x1:
        return Full;
    case 0x2:
        return src.at(channel);
    case 0x3:
        return Full - src.at(channel);
    case 0x4:
        return dst.at(channel);
    case 0x5:
        return Full - dst.at(channel);
    case 0x6:
        return src[Alpha];
    case 0x7:
        return Full - src[Alpha];
    case 0x8:
        return dst[Alpha];
    case 0x9:
        return Full - dst[Alpha];
    case 0xA:
        return constant.at(channel);
    case 0xB:
        return Full - constant.at(channel);
    case 0xC:
        return constant[Alpha];
    case 0xD:
        return Full - constant[Alpha];
    case 0xE:
        // Source alpha saturate, one for alpha itself.
        return channel == Alpha ? Full : std::min<std::uint32_t>(src[Alpha], Full - dst[Alpha]);
    default:
        // Azahar's choice for the one value nobody has described.
        return src.at(channel);
    }
}

// Each channel's source and destination times their factors, combined by
// the equation, over 255: add, subtract, reverse subtract, minimum, maximum.
// GBATEK says 5 to 7 add too.
std::array<std::uint32_t, 4>
Blend(const RegisterFile& registers, const Color& source, const Color& destination) {
    constexpr std::uint16_t BlendFunction = 0x101;
    constexpr std::uint16_t BlendColor = 0x103;
    const std::uint32_t function = registers.at(BlendFunction);
    const std::array<std::uint8_t, 4> src = Channels(source);
    const std::array<std::uint8_t, 4> dst = Channels(destination);
    const std::array<std::uint8_t, 4> constant = Channels(ColorRegister(registers.at(BlendColor)));
    std::array<std::uint32_t, 4> result{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        const bool alpha = channel == 3;
        const auto s = static_cast<std::int32_t>(
            src.at(channel) *
            BlendFactor(Field(function, alpha ? 24 : 16, 4), channel, src, dst, constant));
        const auto d = static_cast<std::int32_t>(
            dst.at(channel) *
            BlendFactor(Field(function, alpha ? 28 : 20, 4), channel, src, dst, constant));
        std::int32_t value = 0;
        switch (Field(function, alpha ? 8 : 0, 3)) {
        case 1:
            value = s - d;
            break;
        case 2:
            value = d - s;
            break;
        case 3:
            value = std::min(s, d);
            break;
        case 4:
            value = std::max(s, d);
            break;
        default:
            value = s + d;
            break;
        }
        result.at(channel) =
            static_cast<std::uint32_t>(std::clamp(value / int{Full}, 0, int{Full}));
    }
    return result;
}

// GPUREG_LOGIC_OP, a channel at a time. Source: GBATEK.
std::array<std::uint32_t, 4>
ApplyLogicOperation(const RegisterFile& registers, const Color& source, const Color& destination) {
    constexpr std::uint16_t LogicOperation = 0x102;
    using Operation = std::uint32_t (*)(std::uint32_t, std::uint32_t);
    constexpr std::array<Operation, 16> Operations = {
        [](std::uint32_t, std::uint32_t) { return 0U; },
        [](std::uint32_t a, std::uint32_t b) { return a & b; },
        [](std::uint32_t a, std::uint32_t b) { return a & ~b; },
        [](std::uint32_t a, std::uint32_t) { return a; },
        [](std::uint32_t, std::uint32_t) { return 0xFFU; },
        [](std::uint32_t a, std::uint32_t) { return ~a; },
        [](std::uint32_t, std::uint32_t b) { return b; },
        [](std::uint32_t, std::uint32_t b) { return ~b; },
        [](std::uint32_t a, std::uint32_t b) { return ~(a & b); },
        [](std::uint32_t a, std::uint32_t b) { return a | b; },
        [](std::uint32_t a, std::uint32_t b) { return ~(a | b); },
        [](std::uint32_t a, std::uint32_t b) { return a ^ b; },
        [](std::uint32_t a, std::uint32_t b) { return ~(a ^ b); },
        [](std::uint32_t a, std::uint32_t b) { return ~a & b; },
        [](std::uint32_t a, std::uint32_t b) { return a | ~b; },
        [](std::uint32_t a, std::uint32_t b) { return ~a | b; },
    };
    const Operation operation = Operations.at(Field(registers.at(LogicOperation), 0, 4));
    const std::array<std::uint8_t, 4> src = Channels(source);
    const std::array<std::uint8_t, 4> dst = Channels(destination);
    std::array<std::uint32_t, 4> result{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        result.at(channel) = operation(src.at(channel), dst.at(channel)) & Full;
    }
    return result;
}
}  // namespace

std::array<Color, 4> SampleTextures(const RegisterFile& registers,
                                    const GpuMemory& memory,
                                    const std::array<TextureCoordinate, 3>& coordinates,
                                    float texcoord0_w,
                                    Sampling sampling,
                                    bool& unsupported) {
    std::array<Color, 4> colors{};
    const std::uint32_t config = registers.at(TextureConfig);
    for (std::size_t index = 0; index < Units.size(); ++index) {
        if (Field(config, static_cast<unsigned>(index), 1) == 0) {
            continue;
        }
        const TextureUnit& unit = Units.at(index);
        const std::uint32_t parameters = registers.at(unit.parameters);
        const std::uint32_t address = (registers.at(unit.address) & 0x0FFFFFFFU) << 3U;
        if (address == 0) {
            colors.at(index) = Color{.red = 0, .green = 0, .blue = 0, .alpha = Full};
            continue;
        }
        // Unit 2 can take unit 1's coordinates.
        TextureCoordinate coordinate =
            coordinates.at(index == 2 && Field(config, 13, 1) != 0 ? 1 : index);
        if (index == 0) {
            // Unit 0's type: 0 a 2D texture, 3 a projected one, 5 off; cube
            // maps and shadow textures are not implemented.
            const std::uint32_t type = Field(parameters, 28, 3);
            if (type == 5) {
                continue;
            }
            if (type == 3) {
                coordinate.u /= texcoord0_w;
                coordinate.v /= texcoord0_w;
            } else if (type != 0) {
                unsupported = true;
                continue;
            }
        }
        const std::uint32_t dimensions = registers.at(unit.dimensions);
        const TextureDescription texture{
            .address = address,
            .format = static_cast<TextureFormat>(Field(registers.at(unit.format), 0, 4)),
            .width = Field(dimensions, 16, 11),
            .height = Field(dimensions, 0, 11),
            .wrap_s = Field(parameters, 12, 3),
            .wrap_t = Field(parameters, 8, 3),
            .border = ColorRegister(registers.at(unit.border))};
        if (texture.width == 0 || texture.height == 0 ||
            Field(registers.at(unit.format), 0, 4) >= TextureFormatCount) {
            continue;
        }
        if (sampling == Sampling::AzaharSoftware) {
            // The coordinate times the size, truncated, as Azahar's software
            // rasterizer takes it.
            colors.at(index) = Fetch(
                memory,
                texture,
                ShaderTruncate(ShaderMultiply(coordinate.u, static_cast<float>(texture.width))),
                ShaderTruncate(ShaderMultiply(coordinate.v, static_cast<float>(texture.height))));
        } else {
            colors.at(index) =
                SampleFiltered(memory, texture, parameters, registers.at(unit.levels), coordinate);
        }
    }
    // The procedural texture.
    if (Field(config, 10, 1) != 0) {
        unsupported = true;
    }
    return colors;
}

std::array<Color, 2> LightFragment(const RegisterFile& registers,
                                   const PicaState& state,
                                   const Vec4& quaternion,
                                   const std::array<float, 3>& view,
                                   const std::array<Color, 4>& textures) {
    const LightingModel model(registers, state, quaternion, textures);
    LightSums sums;
    const std::uint32_t last_light = Field(registers.at(lighting::LightCount), 0, 3);
    for (std::uint32_t slot = 0; slot <= last_light; ++slot) {
        model.AddLight(slot, slot == last_light, Vec3{view[0], view[1], view[2]}, sums);
    }
    return model.Finish(sums);
}

Color CombineTextures(const RegisterFile& registers,
                      const std::array<Color, 4>& textures,
                      const Color& primary,
                      const std::array<Color, 2>& lighting) {
    CombinerInputs inputs{.textures = &textures,
                          .primary = primary,
                          .lighting = lighting,
                          .buffer = {},
                          .previous = {}};
    Color next_buffer = ColorRegister(registers.at(CombinerBufferColor));
    const std::uint32_t update = registers.at(CombinerBufferUpdate);
    for (std::size_t stage = 0; stage < StageBases.size(); ++stage) {
        inputs.previous = RunStage(registers, stage, inputs);
        // The buffer a stage reads is the one the stage before it found.
        inputs.buffer = next_buffer;
        if (stage < 4 && Field(update, 8 + static_cast<unsigned>(stage), 1) != 0) {
            next_buffer.red = inputs.previous.red;
            next_buffer.green = inputs.previous.green;
            next_buffer.blue = inputs.previous.blue;
        }
        if (stage < 4 && Field(update, 12 + static_cast<unsigned>(stage), 1) != 0) {
            next_buffer.alpha = inputs.previous.alpha;
        }
    }
    return inputs.previous;
}

// Source: Azahar's WriteFog: GPUREG_TEXENV_UPDATE_BUFFER's fog mode 5, the
// depth flipped if bit 16 says so, and the 128-entry table interpolated.
void ApplyFog(const RegisterFile& registers, const PicaState& state, float depth, Color& color) {
    constexpr std::uint32_t FogMode = 5;
    constexpr std::uint16_t FogColor = 0x0E1;
    constexpr float Entries = 128.0F;
    constexpr float Scale = 2047.0F;
    const std::uint32_t update = registers.at(CombinerBufferUpdate);
    if (Field(update, 0, 3) != FogMode) {
        return;
    }
    const float index = (Field(update, 16, 1) != 0 ? 1.0F - depth : depth) * Entries;
    const float whole = std::clamp(std::floor(index), 0.0F, Entries - 1.0F);
    // A NaN depth reads the first entry, as Azahar's conversion makes it.
    const std::uint32_t entry = state.FogLut().at(TruncateToUnsigned(whole));
    const auto difference =
        static_cast<float>(static_cast<std::int32_t>(Field(entry, 0, 13) << 19U) >> 19);
    const float factor = std::clamp((static_cast<float>(Field(entry, 13, 11)) / Scale) +
                                        ((difference / Scale) * (index - whole)),
                                    0.0F,
                                    1.0F);
    const Color fog = ColorRegister(registers.at(FogColor));
    const std::array<std::uint8_t, 4> fog_channels = Channels(fog);
    std::array<std::uint8_t, 4> channels = Channels(color);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        channels.at(channel) = static_cast<std::uint8_t>(
            ShaderTruncate((factor * static_cast<float>(channels.at(channel))) +
                           ((1.0F - factor) * static_cast<float>(fog_channels.at(channel)))));
    }
    color = FromChannels({channels[0], channels[1], channels[2], channels[3]});
}

bool Compare(std::uint32_t function, std::uint32_t left, std::uint32_t right) {
    switch (function) {
    case 0:
        return false;
    case 1:
        return true;
    case 2:
        return left == right;
    case 3:
        return left != right;
    case 4:
        return left < right;
    case 5:
        return left <= right;
    case 6:
        return left > right;
    default:
        return left >= right;
    }
}

bool PassesAlphaTest(const RegisterFile& registers, std::uint8_t alpha) {
    constexpr std::uint16_t AlphaTest = 0x104;
    const std::uint32_t test = registers.at(AlphaTest);
    return Field(test, 0, 1) == 0 || Compare(Field(test, 4, 3), alpha, Field(test, 8, 8));
}

std::uint8_t StencilAction(std::uint32_t action, std::uint8_t old_value, std::uint8_t reference) {
    switch (action) {
    case 0:
        return old_value;
    case 1:
        return 0;
    case 2:
        return reference;
    case 3:
        return static_cast<std::uint8_t>(std::min<std::uint8_t>(old_value, Full - 1) + 1);
    case 4:
        return static_cast<std::uint8_t>(std::max<std::uint8_t>(old_value, 1) - 1);
    case 5:
        return static_cast<std::uint8_t>(~old_value);
    case 6:
        return static_cast<std::uint8_t>(old_value + 1);
    default:
        return static_cast<std::uint8_t>(old_value - 1);
    }
}

Color MergeColor(const RegisterFile& registers, const Color& source, const Color& destination) {
    constexpr std::uint16_t ColorOperation = 0x100;
    constexpr std::uint16_t DepthColorMask = 0x107;
    std::array<std::uint32_t, 4> result = Field(registers.at(ColorOperation), 8, 1) != 0
                                              ? Blend(registers, source, destination)
                                              : ApplyLogicOperation(registers, source, destination);
    // The colour write mask, red to alpha in bits 8 to 11.
    const std::array<std::uint8_t, 4> dst = Channels(destination);
    const std::uint32_t mask = registers.at(DepthColorMask);
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (Field(mask, 8 + static_cast<unsigned>(channel), 1) == 0) {
            result.at(channel) = dst.at(channel);
        }
    }
    return FromChannels(result);
}

}  // namespace hexlith::domain::gpu
