#ifndef HEXLITH_DOMAIN_ARM_PRIMITIVES_VFP_H
#define HEXLITH_DOMAIN_ARM_PRIMITIVES_VFP_H

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

// The floating point half of the shared primitive library: VFPv2 arithmetic,
// comparison and conversion, on the host's own floating point.
//
// ADR 0010 records the choice. The host's IEEE arithmetic gives the same
// correctly rounded result as the ARM11 for the operations VFPv2 has, in round
// to nearest, which is what the game uses. What differs is around the edges, and
// those edges are handled here explicitly rather than left to the host:
//
// - Which NaN comes out. ARM prefers a signalling NaN operand over a quiet one
//   and the first operand over the second, and an invalid operation produces
//   the default NaN, 0x7FC00000. x86 produces 0xFFC00000 and has its own rules.
// - Flush to zero, the FZ bit: a denormal operand is read as zero and a
//   denormal result is written as zero, keeping the sign.
// - Default NaN, the DN bit: every NaN result is the default NaN.
//
// What is not handled is refused instead: a rounding mode other than to
// nearest, the short vector mode of VFPv2, and enabled exception traps. A
// caller asks IsSupportedFloatMode before it runs anything and stops if the
// answer is no, which turns a silently different result into a visible stop.
//
// The cumulative exception flags in FPSCR are not modelled: an operation here
// never sets IOC, DZC, OFC, UFC, IXC or IDC. Code that reads them back would see
// what was last written. ADR 0010 records this as a known gap.
//
// Source for every rule: the pseudocode functions FPUnpack, FPProcessNaNs,
// FPRound, FPCompare and FPToFixed of the ARMv7-A and ARMv7-R reference manual
// (DDI 0406C), sections A2.7 and B6, which describe VFPv2 as well.
//
// Values travel as bit patterns, uint32 for single precision and uint64 for
// double, because that is how they sit in the register file and because a NaN's
// payload has to survive the trip unchanged.
namespace hexlith::domain::arm::primitives {

namespace fpscr {
constexpr unsigned NegativeBit = 31;
constexpr unsigned ZeroBit = 30;
constexpr unsigned CarryBit = 29;
constexpr unsigned OverflowBit = 28;
constexpr unsigned FlagsShift = 28;
constexpr std::uint32_t FlagsMask = 0xF0000000;
constexpr unsigned DefaultNanBit = 25;
constexpr unsigned FlushToZeroBit = 24;
constexpr unsigned RoundingModeShift = 22;
constexpr std::uint32_t RoundingModeMask = 0b11;

// STRIDE in bits 21 and 20 and LEN in bits 18 to 16, the short vector mode.
constexpr std::uint32_t VectorMask = 0x00370000;

// IDE, IXE, UFE, OFE, DZE and IOE: the exception trap enables.
constexpr std::uint32_t TrapEnableMask = 0x00009F00;

// The comparison results, already in place in bits 31 to 28.
constexpr std::uint32_t Less = 0x80000000;
constexpr std::uint32_t Equal = 0x60000000;
constexpr std::uint32_t Greater = 0x20000000;
constexpr std::uint32_t Unordered = 0x30000000;
}  // namespace fpscr

// Whether everything this library does is defined for this FPSCR value.
// Round to nearest, no short vectors, no traps. FZ and DN may be either.
[[nodiscard]] constexpr bool IsSupportedFloatMode(std::uint32_t value) {
    const std::uint32_t rounding = (value >> fpscr::RoundingModeShift) & fpscr::RoundingModeMask;
    return rounding == 0 && (value & fpscr::VectorMask) == 0 &&
           (value & fpscr::TrapEnableMask) == 0;
}

// The layout of one precision. Single and double differ only in these numbers,
// so every function below is written once for both.
template<class Float>
struct FloatFormat;

template<>
struct FloatFormat<float> {
    using Bits = std::uint32_t;
    static constexpr unsigned FractionWidth = 23;
    static constexpr Bits SignMask = 0x80000000;
    static constexpr Bits ExponentMask = 0x7F800000;
    static constexpr Bits FractionMask = 0x007FFFFF;
    static constexpr Bits QuietBit = 0x00400000;
    static constexpr Bits DefaultNan = 0x7FC00000;
};

template<>
struct FloatFormat<double> {
    using Bits = std::uint64_t;
    static constexpr unsigned FractionWidth = 52;
    static constexpr Bits SignMask = 0x8000000000000000;
    static constexpr Bits ExponentMask = 0x7FF0000000000000;
    static constexpr Bits FractionMask = 0x000FFFFFFFFFFFFF;
    static constexpr Bits QuietBit = 0x0008000000000000;
    static constexpr Bits DefaultNan = 0x7FF8000000000000;
};

template<class Float>
using FloatBits = FloatFormat<Float>::Bits;

namespace vfp_detail {

template<class Float>
constexpr bool IsNan(FloatBits<Float> value) {
    using Format = FloatFormat<Float>;
    return (value & Format::ExponentMask) == Format::ExponentMask &&
           (value & Format::FractionMask) != 0;
}

template<class Float>
constexpr bool IsSignallingNan(FloatBits<Float> value) {
    return IsNan<Float>(value) && (value & FloatFormat<Float>::QuietBit) == 0;
}

template<class Float>
constexpr bool IsDenormal(FloatBits<Float> value) {
    using Format = FloatFormat<Float>;
    return (value & Format::ExponentMask) == 0 && (value & Format::FractionMask) != 0;
}

constexpr bool FlushToZero(std::uint32_t fpscr_value) {
    return ((fpscr_value >> fpscr::FlushToZeroBit) & 1U) != 0;
}

constexpr bool DefaultNan(std::uint32_t fpscr_value) {
    return ((fpscr_value >> fpscr::DefaultNanBit) & 1U) != 0;
}

// FPUnpack's flush: with FZ set, a denormal operand is read as a zero of the
// same sign.
template<class Float>
constexpr FloatBits<Float> FlushInput(FloatBits<Float> value, std::uint32_t fpscr_value) {
    if (FlushToZero(fpscr_value) && IsDenormal<Float>(value)) {
        return value & FloatFormat<Float>::SignMask;
    }
    return value;
}

// FPRound's flush, applied to a result: with FZ set, a denormal result is
// written as a zero of the same sign.
//
// The reference manual flushes on the exact result before rounding, so a value
// just below the smallest normal number that would round up to it is flushed
// there and not here. That is the one place this differs, and it needs a result
// within half a unit in the last place of the smallest normal number.
template<class Float>
constexpr FloatBits<Float> FlushOutput(FloatBits<Float> value, std::uint32_t fpscr_value) {
    return FlushInput<Float>(value, fpscr_value);
}

template<class Float>
constexpr FloatBits<Float> Quiet(FloatBits<Float> value, std::uint32_t fpscr_value) {
    if (DefaultNan(fpscr_value)) {
        return FloatFormat<Float>::DefaultNan;
    }
    return value | FloatFormat<Float>::QuietBit;
}

// FPProcessNaN for one operand, when it is a NaN.
template<class Float>
constexpr FloatBits<Float> ProcessNan(FloatBits<Float> value, std::uint32_t fpscr_value) {
    return Quiet<Float>(value, fpscr_value);
}

// FPProcessNaNs: a signalling NaN in either operand wins over a quiet one, and
// the first operand wins a tie. Returns false when neither is a NaN.
template<class Float>
constexpr bool ProcessNans(FloatBits<Float> first,
                           FloatBits<Float> second,
                           std::uint32_t fpscr_value,
                           FloatBits<Float>& result) {
    const bool first_signals = IsSignallingNan<Float>(first);
    const bool second_signals = IsSignallingNan<Float>(second);
    if (!first_signals && !second_signals && !IsNan<Float>(first) && !IsNan<Float>(second)) {
        return false;
    }

    // The first operand wins unless only the second one signals, or unless the
    // first is not a NaN at all.
    const bool take_second = (second_signals && !first_signals) || !IsNan<Float>(first);
    result = ProcessNan<Float>(take_second ? second : first, fpscr_value);
    return true;
}

// A result the host computed from operands that were not NaNs. If it is a NaN
// the operation was invalid, and ARM answers the default NaN where the host
// answered its own.
template<class Float>
constexpr FloatBits<Float> Finish(Float host_result, std::uint32_t fpscr_value) {
    const auto bits = std::bit_cast<FloatBits<Float>>(host_result);
    if (IsNan<Float>(bits)) {
        return FloatFormat<Float>::DefaultNan;
    }
    return FlushOutput<Float>(bits, fpscr_value);
}

enum class Arithmetic : std::uint8_t { Add, Subtract, Multiply, Divide };

template<class Float>
constexpr FloatBits<Float> Binary(Arithmetic operation,
                                  FloatBits<Float> first,
                                  FloatBits<Float> second,
                                  std::uint32_t fpscr_value) {
    first = FlushInput<Float>(first, fpscr_value);
    second = FlushInput<Float>(second, fpscr_value);

    FloatBits<Float> nan_result = 0;
    if (ProcessNans<Float>(first, second, fpscr_value, nan_result)) {
        return nan_result;
    }

    const auto x = std::bit_cast<Float>(first);
    const auto y = std::bit_cast<Float>(second);
    switch (operation) {
    case Arithmetic::Add:
        return Finish<Float>(x + y, fpscr_value);
    case Arithmetic::Subtract:
        return Finish<Float>(x - y, fpscr_value);
    case Arithmetic::Multiply:
        return Finish<Float>(x * y, fpscr_value);
    case Arithmetic::Divide:
        return Finish<Float>(x / y, fpscr_value);
    }
    return FloatFormat<Float>::DefaultNan;
}

}  // namespace vfp_detail

// FPAdd, FPSub, FPMul and FPDiv.
template<class Float>
[[nodiscard]] constexpr FloatBits<Float>
FloatAdd(FloatBits<Float> x, FloatBits<Float> y, std::uint32_t fpscr_value) {
    return vfp_detail::Binary<Float>(vfp_detail::Arithmetic::Add, x, y, fpscr_value);
}

template<class Float>
[[nodiscard]] constexpr FloatBits<Float>
FloatSubtract(FloatBits<Float> x, FloatBits<Float> y, std::uint32_t fpscr_value) {
    return vfp_detail::Binary<Float>(vfp_detail::Arithmetic::Subtract, x, y, fpscr_value);
}

template<class Float>
[[nodiscard]] constexpr FloatBits<Float>
FloatMultiply(FloatBits<Float> x, FloatBits<Float> y, std::uint32_t fpscr_value) {
    return vfp_detail::Binary<Float>(vfp_detail::Arithmetic::Multiply, x, y, fpscr_value);
}

template<class Float>
[[nodiscard]] constexpr FloatBits<Float>
FloatDivide(FloatBits<Float> x, FloatBits<Float> y, std::uint32_t fpscr_value) {
    return vfp_detail::Binary<Float>(vfp_detail::Arithmetic::Divide, x, y, fpscr_value);
}

// FPNeg and FPAbs. Neither is arithmetic: they change the sign bit and nothing
// else, NaNs and denormals included.
template<class Float>
[[nodiscard]] constexpr FloatBits<Float> FloatNegate(FloatBits<Float> x) {
    return x ^ FloatFormat<Float>::SignMask;
}

template<class Float>
[[nodiscard]] constexpr FloatBits<Float> FloatAbsolute(FloatBits<Float> x) {
    return x & ~FloatFormat<Float>::SignMask;
}

// FPSqrt. The square root of a negative number other than minus zero is
// invalid, which the host reports as a NaN and Finish turns into the default
// one.
template<class Float>
[[nodiscard]] inline FloatBits<Float> FloatSquareRoot(FloatBits<Float> x,
                                                      std::uint32_t fpscr_value) {
    x = vfp_detail::FlushInput<Float>(x, fpscr_value);
    if (vfp_detail::IsNan<Float>(x)) {
        return vfp_detail::ProcessNan<Float>(x, fpscr_value);
    }
    return vfp_detail::Finish<Float>(std::sqrt(std::bit_cast<Float>(x)), fpscr_value);
}

// The multiply-accumulate family of VFPv2 is not fused: the product is rounded,
// and then the sum. Source: the VMLA, VMLS, VNMLA and VNMLS pages, which write
// each one as an FPMul followed by an FPAdd, with the negations where they say.
template<class Float>
[[nodiscard]] constexpr FloatBits<Float> FloatMultiplyAccumulate(FloatBits<Float> accumulator,
                                                                 FloatBits<Float> x,
                                                                 FloatBits<Float> y,
                                                                 bool negate_product,
                                                                 bool negate_accumulator,
                                                                 std::uint32_t fpscr_value) {
    FloatBits<Float> product = FloatMultiply<Float>(x, y, fpscr_value);
    if (negate_product) {
        product = FloatNegate<Float>(product);
    }
    if (negate_accumulator) {
        accumulator = FloatNegate<Float>(accumulator);
    }
    return FloatAdd<Float>(accumulator, product, fpscr_value);
}

// FPCompare: the NZCV flags of a comparison, in place in bits 31 to 28. A
// comparison with zero passes zero as the second operand.
template<class Float>
[[nodiscard]] constexpr std::uint32_t
FloatCompare(FloatBits<Float> x, FloatBits<Float> y, std::uint32_t fpscr_value) {
    x = vfp_detail::FlushInput<Float>(x, fpscr_value);
    y = vfp_detail::FlushInput<Float>(y, fpscr_value);
    if (vfp_detail::IsNan<Float>(x) || vfp_detail::IsNan<Float>(y)) {
        return fpscr::Unordered;
    }
    const auto first = std::bit_cast<Float>(x);
    const auto second = std::bit_cast<Float>(y);
    if (first == second) {
        return fpscr::Equal;
    }
    return first < second ? fpscr::Less : fpscr::Greater;
}

// FPSingleToDouble and FPDoubleToSingle. A NaN keeps its sign and the top of its
// payload and becomes quiet, unless DN makes it the default NaN.
[[nodiscard]] constexpr std::uint64_t FloatWiden(std::uint32_t x, std::uint32_t fpscr_value) {
    using Single = FloatFormat<float>;
    using Double = FloatFormat<double>;
    constexpr unsigned PayloadShift = Double::FractionWidth - Single::FractionWidth;
    constexpr unsigned SignShift = 32;

    x = vfp_detail::FlushInput<float>(x, fpscr_value);
    if (vfp_detail::IsNan<float>(x)) {
        if (vfp_detail::DefaultNan(fpscr_value)) {
            return Double::DefaultNan;
        }
        const std::uint64_t sign = static_cast<std::uint64_t>(x & Single::SignMask) << SignShift;
        const std::uint64_t payload = static_cast<std::uint64_t>(x & Single::FractionMask)
                                      << PayloadShift;
        return sign | Double::ExponentMask | Double::QuietBit | payload;
    }
    return std::bit_cast<std::uint64_t>(static_cast<double>(std::bit_cast<float>(x)));
}

[[nodiscard]] constexpr std::uint32_t FloatNarrow(std::uint64_t x, std::uint32_t fpscr_value) {
    using Single = FloatFormat<float>;
    using Double = FloatFormat<double>;
    constexpr unsigned PayloadShift = Double::FractionWidth - Single::FractionWidth;
    constexpr unsigned SignShift = 32;

    x = vfp_detail::FlushInput<double>(x, fpscr_value);
    if (vfp_detail::IsNan<double>(x)) {
        if (vfp_detail::DefaultNan(fpscr_value)) {
            return Single::DefaultNan;
        }
        const auto sign = static_cast<std::uint32_t>((x & Double::SignMask) >> SignShift);
        const auto payload = static_cast<std::uint32_t>((x & Double::FractionMask) >> PayloadShift);
        return sign | Single::ExponentMask | Single::QuietBit | payload;
    }
    return vfp_detail::Finish<float>(static_cast<float>(std::bit_cast<double>(x)), fpscr_value);
}

// FPToFixed with no fraction bits: a float to a 32-bit integer, saturating at
// the ends of the range, with a NaN converting to zero. Rounds towards zero, or
// to nearest with ties to even, which is the only FPSCR mode supported.
template<class Float>
[[nodiscard]] inline std::uint32_t FloatToInteger(FloatBits<Float> x,
                                                  bool is_signed,
                                                  bool round_towards_zero,
                                                  std::uint32_t fpscr_value) {
    x = vfp_detail::FlushInput<Float>(x, fpscr_value);
    if (vfp_detail::IsNan<Float>(x)) {
        return 0;
    }

    // Every single and every double in the range that matters is exact as a
    // double, so the rounding below is exact too.
    const auto value = static_cast<double>(std::bit_cast<Float>(x));
    double rounded = std::trunc(value);
    if (!round_towards_zero) {
        const double remainder = value - rounded;
        const double half = 0.5;
        const double step = value < 0 ? -1.0 : 1.0;
        const bool odd = std::fmod(rounded, 2.0) != 0.0;
        if (std::abs(remainder) > half || (std::abs(remainder) == half && odd)) {
            rounded += step;
        }
    }

    if (is_signed) {
        constexpr double Maximum = std::numeric_limits<std::int32_t>::max();
        constexpr double Minimum = std::numeric_limits<std::int32_t>::min();
        if (rounded >= Maximum) {
            return static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
        }
        if (rounded <= Minimum) {
            return static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::min());
        }
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(rounded));
    }

    constexpr double Maximum = std::numeric_limits<std::uint32_t>::max();
    if (rounded >= Maximum) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    if (rounded <= 0.0) {
        return 0;
    }
    return static_cast<std::uint32_t>(rounded);
}

// FixedToFP with no fraction bits: a 32-bit integer to a float, rounded to
// nearest by the host, which is the mode the game runs in.
template<class Float>
[[nodiscard]] constexpr FloatBits<Float> IntegerToFloat(std::uint32_t x, bool is_signed) {
    const Float value =
        is_signed ? static_cast<Float>(static_cast<std::int32_t>(x)) : static_cast<Float>(x);
    return std::bit_cast<FloatBits<Float>>(value);
}

}  // namespace hexlith::domain::arm::primitives

#endif  // HEXLITH_DOMAIN_ARM_PRIMITIVES_VFP_H
