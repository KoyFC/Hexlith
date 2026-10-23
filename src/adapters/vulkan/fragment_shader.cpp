#include "adapters/vulkan/fragment_shader.h"

namespace hexlith::adapters::vulkan {

// The shaders every draw of the Vulkan backend uses. The fragment shader is
// the reference's fragment stage written again in GLSL, function by
// function: domain/gpu/fragment_stage.cpp and the shading in rasterizer.cpp
// are what each part is checked against, and cite their sources. It reads the
// draw's whole register file rather than being generated for each
// configuration (ADR 0022).

const std::string_view CommonSource = R"(
// Each draw's words: the register file, the 24 lighting tables, the fog
// table, for each texture unit the offsets of its levels in the texels, then
// the vertex shader's float uniforms, its bool uniforms and its integer
// uniforms.
const uint RegisterWords = 1024u;
const uint LightingBase = RegisterWords;
const uint FogBase = LightingBase + 24u * 256u;
const uint LevelBase = FogBase + 128u;
const uint LevelsPerUnit = 16u;
const uint FloatUniformBase = LevelBase + 3u * LevelsPerUnit;
const uint BoolUniformWord = FloatUniformBase + 96u * 4u;
const uint IntUniformBase = BoolUniformWord + 1u;
const uint DrawWords = IntUniformBase + 4u;

layout(std430, set = 0, binding = 0) readonly buffer Draws {
    uint words[];
};

layout(push_constant) uniform Draw {
    uint draw;
    uint sampling;
    uint height;
    uint clip_enabled;
    vec4 clip_plane;
    // The PICA's viewport: half its width and height, and its corner.
    vec4 viewport;
};

uint reg(uint id) {
    return words[draw * DrawWords + id];
}

uint field(uint word, uint low, uint width) {
    return (word >> low) & ((1u << width) - 1u);
}

int signed_field(uint word, uint low, uint width) {
    return int(word << (32u - low - width)) >> int(32u - width);
}

)";

// Zero times infinity is zero (ADR 0018). A shader translation defines this
// itself.
const std::string_view MultiplySource = R"(
float pica_mul(float a, float b) {
    precise float product = a * b;
    return (isnan(product) && !isnan(a) && !isnan(b)) ? 0.0 : product;
}
)";

const std::string_view VertexOutputSource = R"(
layout(location = 0) out vec4 v_quaternion;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec4 v_texcoords;
layout(location = 3) out vec4 v_slots16;
layout(location = 4) out vec4 v_slots20;

out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[1];
};

// A shaded vertex out to the rasterizer. The PICA places a vertex on the
// screen in sixteenths of a pixel, rounded to nearest, and fills by a rule
// that is Vulkan's top-left rule seen with y up. So the vertex goes to the
// screen as the reference computes it, is rounded there, and comes back in
// clip space on that grid, which Vulkan's finer one holds exactly. PICA clip
// space keeps z from -w to 0 and has y up; Vulkan's keeps z from 0 to w and
// has y down.
void emit_vertex(vec4 position, vec4 quaternion, vec4 color, vec4 texcoords, vec4 slots16, vec4 slots20) {
    gl_ClipDistance[0] = clip_enabled != 0u ? dot(position, clip_plane) : 1.0;
    if (position.w > 0.0) {
        float inverse_w = 1.0 / position.w;
        precise vec2 screen = vec2(pica_mul(pica_mul(position.x, inverse_w) + 1.0, viewport.x) + viewport.z,
                                   pica_mul(pica_mul(position.y, inverse_w) + 1.0, viewport.y) + viewport.w);
        vec2 snapped = round(screen * 16.0) / 16.0;
        precise vec2 normalised = (snapped - viewport.zw) / viewport.xy - 1.0;
        position.xy = normalised * position.w;
    }
    gl_Position = vec4(position.x, -position.y, -position.z, position.w);
    v_quaternion = quaternion;
    v_color = color;
    v_texcoords = texcoords;
    v_slots16 = slots16;
    v_slots20 = slots20;
}
)";

const std::string_view VertexShaderSource = R"(
// The vertices come in already shaded, as the rasterizer takes them: 24
// floats in six vec4s, the semantic slots of OutputVertex.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_quaternion;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec4 a_texcoords;
layout(location = 4) in vec4 a_slots16;
layout(location = 5) in vec4 a_slots20;

void main() {
    emit_vertex(a_position, a_quaternion, a_color, a_texcoords, a_slots16, a_slots20);
}
)";

const std::string_view ShadedVertexInputSource = R"(
// The vertex shader's sixteen input registers, as the attribute permutation
// filled them.
layout(location = 0) in vec4 a_inputs[16];

vec4 pica_float(uint index) {
    uint at = draw * DrawWords + FloatUniformBase + index * 4u;
    return uintBitsToFloat(uvec4(words[at], words[at + 1u], words[at + 2u], words[at + 3u]));
}

bool pica_bool(uint index) {
    return ((words[draw * DrawWords + BoolUniformWord] >> index) & 1u) != 0u;
}

uint pica_int(uint index) {
    return words[draw * DrawWords + IntUniformBase + index];
}
)";

const std::string_view ShadedVertexMainSource = R"(
// The program's outputs as MakeOutputVertex takes them: packed in register
// order by GPUREG_VSH_OUTMAP_MASK, each component to the semantic
// GPUREG_SH_OUTMAP_Oi names, a semantic no output gives 1.0, the colour made
// positive and kept to one.
void main() {
    pica_v = a_inputs;
    for (int at = 0; at < 16; ++at) {
        pica_r[at] = vec4(0.0);
        pica_o[at] = vec4(0.0);
    }
    pica_a = ivec3(0);
    pica_cmp = bvec2(false);
    pica_run(1048576u);
    float slots[32];
    for (int at = 0; at < 32; ++at) {
        slots[at] = 1.0;
    }
    uint mask = reg(0x2BDu);
    uint total = field(reg(0x04Fu), 0u, 3u);
    uint packed = 0u;
    for (uint output_ = 0u; output_ < 16u; ++output_) {
        if (field(mask, output_, 1u) == 0u) {
            continue;
        }
        if (packed < total) {
            uint map = reg(0x050u + packed);
            for (uint component = 0u; component < 4u; ++component) {
                slots[field(map, 8u * component, 5u)] = pica_o[output_][component];
            }
        }
        ++packed;
    }
    for (int channel = 8; channel < 12; ++channel) {
        float magnitude = abs(slots[channel]);
        slots[channel] = magnitude < 1.0 ? magnitude : 1.0;
    }
    emit_vertex(vec4(slots[0], slots[1], slots[2], slots[3]), vec4(slots[4], slots[5], slots[6], slots[7]),
                vec4(slots[8], slots[9], slots[10], slots[11]), vec4(slots[12], slots[13], slots[14], slots[15]),
                vec4(slots[16], slots[17], slots[18], slots[19]), vec4(slots[20], slots[21], slots[22], slots[23]));
}
)";

const std::string_view FragmentShaderSource = R"(
layout(location = 0) in vec4 v_quaternion;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec4 v_texcoords;
layout(location = 3) in vec4 v_slots16;
layout(location = 4) in vec4 v_slots20;

layout(location = 0) out vec4 o_color;

// Texels as red, green, blue and alpha bytes, low to high, row by row in
// stored order.
layout(std430, set = 0, binding = 1) readonly buffer Texels {
    uint texels[];
};

uvec4 color_register(uint value) {
    return uvec4(value & 0xFFu, (value >> 8u) & 0xFFu, (value >> 16u) & 0xFFu, value >> 24u);
}

int pica_trunc(float value) {
    return (isnan(value) || value >= 2147483648.0 || value < -2147483648.0) ? (-2147483647 - 1) : int(value);
}

uint truncate_unsigned(float value) {
    return (isnan(value) || value >= 9223372036854775808.0 || value < 0.0) ? 0u : uint(value);
}

// float1.5.10 and float1.7.12 as float32.
float narrow_float(uint bits, uint mantissa_bits, uint exponent_bits) {
    uint mantissa = bits & ((1u << mantissa_bits) - 1u);
    int exponent = int((bits >> mantissa_bits) & ((1u << exponent_bits) - 1u));
    uint sign = ((bits >> (mantissa_bits + exponent_bits)) & 1u) << 31u;
    int bias = (1 << (exponent_bits - 1u)) - 1;
    if (exponent == 0 && mantissa == 0u) {
        return uintBitsToFloat(sign);
    }
    if (exponent == int((1u << exponent_bits) - 1u)) {
        return uintBitsToFloat(sign | 0x7F800000u | (mantissa << (23u - mantissa_bits)));
    }
    if (exponent == 0) {
        exponent = 1;
        while ((mantissa & (1u << mantissa_bits)) == 0u) {
            mantissa <<= 1u;
            exponent -= 1;
        }
        mantissa &= (1u << mantissa_bits) - 1u;
    }
    return uintBitsToFloat(sign | (uint(exponent - bias + 127) << 23u) | (mantissa << (23u - mantissa_bits)));
}

// ---------------------------------------------------------------------------
// Texturing.

struct Texture {
    uint unit;
    uint width;
    uint height;
    uint wrap_s;
    uint wrap_t;
    uvec4 border;
};

const uint UnitBorder[3] = uint[3](0x081u, 0x091u, 0x099u);
const uint UnitDimensions[3] = uint[3](0x082u, 0x092u, 0x09Au);
const uint UnitParameters[3] = uint[3](0x083u, 0x093u, 0x09Bu);
const uint UnitLevels[3] = uint[3](0x084u, 0x094u, 0x09Cu);
const uint UnitAddress[3] = uint[3](0x085u, 0x095u, 0x09Du);

bool on_border(uint wrap, int value, uint size) {
    if (wrap == 1u) {
        return value < 0 || value >= int(size);
    }
    if (wrap == 5u) {
        return value >= int(size);
    }
    return false;
}

int wrapped(uint wrap, int value, uint size) {
    int last = int(size) - 1;
    if (wrap == 4u) {
        return value < 0 ? int(uint(value) % size) : clamp(value, 0, last);
    }
    if (wrap == 0u) {
        return clamp(value, 0, last);
    }
    if (wrap == 1u) {
        return value;
    }
    if (wrap == 3u) {
        uint coordinate = uint(value) % (2u * size);
        if (coordinate >= size) {
            coordinate = 2u * size - 1u - coordinate;
        }
        return int(coordinate);
    }
    return int(uint(value) % size);
}

uvec4 fetch(Texture texture, uint level, int s, int t) {
    if (on_border(texture.wrap_s, s, texture.width) || on_border(texture.wrap_t, t, texture.height)) {
        return texture.border;
    }
    int x = wrapped(texture.wrap_s, s, texture.width);
    int y = int(texture.height) - 1 - wrapped(texture.wrap_t, t, texture.height);
    if (x < 0 || y < 0 || x >= int(texture.width) || y >= int(texture.height)) {
        return uvec4(0u);
    }
    uint offset = words[draw * DrawWords + LevelBase + texture.unit * LevelsPerUnit + level];
    // A level that could not be decoded, its format unknown or its bytes
    // outside GPU memory, reads as zero, as the reference's does.
    if (offset == 0xFFFFFFFFu) {
        return uvec4(0u);
    }
    uint texel = texels[offset + uint(y) * texture.width + uint(x)];
    return uvec4(texel & 0xFFu, (texel >> 8u) & 0xFFu, (texel >> 16u) & 0xFFu, texel >> 24u);
}

Texture level_of(Texture base, uint level) {
    Texture texture = base;
    for (uint at = 0u; at < level; ++at) {
        texture.width = max(texture.width / 2u, 8u);
        texture.height = max(texture.height / 2u, 8u);
    }
    return texture;
}

uvec4 bilinear(Texture texture, uint level, vec2 uv) {
    float x = uv.x * float(texture.width) - 0.5;
    float y = uv.y * float(texture.height) - 0.5;
    float x0 = floor(x);
    float y0 = floor(y);
    float fx = x - x0;
    float fy = y - y0;
    int s = int(x0);
    int t = int(y0);
    vec4 a = vec4(fetch(texture, level, s, t));
    vec4 b = vec4(fetch(texture, level, s + 1, t));
    vec4 c = vec4(fetch(texture, level, s, t + 1));
    vec4 d = vec4(fetch(texture, level, s + 1, t + 1));
    precise vec4 top = a * (1.0 - fx) + b * fx;
    precise vec4 bottom = c * (1.0 - fx) + d * fx;
    precise vec4 mixed = top * (1.0 - fy) + bottom * fy;
    // lround: halves away from zero, and the values are not negative.
    return uvec4(floor(mixed + 0.5));
}

uvec4 nearest(Texture texture, uint level, vec2 uv) {
    return fetch(texture, level, int(floor(uv.x * float(texture.width))), int(floor(uv.y * float(texture.height))));
}

uvec4 sample_level(Texture base, uint level, bool linear, vec2 uv) {
    Texture texture = level_of(base, level);
    return linear ? bilinear(texture, level, uv) : nearest(texture, level, uv);
}

uvec4 sample_filtered(Texture base, uint parameters, uint levels, vec2 uv, vec2 du, vec2 dv) {
    float width = float(base.width);
    float height = float(base.height);
    float rate_x = length(vec2(du.x * width, dv.x * height));
    float rate_y = length(vec2(du.y * width, dv.y * height));
    float rate = max(rate_x, rate_y);
    float bias = float(signed_field(levels, 0u, 13u)) / 256.0;
    float least = float(field(levels, 24u, 4u));
    float greatest = float(field(levels, 16u, 4u));
    float lod = clamp((rate > 0.0 ? log2(rate) : -1.0e30) + bias, least, greatest);
    bool magnify = lod <= 0.0;
    bool linear = field(parameters, magnify ? 1u : 2u, 1u) != 0u;
    if (magnify) {
        return sample_level(base, 0u, linear, uv);
    }
    if (field(parameters, 24u, 1u) == 0u) {
        return sample_level(base, uint(floor(lod + 0.5)), linear, uv);
    }
    uint lower = uint(floor(lod));
    uint upper = min(lower + 1u, uint(greatest));
    float weight = lod - floor(lod);
    vec4 first = vec4(sample_level(base, lower, linear, uv));
    vec4 second = vec4(sample_level(base, upper, linear, uv));
    precise vec4 mixed = first * (1.0 - weight) + second * weight;
    return uvec4(floor(mixed + 0.5));
}

uvec4 textures[4];

void sample_textures(vec2 coordinates[3], vec2 du[3], vec2 dv[3], float texcoord0_w) {
    uint config = reg(0x080u);
    for (uint unit = 0u; unit < 4u; ++unit) {
        textures[unit] = uvec4(0u);
    }
    for (uint unit = 0u; unit < 3u; ++unit) {
        if (field(config, unit, 1u) == 0u) {
            continue;
        }
        uint parameters = reg(UnitParameters[unit]);
        if ((reg(UnitAddress[unit]) & 0x0FFFFFFFu) == 0u) {
            textures[unit] = uvec4(0u, 0u, 0u, 255u);
            continue;
        }
        uint source = (unit == 2u && field(config, 13u, 1u) != 0u) ? 1u : unit;
        vec2 uv = coordinates[source];
        if (unit == 0u) {
            uint type = field(parameters, 28u, 3u);
            if (type == 5u || (type != 0u && type != 3u)) {
                continue;
            }
            if (type == 3u) {
                uv = uv / texcoord0_w;
            }
        }
        uint dimensions = reg(UnitDimensions[unit]);
        Texture texture;
        texture.unit = unit;
        texture.width = field(dimensions, 16u, 11u);
        texture.height = field(dimensions, 0u, 11u);
        texture.wrap_s = field(parameters, 12u, 3u);
        texture.wrap_t = field(parameters, 8u, 3u);
        texture.border = color_register(reg(UnitBorder[unit]));
        if (texture.width == 0u || texture.height == 0u) {
            continue;
        }
        if (sampling == 1u) {
            textures[unit] = fetch(texture, 0u, pica_trunc(pica_mul(uv.x, float(texture.width))),
                                   pica_trunc(pica_mul(uv.y, float(texture.height))));
        } else {
            textures[unit] = sample_filtered(texture, parameters, reg(UnitLevels[unit]), uv, du[source], dv[source]);
        }
    }
}

// ---------------------------------------------------------------------------
// Fragment lighting.

vec3 light_color(uint value) {
    return vec3(float(field(value, 20u, 10u)) / 255.0, float(field(value, 10u, 10u)) / 255.0,
                float(field(value, 0u, 10u)) / 255.0);
}

vec3 rotate(vec4 q, vec3 v) {
    vec3 axis = q.xyz;
    return v + cross(axis, cross(axis, v) + v * q.w) * 2.0;
}

vec3 normalized(vec3 v) {
    float length_ = sqrt(dot(v, v));
    return v / length_;
}

bool lut_available(uint configuration, uint lut) {
    if (lut == 0u) return configuration != 1u;
    if (lut == 1u) return configuration != 0u && configuration != 1u && configuration != 5u;
    if (lut == 8u) return configuration != 2u && configuration != 3u;
    if (lut == 3u) return configuration != 0u && configuration != 2u && configuration != 4u;
    if (lut == 6u) return configuration != 3u;
    return configuration == 4u || configuration == 5u || configuration == 8u;
}

float look_up(uint lut, uint index, float delta) {
    uint entry = words[draw * DrawWords + LightingBase + lut * 256u + index];
    float difference = float(field(entry, 12u, 11u)) / 2047.0;
    return float(field(entry, 0u, 12u)) / 4095.0 + (field(entry, 23u, 1u) != 0u ? -difference : difference) * delta;
}

float lut_scale(uint scale) {
    if (scale <= 3u) return float(1u << scale);
    if (scale == 6u) return 0.25;
    if (scale == 7u) return 0.5;
    return 0.0;
}

uint light_register(uint number, uint offset) {
    return reg(0x140u + 0x10u * number + offset);
}

vec3 normal;
vec3 tangent;
vec4 shadow;

float lut_input(uint select, uint number, vec3 light, vec3 view, vec3 half_) {
    if (select == 0u) return dot(normal, normalized(half_));
    if (select == 1u) return dot(view, normalized(half_));
    if (select == 2u) return dot(normal, view);
    if (select == 3u) return dot(light, normal);
    if (select == 4u) {
        uint low = light_register(number, 6u);
        vec3 spot = vec3(float(signed_field(low, 0u, 13u)) / 2047.0, float(signed_field(low, 16u, 13u)) / 2047.0,
                         float(signed_field(light_register(number, 7u), 0u, 13u)) / 2047.0);
        return dot(light, spot);
    }
    if (select == 5u && field(reg(0x1C3u), 4u, 4u) == 8u) {
        vec3 h = normalized(half_);
        return dot(h + normal * -dot(normal, h), tangent);
    }
    return 0.0;
}

float lut_value(uint lut_field, uint table, uint number, vec3 light, vec3 view, vec3 half_) {
    float input_ = lut_input(field(reg(0x1D1u), 4u * lut_field, 3u), number, light, view, half_);
    uint index;
    float delta;
    if (field(reg(0x1D0u), 4u * lut_field + 1u, 1u) == 0u) {
        input_ = field(light_register(number, 9u), 1u, 1u) != 0u ? abs(input_) : max(input_, 0.0);
        index = truncate_unsigned(clamp(floor(input_ * 256.0), 0.0, 255.0));
        delta = input_ * 256.0 - float(index);
    } else {
        int signed_index = pica_trunc(clamp(floor(input_ * 128.0), -128.0, 127.0));
        delta = input_ * 128.0 - float(signed_index);
        index = uint(signed_index) & 0xFFu;
    }
    return lut_scale(field(reg(0x1D2u), 4u * lut_field, 3u)) * look_up(table, index, delta);
}

bool lut_enabled(uint lut) {
    return field(reg(0x1C4u), 16u + lut, 1u) == 0u && lut_available(field(reg(0x1C3u), 4u, 4u), lut);
}

void light_fragment(vec4 quaternion, vec3 view_vector, out uvec4 primary, out uvec4 secondary) {
    uint config0 = reg(0x1C3u);
    uint config1 = reg(0x1C4u);
    uint configuration = field(config0, 4u, 4u);
    shadow = vec4(1.0);
    if (field(config0, 0u, 1u) != 0u) {
        shadow = vec4(textures[field(config0, 24u, 2u)]) / 255.0;
        if (field(config0, 18u, 1u) != 0u) {
            shadow = vec4(1.0) - shadow;
        }
    }
    vec3 surface_normal = vec3(0.0, 0.0, 1.0);
    vec3 surface_tangent = vec3(1.0, 0.0, 0.0);
    uint bump_mode = field(config0, 28u, 2u);
    if (bump_mode != 0u) {
        uvec4 bump = textures[field(config0, 22u, 2u)];
        vec3 perturbation = vec3(bump.rgb) / 127.5 - vec3(1.0);
        if (bump_mode == 1u && field(config0, 30u, 1u) == 0u) {
            perturbation.z = sqrt(max(1.0 - (perturbation.x * perturbation.x + perturbation.y * perturbation.y), 0.0));
        }
        if (bump_mode == 1u) {
            surface_normal = perturbation;
        } else if (bump_mode == 2u) {
            surface_tangent = perturbation;
        } else {
            surface_normal = vec3(0.0);
            surface_tangent = vec3(0.0);
        }
    }
    float length_ = sqrt(quaternion.x * quaternion.x + quaternion.y * quaternion.y + quaternion.z * quaternion.z +
                         quaternion.w * quaternion.w);
    vec4 unit_quaternion = quaternion / length_;
    normal = rotate(unit_quaternion, surface_normal);
    tangent = rotate(unit_quaternion, surface_tangent);

    vec4 diffuse_sum = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 specular_sum = vec4(0.0, 0.0, 0.0, 1.0);
    uint last = field(reg(0x1C2u), 0u, 3u);
    for (uint slot = 0u; slot <= last; ++slot) {
        uint number = field(reg(0x1D9u), 4u * slot, 3u);
        uint light_config = light_register(number, 9u);
        uint vector_low = light_register(number, 4u);
        vec3 position = vec3(narrow_float(field(vector_low, 0u, 16u), 10u, 5u),
                             narrow_float(field(vector_low, 16u, 16u), 10u, 5u),
                             narrow_float(field(light_register(number, 5u), 0u, 16u), 10u, 5u));
        vec3 light = field(light_config, 0u, 1u) != 0u ? position : position + view_vector;
        float distance_ = sqrt(dot(light, light));
        light = normalized(light);
        vec3 view = normalized(view_vector);
        vec3 half_ = view + light;

        float distance_attenuation = 1.0;
        if (field(config1, 24u + number, 1u) == 0u) {
            float scale = narrow_float(field(light_register(number, 0xBu), 0u, 20u), 12u, 7u);
            float bias = narrow_float(field(light_register(number, 0xAu), 0u, 20u), 12u, 7u);
            float sample_ = clamp(scale * distance_ + bias, 0.0, 1.0);
            uint index = truncate_unsigned(clamp(floor(sample_ * 256.0), 0.0, 255.0));
            distance_attenuation = look_up(16u + number, index, sample_ * 256.0 - float(index));
        }
        float spot_attenuation = 1.0;
        if (field(config1, 8u + number, 1u) == 0u && lut_available(configuration, 8u)) {
            spot_attenuation = lut_value(2u, 8u + number, number, light, view, half_);
        }
        vec3 reflection;
        reflection.x = lut_enabled(6u) ? lut_value(6u, 6u, number, light, view, half_) : 1.0;
        reflection.y = lut_enabled(5u) ? lut_value(5u, 5u, number, light, view, half_) : reflection.x;
        reflection.z = lut_enabled(4u) ? lut_value(4u, 4u, number, light, view, half_) : reflection.x;
        float d0 = lut_enabled(0u) ? lut_value(0u, 0u, number, light, view, half_) : 1.0;
        float d1 = lut_enabled(1u) ? lut_value(1u, 1u, number, light, view, half_) : 1.0;
        vec3 specular_0 = light_color(light_register(number, 0u)) * d0;
        vec3 specular_1 = reflection * d1 * light_color(light_register(number, 1u));
        if (slot == last && lut_enabled(3u)) {
            float fresnel = lut_value(3u, 3u, number, light, view, half_);
            if (field(config0, 2u, 1u) != 0u) diffuse_sum.w = fresnel;
            if (field(config0, 3u, 1u) != 0u) specular_sum.w = fresnel;
        }
        float dot_product = dot(light, normal);
        dot_product = field(light_config, 1u, 1u) != 0u ? abs(dot_product) : max(dot_product, 0.0);
        float clamp_highlights = (field(config0, 27u, 1u) != 0u && dot_product == 0.0) ? 0.0 : 1.0;
        if (field(light_config, 2u, 2u) != 0u) {
            float half_length = dot(half_, half_);
            float geometric = half_length == 0.0 ? 0.0 : min(dot_product / half_length, 1.0);
            if (field(light_config, 2u, 1u) != 0u) specular_0 *= geometric;
            if (field(light_config, 3u, 1u) != 0u) specular_1 *= geometric;
        }
        bool shadowed = field(config1, number, 1u) == 0u;
        vec3 primary_shadow = (field(config0, 16u, 1u) != 0u && shadowed) ? shadow.rgb : vec3(1.0);
        vec3 secondary_shadow = (field(config0, 17u, 1u) != 0u && shadowed) ? shadow.rgb : vec3(1.0);
        vec3 diffuse = ((light_color(light_register(number, 2u)) * dot_product) * primary_shadow +
                        light_color(light_register(number, 3u))) * distance_attenuation * spot_attenuation;
        vec3 specular = (specular_0 + specular_1) * clamp_highlights * distance_attenuation * spot_attenuation *
                        secondary_shadow;
        diffuse_sum.rgb += diffuse;
        specular_sum.rgb += specular;
    }
    if (field(config0, 19u, 1u) != 0u) {
        if (field(config0, 2u, 1u) != 0u) diffuse_sum.w *= shadow.w;
        if (field(config0, 3u, 1u) != 0u) specular_sum.w *= shadow.w;
    }
    diffuse_sum.rgb += light_color(reg(0x1C0u));
    primary = uvec4(truncate_unsigned(clamp(diffuse_sum.r, 0.0, 1.0) * 255.0),
                    truncate_unsigned(clamp(diffuse_sum.g, 0.0, 1.0) * 255.0),
                    truncate_unsigned(clamp(diffuse_sum.b, 0.0, 1.0) * 255.0),
                    truncate_unsigned(clamp(diffuse_sum.a, 0.0, 1.0) * 255.0)) & uvec4(0xFFu);
    secondary = uvec4(truncate_unsigned(clamp(specular_sum.r, 0.0, 1.0) * 255.0),
                      truncate_unsigned(clamp(specular_sum.g, 0.0, 1.0) * 255.0),
                      truncate_unsigned(clamp(specular_sum.b, 0.0, 1.0) * 255.0),
                      truncate_unsigned(clamp(specular_sum.a, 0.0, 1.0) * 255.0)) & uvec4(0xFFu);
}

// ---------------------------------------------------------------------------
// The texture combiners, in the integers the reference uses.

const uint StageBases[6] = uint[6](0x0C0u, 0x0C8u, 0x0D0u, 0x0D8u, 0x0F0u, 0x0F8u);

uvec4 stage_source(uint which, uvec4 primary, uvec4 lit[2], uvec4 buffer_, uvec4 constant, uvec4 previous) {
    if (which == 0x0u) return primary;
    if (which == 0x1u) return lit[0];
    if (which == 0x2u) return lit[1];
    if (which >= 0x3u && which <= 0x6u) return textures[which - 0x3u];
    if (which == 0xDu) return buffer_;
    if (which == 0xEu) return constant;
    if (which == 0xFu) return previous;
    return uvec4(0u);
}

uvec3 color_modifier(uint modifier, uvec4 c) {
    if (modifier == 0x0u) return c.rgb;
    if (modifier == 0x1u) return uvec3(255u) - c.rgb;
    if (modifier == 0x2u) return uvec3(c.a);
    if (modifier == 0x3u) return uvec3(255u - c.a);
    if (modifier == 0x4u) return uvec3(c.r);
    if (modifier == 0x5u) return uvec3(255u - c.r);
    if (modifier == 0x8u) return uvec3(c.g);
    if (modifier == 0x9u) return uvec3(255u - c.g);
    if (modifier == 0xCu) return uvec3(c.b);
    if (modifier == 0xDu) return uvec3(255u - c.b);
    return uvec3(0u);
}

uint alpha_modifier(uint modifier, uvec4 c) {
    uint which = (modifier >> 1u) & 3u;
    uint channel = which == 0u ? c.a : (which == 1u ? c.r : (which == 2u ? c.g : c.b));
    return (modifier & 1u) != 0u ? 255u - channel : channel;
}

uint combine_channel(uint operation, uint a, uint b, uint c) {
    if (operation == 0u) return a;
    if (operation == 1u) return (a * b) / 255u;
    if (operation == 2u) return min(a + b, 255u);
    if (operation == 3u) return uint(clamp(int(a + b) - 128, 0, 255));
    if (operation == 4u) return (a * c + b * (255u - c)) / 255u;
    if (operation == 5u) return a > b ? a - b : 0u;
    if (operation == 8u) return min((a * b + 255u * c) / 255u, 255u);
    if (operation == 9u) return (min(a + b, 255u) * c) / 255u;
    return 0u;
}

uint dot3(uvec3 inputs[3]) {
    int sum = 0;
    for (int channel = 0; channel < 3; ++channel) {
        int a = int(inputs[0][channel] * 2u) - 255;
        int b = int(inputs[1][channel] * 2u) - 255;
        sum += (a * b + 128) / 256;
    }
    return uint(clamp(sum, 0, 255));
}

uvec4 combine(uvec4 primary, uvec4 lit[2]) {
    uvec4 previous = uvec4(0u);
    uvec4 buffer_ = uvec4(0u);
    uvec4 next_buffer = color_register(reg(0x0FDu));
    uint update = reg(0x0E0u);
    for (uint stage = 0u; stage < 6u; ++stage) {
        uint base = StageBases[stage];
        uint sources = reg(base);
        uint operands = reg(base + 1u);
        uint operations = reg(base + 2u);
        uvec4 constant = color_register(reg(base + 3u));
        uint scales = reg(base + 4u);
        uint color_sources[3] = uint[3](field(sources, 0u, 4u), field(sources, 4u, 4u), field(sources, 8u, 4u));
        if (stage == 0u) {
            for (int at = 0; at < 2; ++at) {
                if (color_sources[at] == 0xFu) color_sources[at] = color_sources[2];
            }
        }
        uvec3 color_inputs[3];
        uint alpha_inputs[3];
        for (uint at = 0u; at < 3u; ++at) {
            color_inputs[at] = color_modifier(field(operands, 4u * at, 4u),
                                              stage_source(color_sources[at], primary, lit, buffer_, constant, previous));
            alpha_inputs[at] = alpha_modifier(field(operands, 12u + 4u * at, 3u),
                                              stage_source(field(sources, 16u + 4u * at, 4u), primary, lit, buffer_,
                                                           constant, previous));
        }
        uint color_operation = field(operations, 0u, 4u);
        uvec3 color;
        if (color_operation == 6u || color_operation == 7u) {
            color = uvec3(dot3(color_inputs));
        } else {
            for (int channel = 0; channel < 3; ++channel) {
                color[channel] = combine_channel(color_operation, color_inputs[0][channel], color_inputs[1][channel],
                                                 color_inputs[2][channel]);
            }
        }
        uint alpha = color_operation == 7u ? color.r
                                            : combine_channel(field(operations, 16u, 4u), alpha_inputs[0],
                                                              alpha_inputs[1], alpha_inputs[2]);
        uint color_scale = field(scales, 0u, 2u) < 3u ? (1u << field(scales, 0u, 2u)) : 1u;
        uint alpha_scale = field(scales, 16u, 2u) < 3u ? (1u << field(scales, 16u, 2u)) : 1u;
        previous = uvec4(min(color * color_scale, uvec3(255u)), min(alpha * alpha_scale, 255u));
        buffer_ = next_buffer;
        if (stage < 4u && field(update, 8u + stage, 1u) != 0u) next_buffer.rgb = previous.rgb;
        if (stage < 4u && field(update, 12u + stage, 1u) != 0u) next_buffer.a = previous.a;
    }
    return previous;
}

// ---------------------------------------------------------------------------
// Fog, the alpha test and the scissor.

uvec4 apply_fog(float depth, uvec4 color) {
    uint update = reg(0x0E0u);
    if (field(update, 0u, 3u) != 5u) {
        return color;
    }
    float index = (field(update, 16u, 1u) != 0u ? 1.0 - depth : depth) * 128.0;
    float whole = clamp(floor(index), 0.0, 127.0);
    uint entry = words[draw * DrawWords + FogBase + truncate_unsigned(whole)];
    float difference = float(signed_field(entry, 0u, 13u));
    float factor = clamp(float(field(entry, 13u, 11u)) / 2047.0 + (difference / 2047.0) * (index - whole), 0.0, 1.0);
    uvec4 fog = color_register(reg(0x0E1u));
    for (int channel = 0; channel < 3; ++channel) {
        color[channel] = uint(pica_trunc(factor * float(color[channel]) + (1.0 - factor) * float(fog[channel]))) & 0xFFu;
    }
    return color;
}

bool compare(uint function, uint left, uint right) {
    if (function == 0u) return false;
    if (function == 1u) return true;
    if (function == 2u) return left == right;
    if (function == 3u) return left != right;
    if (function == 4u) return left < right;
    if (function == 5u) return left <= right;
    if (function == 6u) return left > right;
    return left >= right;
}

// GPUREG_SCISSORTEST_*, in the rasterizer's sixteenths: a pixel centre
// inside the box or out of it.
bool scissored(uvec2 pixel) {
    uint mode = field(reg(0x065u), 0u, 2u);
    if (mode != 1u && mode != 3u) {
        return false;
    }
    uint first = reg(0x066u);
    uint last = reg(0x067u);
    uint x = pixel.x * 16u + 8u;
    uint y = pixel.y * 16u + 8u;
    bool inside = x >= field(first, 0u, 10u) * 16u && x < (field(last, 0u, 10u) + 1u) * 16u &&
                  y >= field(first, 16u, 10u) * 16u && y < (field(last, 16u, 10u) + 1u) * 16u;
    return mode == 1u ? inside : !inside;
}

void main() {
    // The pixel counted from the bottom, as the PICA counts.
    uvec2 pixel = uvec2(uint(gl_FragCoord.x), height - 1u - uint(gl_FragCoord.y));
    if (scissored(pixel)) {
        discard;
    }
    // z over w, linear on the screen, then the depth map; a W-buffer
    // multiplies w back in.
    float z_over_w = -gl_FragCoord.z;
    float depth = z_over_w * narrow_float(field(reg(0x04Du), 0u, 24u), 16u, 7u) +
                  narrow_float(field(reg(0x04Eu), 0u, 24u), 16u, 7u);
    if (field(reg(0x06Du), 0u, 1u) == 0u) {
        depth *= 1.0 / gl_FragCoord.w;
    }
    depth = clamp(depth, 0.0, 1.0);

    uvec4 primary = uvec4(pica_trunc(floor(v_color.r * 255.0 + 0.5)), pica_trunc(floor(v_color.g * 255.0 + 0.5)),
                          pica_trunc(floor(v_color.b * 255.0 + 0.5)), pica_trunc(floor(v_color.a * 255.0 + 0.5))) &
                    uvec4(0xFFu);
    vec2 coordinates[3] = vec2[3](v_texcoords.xy, v_texcoords.zw, v_slots20.zw);
    vec2 du[3] = vec2[3](dFdxFine(coordinates[0]), dFdxFine(coordinates[1]), dFdxFine(coordinates[2]));
    vec2 dv[3] = vec2[3](dFdyFine(coordinates[0]), dFdyFine(coordinates[1]), dFdyFine(coordinates[2]));
    // Rates as (d/dx, d/dy) for u and for v.
    vec2 rate_u[3] = vec2[3](vec2(du[0].x, dv[0].x), vec2(du[1].x, dv[1].x), vec2(du[2].x, dv[2].x));
    vec2 rate_v[3] = vec2[3](vec2(du[0].y, dv[0].y), vec2(du[1].y, dv[1].y), vec2(du[2].y, dv[2].y));
    sample_textures(coordinates, rate_u, rate_v, v_slots16.x);

    uvec4 lit[2] = uvec4[2](uvec4(0u), uvec4(0u));
    if (field(reg(0x1C6u), 0u, 1u) == 0u) {
        light_fragment(v_quaternion, vec3(v_slots16.z, v_slots16.w, v_slots20.x), lit[0], lit[1]);
    }
    uvec4 color = combine(primary, lit);
    uint alpha_test = reg(0x104u);
    if (field(alpha_test, 0u, 1u) != 0u && !compare(field(alpha_test, 4u, 3u), color.a, field(alpha_test, 8u, 8u))) {
        discard;
    }
    color = apply_fog(depth, color);

    // The depth the buffer will hold, so that the test compares what the
    // reference compares: 16 or 24 bits.
    float maximum = field(reg(0x116u), 0u, 2u) == 0u ? 65535.0 : 16777215.0;
    gl_FragDepth = float(truncate_unsigned(depth * maximum)) / maximum;
    o_color = vec4(color) / 255.0;
}
)";

}  // namespace hexlith::adapters::vulkan
