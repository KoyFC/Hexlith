#ifndef HEXLITH_DOMAIN_GPU_FLOAT24_H
#define HEXLITH_DOMAIN_GPU_FLOAT24_H

#include <bit>
#include <cstdint>
#include <utility>

namespace hexlith::domain::gpu {

// The PICA200's 24-bit float, float1.7.16: a sign bit, seven bits of
// exponent with a bias of 63, and sixteen of mantissa (GBATEK, 3DS GPU
// Internal Register Overview, Data Types). Every float24 value has an exact
// float32, so the conversion loses nothing: a denormal stays a denormal's
// value, and an exponent of all ones is an infinity or a NaN with the
// mantissa kept. What the hardware does with denormals in arithmetic is the
// shader interpreter's business, not the conversion's.
constexpr float Float24ToFloat32(std::uint32_t bits) {
    constexpr unsigned MantissaBits = 16;
    constexpr std::uint32_t MantissaMask = (1U << MantissaBits) - 1;
    constexpr std::uint32_t ExponentMask = 0x7F;
    constexpr std::int32_t Float24Bias = 63;
    constexpr std::int32_t Float32Bias = 127;
    constexpr unsigned Float32MantissaBits = 23;
    constexpr std::uint32_t Float32ExponentAllOnes = 0xFF;

    const std::uint32_t sign = ((bits >> 23) & 1U) << 31;
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
    const auto biased = static_cast<std::uint32_t>(exponent - Float24Bias + Float32Bias);
    return std::bit_cast<float>(sign | (biased << Float32MantissaBits) |
                                (mantissa << (Float32MantissaBits - MantissaBits)));
}

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_FLOAT24_H
