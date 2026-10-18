#ifndef HEXLITH_DOMAIN_GPU_FLOAT24_H
#define HEXLITH_DOMAIN_GPU_FLOAT24_H

#include <bit>
#include <cstdint>
#include <utility>

namespace hexlith::domain::gpu {

// The PICA200's narrow floats: a sign bit, an exponent of `ExponentBits`
// with a bias of half its range, and `MantissaBits` of mantissa. GBATEK,
// 3DS GPU Internal Register Overview, Data Types, names float1.5.10,
// float1.7.12 and float1.7.16. Every value of each has an exact float32, so
// the conversion loses nothing: a denormal keeps its value, and an exponent
// of all ones is an infinity or a NaN with its mantissa kept. What the
// hardware does with denormals in arithmetic is the shader interpreter's
// business, not the conversion's.
template<unsigned MantissaBits, unsigned ExponentBits>
constexpr float NarrowFloatToFloat32(std::uint32_t bits) {
    constexpr std::uint32_t MantissaMask = (1U << MantissaBits) - 1;
    constexpr std::uint32_t ExponentMask = (1U << ExponentBits) - 1;
    constexpr auto Bias = static_cast<std::int32_t>((1U << (ExponentBits - 1)) - 1);
    constexpr std::int32_t Float32Bias = 127;
    constexpr unsigned Float32MantissaBits = 23;
    constexpr std::uint32_t Float32ExponentAllOnes = 0xFF;

    const std::uint32_t sign = ((bits >> (MantissaBits + ExponentBits)) & 1U) << 31;
    std::uint32_t mantissa = bits & MantissaMask;
    auto exponent = static_cast<std::int32_t>((bits >> MantissaBits) & ExponentMask);

    if (exponent == 0 && mantissa == 0) {
        return std::bit_cast<float>(sign);
    }
    if (std::cmp_equal(exponent, ExponentMask)) {
        return std::bit_cast<float>(sign | (Float32ExponentAllOnes << Float32MantissaBits) |
                                    (mantissa << (Float32MantissaBits - MantissaBits)));
    }
    if (exponent == 0) {
        // A denormal: shift the mantissa up to a leading one, which float32's
        // wider exponent can always represent.
        exponent = 1;
        while ((mantissa & (1U << MantissaBits)) == 0) {
            mantissa <<= 1U;
            --exponent;
        }
        mantissa &= MantissaMask;
    }
    const auto biased = static_cast<std::uint32_t>(exponent - Bias + Float32Bias);
    return std::bit_cast<float>(sign | (biased << Float32MantissaBits) |
                                (mantissa << (Float32MantissaBits - MantissaBits)));
}

// float1.7.16: vertex attributes, uniforms, viewport and depth registers.
constexpr float Float24ToFloat32(std::uint32_t bits) {
    return NarrowFloatToFloat32<16, 7>(bits);
}

// float1.7.12: a light's distance attenuation bias and scale.
constexpr float Float20ToFloat32(std::uint32_t bits) {
    return NarrowFloatToFloat32<12, 7>(bits);
}

// float1.5.10: a light's position or direction.
constexpr float Float16ToFloat32(std::uint32_t bits) {
    return NarrowFloatToFloat32<10, 5>(bits);
}

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_FLOAT24_H
