#include "core/text.h"

#include <array>
#include <format>

namespace hexlith::core {
namespace {

constexpr std::uint32_t HighSurrogateFirst = 0xD800;
constexpr std::uint32_t HighSurrogateLast = 0xDBFF;
constexpr std::uint32_t LowSurrogateFirst = 0xDC00;
constexpr std::uint32_t LowSurrogateLast = 0xDFFF;

// A code point outside the basic multilingual plane is written as a pair of
// surrogates, each carrying ten bits of the value biased by this amount.
constexpr std::uint32_t SurrogateBase = 0x10000;
constexpr unsigned SurrogateBits = 10;
constexpr std::uint32_t SurrogateValueMask = 0x3FF;
constexpr std::uint32_t MaxCodePoint = 0x10FFFF;

constexpr std::uint32_t OneByteLimit = 0x80;
constexpr std::uint32_t TwoByteLimit = 0x800;
constexpr std::uint32_t ThreeByteLimit = 0x10000;

// The leading-byte markers of each UTF-8 form and the shifts that build
// them, from RFC 3629. A sequence of n bytes carries its first bits in a
// leading byte marked with n high bits, and the rest six at a time in
// continuation bytes marked 10.
constexpr std::uint32_t ContinuationMask = 0x3F;
constexpr std::uint8_t ContinuationMarker = 0x80;
constexpr std::uint32_t ContinuationCheckMask = 0xC0;
constexpr std::uint32_t TwoByteMarker = 0xC0;
constexpr std::uint32_t ThreeByteMarker = 0xE0;
constexpr std::uint32_t FourByteMarker = 0xF0;
constexpr std::uint32_t FiveByteMarker = 0xF8;
constexpr unsigned ContinuationBits = 6;
constexpr unsigned TwoContinuationBits = 12;
constexpr unsigned ThreeContinuationBits = 18;

constexpr std::uint32_t ByteMask = 0xFF;
constexpr unsigned ByteBits = 8;

void AppendUtf8(std::string& text, std::uint32_t code_point) {
    const auto continuation = [&text](std::uint32_t value, unsigned shift) {
        text.push_back(
            static_cast<char>(ContinuationMarker | ((value >> shift) & ContinuationMask)));
    };

    if (code_point < OneByteLimit) {
        text.push_back(static_cast<char>(code_point));
        return;
    }
    if (code_point < TwoByteLimit) {
        text.push_back(static_cast<char>(TwoByteMarker | (code_point >> ContinuationBits)));
        continuation(code_point, 0);
        return;
    }
    if (code_point < ThreeByteLimit) {
        text.push_back(static_cast<char>(ThreeByteMarker | (code_point >> TwoContinuationBits)));
        continuation(code_point, ContinuationBits);
        continuation(code_point, 0);
        return;
    }
    text.push_back(static_cast<char>(FourByteMarker | (code_point >> ThreeContinuationBits)));
    continuation(code_point, TwoContinuationBits);
    continuation(code_point, ContinuationBits);
    continuation(code_point, 0);
}

void AppendUtf16Le(std::vector<std::uint8_t>& units, std::uint16_t unit) {
    units.push_back(static_cast<std::uint8_t>(unit & ByteMask));
    units.push_back(static_cast<std::uint8_t>(unit >> ByteBits));
}

bool IsHighSurrogate(std::uint32_t unit) {
    return unit >= HighSurrogateFirst && unit <= HighSurrogateLast;
}

bool IsLowSurrogate(std::uint32_t unit) {
    return unit >= LowSurrogateFirst && unit <= LowSurrogateLast;
}

// How many bytes the leading byte of a UTF-8 sequence announces, or zero when
// it announces nothing valid.
unsigned Utf8SequenceLength(std::uint8_t leading) {
    if (leading < OneByteLimit) {
        return 1;
    }
    if ((leading & ThreeByteMarker) == TwoByteMarker) {
        return 2;
    }
    if ((leading & FourByteMarker) == ThreeByteMarker) {
        return 3;
    }
    if ((leading & FiveByteMarker) == FourByteMarker) {
        return 4;
    }
    return 0;
}

}  // namespace

Result<std::string> Utf16LeToUtf8(ByteSpan utf16) {
    if (utf16.size() % 2 != 0) {
        return Error{
            std::format("a UTF-16 string is an even number of bytes and this is {}", utf16.size())};
    }

    std::string text;
    text.reserve(utf16.size());

    for (std::size_t index = 0; index < utf16.size(); index += 2) {
        const std::uint32_t unit = ReadU16At(utf16, index).Value();

        if (IsLowSurrogate(unit)) {
            return Error{std::format("the unit at {} is a trailing surrogate with nothing before "
                                     "it, so this is not UTF-16",
                                     index / 2)};
        }

        if (!IsHighSurrogate(unit)) {
            AppendUtf8(text, unit);
            continue;
        }

        if (index + 4 > utf16.size()) {
            return Error{std::format("the leading surrogate at {} has no pair", index / 2)};
        }
        const std::uint32_t low = ReadU16At(utf16, index + 2).Value();
        if (!IsLowSurrogate(low)) {
            return Error{std::format("the leading surrogate at {} is followed by {:#06x} rather "
                                     "than a trailing surrogate",
                                     index / 2,
                                     low)};
        }

        const std::uint32_t code_point =
            SurrogateBase +
            (((unit - HighSurrogateFirst) << SurrogateBits) | (low - LowSurrogateFirst));
        AppendUtf8(text, code_point);
        index += 2;
    }

    return text;
}

Result<std::vector<std::uint8_t>> Utf8ToUtf16Le(std::string_view utf8) {
    std::vector<std::uint8_t> units;
    units.reserve(utf8.size() * 2);

    std::size_t index = 0;
    while (index < utf8.size()) {
        const auto leading = static_cast<std::uint8_t>(utf8[index]);
        const unsigned length = Utf8SequenceLength(leading);
        if (length == 0) {
            return Error{std::format(
                "the byte {:#04x} at {} does not start a UTF-8 sequence", leading, index)};
        }
        if (index + length > utf8.size()) {
            return Error{std::format("the sequence at {} needs {} bytes and only {} remain",
                                     index,
                                     length,
                                     utf8.size() - index)};
        }

        std::uint32_t code_point = leading;
        if (length > 1) {
            static constexpr std::array<std::uint32_t, 5> LeadingMask = {0, 0x7F, 0x1F, 0x0F, 0x07};
            code_point = leading & LeadingMask.at(length);
            for (unsigned extra = 1; extra < length; ++extra) {
                const auto continuation = static_cast<std::uint8_t>(utf8[index + extra]);
                if ((continuation & ContinuationCheckMask) != ContinuationMarker) {
                    return Error{std::format("the byte {:#04x} at {} is not a continuation byte",
                                             continuation,
                                             index + extra)};
                }
                code_point = (code_point << ContinuationBits) | (continuation & ContinuationMask);
            }
        }

        if (code_point > MaxCodePoint ||
            (code_point >= HighSurrogateFirst && code_point <= LowSurrogateLast)) {
            return Error{std::format("{:#x} is not a code point UTF-16 can carry", code_point)};
        }

        if (code_point < SurrogateBase) {
            AppendUtf16Le(units, static_cast<std::uint16_t>(code_point));
        } else {
            const std::uint32_t biased = code_point - SurrogateBase;
            AppendUtf16Le(
                units, static_cast<std::uint16_t>(HighSurrogateFirst + (biased >> SurrogateBits)));
            AppendUtf16Le(
                units,
                static_cast<std::uint16_t>(LowSurrogateFirst + (biased & SurrogateValueMask)));
        }

        index += length;
    }

    return units;
}

}  // namespace hexlith::core
