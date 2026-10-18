#ifndef HEXLITH_DOMAIN_GPU_SHADER_ARITHMETIC_H
#define HEXLITH_DOMAIN_GPU_SHADER_ARITHMETIC_H

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace hexlith::domain::gpu {

// The arithmetic of a PICA shader instruction, one operation each, as ADR
// 0018 decides it: float32 with the behaviours measured on hardware that
// Azahar keeps. The interpreter calls these, and the GLSL the Vulkan backend
// generates spells out the same rules.

// A product. Zero times infinity is zero, not a NaN: the PICA gives zero when
// a product is a NaN and neither factor was one. Source: Float::operator* in
// Azahar's pica_types.h, which says so of the hardware.
inline float ShaderMultiply(float left, float right) {
    const float product = left * right;
    if (std::isnan(product) && !std::isnan(left) && !std::isnan(right)) {
        return 0.0F;
    }
    return product;
}

// The first operand when it is the greater, the second otherwise, so that a
// NaN first loses and a NaN second wins. Source: Azahar's interpreter, which
// says the form is required to match the hardware.
inline float ShaderMax(float first, float second) {
    return first > second ? first : second;
}

inline float ShaderMin(float first, float second) {
    return first < second ? first : second;
}

// MOVA's conversion: towards zero, with a NaN or a value no 32-bit integer
// holds becoming the most negative integer, which is what the x86
// conversion Azahar compiles to gives.
inline std::int32_t ShaderTruncate(float value) {
    constexpr float Limit = 2147483648.0F;
    if (std::isnan(value) || value >= Limit || value < -Limit) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(value);
}

// The nearest float24 to a float32, for ShaderPrecision::Float24: sixteen
// bits of mantissa rounded to nearest even, the exponent range of a bias of
// 63, denormals flushed to a zero of the same sign, and a value too large
// rounding to infinity. Source: GBATEK, float1.7.16; the flushing is
// Azahar's note that the PICA200 flushes denormals.
inline float RoundToFloat24(float value) {
    constexpr unsigned DroppedBits = 23 - 16;
    constexpr std::uint32_t SignBit = 0x80000000;
    constexpr std::uint32_t Infinity = 0x7F800000;
    // Float32 exponents a float24 can hold, biased: 2^-62 to 2^63.
    constexpr std::uint32_t SmallestNormal = (127U - 62U) << 23U;
    constexpr std::uint32_t LargestFinite = ((127U + 63U) << 23U) | 0x007FFF80;

    if (std::isnan(value)) {
        return value;
    }
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = bits & SignBit;
    std::uint32_t magnitude = bits & ~SignBit;
    if (magnitude >= Infinity) {
        return value;
    }
    const std::uint32_t half = 1U << (DroppedBits - 1);
    const std::uint32_t odd = (magnitude >> DroppedBits) & 1U;
    magnitude = (magnitude + half - 1 + odd) & ~((1U << DroppedBits) - 1);
    if (magnitude < SmallestNormal) {
        return std::bit_cast<float>(sign);
    }
    if (magnitude > LargestFinite) {
        return std::bit_cast<float>(sign | Infinity);
    }
    return std::bit_cast<float>(sign | magnitude);
}

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_SHADER_ARITHMETIC_H
