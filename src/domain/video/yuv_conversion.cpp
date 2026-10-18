#include "domain/video/yuv_conversion.h"

#include "domain/video/morton.h"

#include <algorithm>

namespace hexlith::domain::video {
namespace {

// The widest line the unit takes: Y2R_WIDTH counts in eight-pixel units up
// to 7Fh, and y2r:u refuses more than 1024. Source: GBATEK, Y2R_WIDTH;
// SetInputLineWidth in Azahar's y2r_u.cpp.
constexpr std::uint32_t MaximumWidth = 1024;

// The products are kept with three fractional bits more than the offsets
// and eight more than the result, and 0x18 is added before the last shift.
// Source: ConvertYUVToRGB in Azahar's y2r.cpp.
constexpr unsigned ProductShift = 3;
constexpr unsigned ResultShift = 5;
constexpr std::int32_t RoundingOffset = 0x18;
constexpr std::int32_t ChannelMaximum = 0xFF;

struct Rgb {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
};

struct Yuv {
    std::int32_t y = 0;
    std::int32_t u = 0;
    std::int32_t v = 0;
};

bool IsWide(YuvFormat format) {
    return format == YuvFormat::Planar422Wide || format == YuvFormat::Planar420Wide;
}

bool IsSubsampledVertically(YuvFormat format) {
    return format == YuvFormat::Planar420 || format == YuvFormat::Planar420Wide;
}

// The samples of the pixel at (x, line). U and V are shared by two pixels of
// a line, and in the 4:2:0 formats by two lines as well.
Yuv SampleAt(const YuvConversion& conversion,
             const YuvPlanes& planes,
             std::uint32_t x,
             std::uint32_t line) {
    const std::size_t width = conversion.width;
    if (conversion.input == YuvFormat::Interleaved422) {
        const std::size_t pair = (line * width) + (x & ~1U);
        return Yuv{.y = planes.interleaved[((line * width) + x) * 2],
                   .u = planes.interleaved[(pair * 2) + 1],
                   .v = planes.interleaved[(pair * 2) + 3]};
    }
    const std::size_t chroma_line = IsSubsampledVertically(conversion.input) ? line / 2 : line;
    const std::size_t chroma = (chroma_line * (width / 2)) + (x / 2);
    return Yuv{.y = planes.y[(line * width) + x], .u = planes.u[chroma], .v = planes.v[chroma]};
}

std::uint8_t Channel(std::int32_t product, std::int16_t offset) {
    const std::int32_t value = (product >> ProductShift) + offset + RoundingOffset;
    return static_cast<std::uint8_t>(std::clamp(value >> ResultShift, 0, ChannelMaximum));
}

// The formula: Y times one multiplier for every channel, the chroma times
// the others, and an offset in place of subtracting 128 from U and V.
// Source: GBATEK, Standard YUV to RGB Formula; Azahar for the rounding.
Rgb ToRgb(const Yuv& sample, const YuvCoefficients& c) {
    const std::int32_t luma = c[0] * sample.y;
    return Rgb{.red = Channel(luma + (c[1] * sample.v), c[5]),
               .green = Channel(luma - (c[2] * sample.v) - (c[3] * sample.u), c[6]),
               .blue = Channel(luma + (c[4] * sample.u), c[7])};
}

// One pixel in the output format, in memory order. Source: GBATEK, Y2R_CNT
// bits 8 and 9 and Y2R_ALPHA; the byte order is the GPU's, as EncodeRGBA8 and
// its neighbours write it in Azahar's src/common/color.h.
void Encode(RgbFormat format,
            const Rgb& pixel,
            std::uint8_t alpha,
            std::vector<std::uint8_t>& out) {
    constexpr unsigned To5 = 3;
    constexpr unsigned To6 = 2;
    constexpr unsigned To1 = 7;
    const auto push_halfword = [&out](std::uint32_t value) {
        constexpr unsigned ByteBits = 8;
        out.push_back(static_cast<std::uint8_t>(value));
        out.push_back(static_cast<std::uint8_t>(value >> ByteBits));
    };
    switch (format) {
    case RgbFormat::Rgba8:
        out.insert(out.end(), {alpha, pixel.blue, pixel.green, pixel.red});
        break;
    case RgbFormat::Rgb8:
        out.insert(out.end(), {pixel.blue, pixel.green, pixel.red});
        break;
    case RgbFormat::Rgb5A1:
        push_halfword((static_cast<std::uint32_t>(pixel.red >> To5) << 11U) |
                      (static_cast<std::uint32_t>(pixel.green >> To5) << 6U) |
                      (static_cast<std::uint32_t>(pixel.blue >> To5) << 1U) |
                      static_cast<std::uint32_t>(alpha >> To1));
        break;
    case RgbFormat::Rgb565:
        push_halfword((static_cast<std::uint32_t>(pixel.red >> To5) << 11U) |
                      (static_cast<std::uint32_t>(pixel.green >> To6) << 5U) |
                      static_cast<std::uint32_t>(pixel.blue >> To5));
        break;
    }
}

// The order the pixels of an 8-wide block of a strip, `lines` high, come out
// in once it is rotated, as indices into the block read row by row. A
// quarter turn reads it a column at a time from the bottom up, which for a
// full block is the block turned clockwise. Source: RotateTile90 and its
// neighbours in Azahar's y2r.cpp.
std::vector<std::uint32_t> RotatedOrder(Rotation rotation, std::uint32_t lines) {
    std::vector<std::uint32_t> order;
    order.reserve(static_cast<std::size_t>(TileSide) * lines);
    switch (rotation) {
    case Rotation::None:
        for (std::uint32_t index = 0; index < TileSide * lines; ++index) {
            order.push_back(index);
        }
        break;
    case Rotation::Quarter:
        for (std::uint32_t x = 0; x < TileSide; ++x) {
            for (std::uint32_t y = lines; y-- > 0;) {
                order.push_back((y * TileSide) + x);
            }
        }
        break;
    case Rotation::Half:
        for (std::uint32_t index = TileSide * lines; index-- > 0;) {
            order.push_back(index);
        }
        break;
    case Rotation::ThreeQuarters:
        for (std::uint32_t x = TileSide; x-- > 0;) {
            for (std::uint32_t y = 0; y < lines; ++y) {
                order.push_back((y * TileSide) + x);
            }
        }
        break;
    }
    return order;
}

bool TurnsSideways(Rotation rotation) {
    return rotation == Rotation::Quarter || rotation == Rotation::ThreeQuarters;
}

// One strip of up to eight lines, as pixels in the order they are sent.
std::vector<Rgb> ConvertStrip(const YuvConversion& conversion,
                              const YuvPlanes& planes,
                              std::uint32_t first_line,
                              std::uint32_t lines) {
    const std::uint32_t blocks = conversion.width / TileSide;
    const std::vector<std::uint32_t> order = RotatedOrder(conversion.rotation, lines);
    const bool reversed =
        conversion.rotation == Rotation::Half || conversion.rotation == Rotation::ThreeQuarters;
    std::vector<Rgb> strip(static_cast<std::size_t>(conversion.width) * lines);

    for (std::uint32_t slot = 0; slot < blocks; ++slot) {
        // A half or three-quarter turn also reverses the blocks of a strip.
        const std::uint32_t block = reversed ? blocks - 1 - slot : slot;
        std::array<Rgb, TilePixels> placed{};
        for (std::uint32_t position = 0; position < order.size(); ++position) {
            const std::uint32_t source = order[position];
            const Rgb pixel = ToRgb(SampleAt(conversion,
                                             planes,
                                             (block * TileSide) + (source % TileSide),
                                             first_line + (source / TileSide)),
                                    conversion.coefficients);
            const std::uint32_t x = position % TileSide;
            const std::uint32_t y = position / TileSide;
            placed.at(conversion.layout == OutputLayout::Tiles ? MortonIndex(x, y) : position) =
                pixel;
        }

        if (conversion.layout == OutputLayout::Tiles) {
            std::ranges::copy(placed,
                              strip.begin() + (static_cast<std::ptrdiff_t>(slot) * TilePixels));
            continue;
        }
        // In lines, a block turned sideways is sent whole after the one
        // before it; otherwise it takes its place across the strip.
        const bool sideways = TurnsSideways(conversion.rotation);
        const std::size_t base =
            sideways ? std::size_t{slot} * TileSide * lines : std::size_t{slot} * TileSide;
        const std::size_t stride = sideways ? TileSide : conversion.width;
        for (std::uint32_t y = 0; y < lines; ++y) {
            for (std::uint32_t x = 0; x < TileSide; ++x) {
                strip.at(base + (y * stride) + x) = placed.at((y * TileSide) + x);
            }
        }
    }
    return strip;
}

}  // namespace

std::size_t BytesPerSample(YuvFormat format) {
    return IsWide(format) ? 2 : 1;
}

std::size_t BytesPerPixel(RgbFormat format) {
    switch (format) {
    case RgbFormat::Rgba8:
        return 4;
    case RgbFormat::Rgb8:
        return 3;
    case RgbFormat::Rgb5A1:
    case RgbFormat::Rgb565:
        return 2;
    }
    return 0;
}

std::size_t SamplesNeeded(const YuvConversion& conversion, YuvPlane plane) {
    const std::size_t pixels = static_cast<std::size_t>(conversion.width) * conversion.lines;
    if (conversion.input == YuvFormat::Interleaved422) {
        return plane == YuvPlane::Interleaved ? pixels * 2 : 0;
    }
    switch (plane) {
    case YuvPlane::Y:
        return pixels;
    case YuvPlane::U:
    case YuvPlane::V:
        return IsSubsampledVertically(conversion.input) ? pixels / 4 : pixels / 2;
    case YuvPlane::Interleaved:
        return 0;
    }
    return 0;
}

bool IsConvertible(const YuvConversion& conversion) {
    return conversion.input <= YuvFormat::Interleaved422 &&
           conversion.output <= RgbFormat::Rgb565 &&
           conversion.rotation <= Rotation::ThreeQuarters &&
           conversion.layout <= OutputLayout::Tiles && conversion.width != 0 &&
           conversion.width <= MaximumWidth && conversion.width % TileSide == 0 &&
           (conversion.layout == OutputLayout::Lines || conversion.lines % TileSide == 0) &&
           (!IsSubsampledVertically(conversion.input) || conversion.lines % 2 == 0);
}

std::vector<std::uint8_t> ConvertYuvToRgb(const YuvConversion& conversion,
                                          const YuvPlanes& planes) {
    std::vector<std::uint8_t> out;
    if (!IsConvertible(conversion)) {
        return out;
    }
    out.reserve(static_cast<std::size_t>(conversion.width) * conversion.lines *
                BytesPerPixel(conversion.output));
    for (std::uint32_t first = 0; first < conversion.lines; first += TileSide) {
        const std::uint32_t lines = std::min(TileSide, conversion.lines - first);
        for (const Rgb& pixel : ConvertStrip(conversion, planes, first, lines)) {
            Encode(conversion.output, pixel, conversion.alpha, out);
        }
    }
    return out;
}

}  // namespace hexlith::domain::video
