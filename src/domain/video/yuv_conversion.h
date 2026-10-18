#ifndef HEXLITH_DOMAIN_VIDEO_YUV_CONVERSION_H
#define HEXLITH_DOMAIN_VIDEO_YUV_CONVERSION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hexlith::domain::video {

// What the Y2R unit does to an image: turn YUV samples into RGB pixels, in
// strips of eight lines, rotating each 8x8 block of a strip if asked, and lay
// them out as lines or as 8x8 tiles. Source: GBATEK, 3DS Video Y2R Registers,
// for the settings and the formula; Azahar's src/core/hw/y2r.cpp for the
// rounding, which it says matches hardware bit for bit as far as it was
// tested, and for what a rotation does to a strip.
//
// This is the conversion alone. Gathering the samples from memory and
// scattering the pixels back, in units with gaps between them, is y2r:u's.

// Values as y2r:u's SetInputFormat takes them.
enum class YuvFormat : std::uint8_t {
    // Y, U and V in planes of their own, a byte each, U and V for every two
    // pixels of a line.
    Planar422 = 0,
    // The same, with U and V for every two pixels of every two lines.
    Planar420 = 1,
    // As the first two, with each sample in the low byte of a halfword.
    Planar422Wide = 2,
    Planar420Wide = 3,
    // One plane of Y, U, Y, V for each two pixels, as the camera writes.
    Interleaved422 = 4,
};

// Values as SetOutputFormat takes them.
enum class RgbFormat : std::uint8_t { Rgba8 = 0, Rgb8 = 1, Rgb5A1 = 2, Rgb565 = 3 };

// Values as SetRotation takes them: clockwise, a quarter turn each.
enum class Rotation : std::uint8_t { None = 0, Quarter = 1, Half = 2, ThreeQuarters = 3 };

// Values as SetBlockAlignment takes them.
enum class OutputLayout : std::uint8_t { Lines = 0, Tiles = 1 };

// The multipliers of Y for every channel, V for red, V for green, U for
// green and U for blue, then the offsets of red, green and blue. Source:
// GBATEK, Y2R_MULTIPLIER and Y2R_OFFSET.
using YuvCoefficients = std::array<std::int16_t, 8>;

struct YuvConversion {
    YuvFormat input = YuvFormat::Planar422;
    RgbFormat output = RgbFormat::Rgba8;
    Rotation rotation = Rotation::None;
    OutputLayout layout = OutputLayout::Lines;
    // In pixels, a multiple of eight.
    std::uint32_t width = 0;
    std::uint32_t lines = 0;
    YuvCoefficients coefficients{};
    // The alpha of the formats that have one; RGB5A1 takes its top bit.
    std::uint8_t alpha = 0;
};

// The samples of an image, one byte each, gathered in the order the hardware
// takes them. A wide format's samples are their low bytes. Planes the format
// does not use are empty.
struct YuvPlanes {
    std::span<const std::uint8_t> y;
    std::span<const std::uint8_t> u;
    std::span<const std::uint8_t> v;
    std::span<const std::uint8_t> interleaved;
};

enum class YuvPlane : std::uint8_t { Y, U, V, Interleaved };

// How many samples of a plane the conversion takes. Zero for a plane the
// format does not use.
std::size_t SamplesNeeded(const YuvConversion& conversion, YuvPlane plane);

// Bytes in memory for each sample: two for the wide formats, one otherwise.
std::size_t BytesPerSample(YuvFormat format);

std::size_t BytesPerPixel(RgbFormat format);

// Whether the hardware can do the conversion: a known format, rotation and
// layout, a width that is a positive multiple of eight up to 1024, for tiles
// a number of lines that is a multiple of eight, and for 4:2:0 an even number.
// An odd count of lines in 4:2:0 would leave the last line without the chroma
// it needs: the hardware is sent a quarter of the pixel count, and what it
// does then has not been found out.
bool IsConvertible(const YuvConversion& conversion);

// The output, in the order the unit sends it out: strip after strip, each
// strip's pixels in its layout. Each plane must hold at least the samples
// SamplesNeeded asks for. Empty if the conversion is not convertible.
std::vector<std::uint8_t> ConvertYuvToRgb(const YuvConversion& conversion, const YuvPlanes& planes);

}  // namespace hexlith::domain::video

#endif  // HEXLITH_DOMAIN_VIDEO_YUV_CONVERSION_H
