#include "domain/video/yuv_conversion.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <numeric>
#include <vector>

namespace hexlith::domain::video {
namespace {

// ITU Rec. 601 with full ranges, the first of y2r:u's standard sets. Source:
// standard_coefficients in Azahar's y2r_u.cpp.
constexpr YuvCoefficients Rec601 = {0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B};

// Copies Y into every channel: (0x100 * Y >> 3) is 32 Y, and an offset of
// -0x18 cancels the rounding before the last shift by five.
constexpr YuvCoefficients LumaOnly = {0x100, 0, 0, 0, 0, -0x18, -0x18, -0x18};

YuvConversion Conversion(std::uint32_t width, std::uint32_t lines, const YuvCoefficients& set) {
    return YuvConversion{.input = YuvFormat::Planar422,
                         .output = RgbFormat::Rgb8,
                         .rotation = Rotation::None,
                         .layout = OutputLayout::Lines,
                         .width = width,
                         .lines = lines,
                         .coefficients = set,
                         .alpha = 0};
}

// Y counting up from zero, and U and V of 128.
struct Planes {
    explicit Planes(const YuvConversion& conversion)
        : y(SamplesNeeded(conversion, YuvPlane::Y)),
          u(SamplesNeeded(conversion, YuvPlane::U), 0x80),
          v(SamplesNeeded(conversion, YuvPlane::V), 0x80) {
        std::iota(y.begin(), y.end(), std::uint8_t{0});
    }

    [[nodiscard]] YuvPlanes View() const {
        return YuvPlanes{.y = y, .u = u, .v = v, .interleaved = {}};
    }

    std::vector<std::uint8_t> y;
    std::vector<std::uint8_t> u;
    std::vector<std::uint8_t> v;
};

// The red channel of each pixel of RGB8 output, which is stored blue, green,
// red.
std::vector<std::uint8_t> Reds(const std::vector<std::uint8_t>& rgb8) {
    std::vector<std::uint8_t> reds;
    for (std::size_t at = 2; at < rgb8.size(); at += 3) {
        reds.push_back(rgb8[at]);
    }
    return reds;
}

TEST(ConvertYuvToRgb, AppliesTheFormulaWithItsRounding) {
    // Worked by hand: 0x100 * 128 is 32768. Red: (32768 + 0x166 * 128) >> 3
    // is 9824, less 0x166F plus 0x18 is 4105, >> 5 is 128. Green: (32768 -
    // 0xB6 * 128 - 0x58 * 128) >> 3 is -224, plus 0x10EE and 0x18 is 4134,
    // >> 5 is 129. Blue: (32768 + 0x1C5 * 128) >> 3 is 11344, less 0x1C5B
    // plus 0x18 is 4109, >> 5 is 128.
    YuvConversion conversion = Conversion(8, 1, Rec601);
    conversion.output = RgbFormat::Rgba8;
    conversion.alpha = 0x7F;
    Planes planes(conversion);
    planes.y.assign(planes.y.size(), 0x80);
    const std::vector<std::uint8_t> out = ConvertYuvToRgb(conversion, planes.View());
    ASSERT_EQ(out.size(), 8U * 4U);
    EXPECT_EQ(out[0], 0x7F) << "alpha first, then blue, green, red";
    EXPECT_EQ(out[1], 128);
    EXPECT_EQ(out[2], 129);
    EXPECT_EQ(out[3], 128);
}

TEST(ConvertYuvToRgb, ClampsEachChannel) {
    const YuvConversion conversion = Conversion(8, 1, Rec601);
    Planes planes(conversion);
    planes.y.assign(planes.y.size(), 0xFF);
    planes.v.assign(planes.v.size(), 0xFF);
    EXPECT_EQ(ConvertYuvToRgb(conversion, planes.View())[2], 0xFF) << "red runs over";
    planes.y.assign(planes.y.size(), 0);
    planes.u.assign(planes.u.size(), 0);
    EXPECT_EQ(ConvertYuvToRgb(conversion, planes.View())[0], 0) << "blue runs under";
}

TEST(ConvertYuvToRgb, SharesChromaBetweenTwoLinesIn420) {
    // Blue from U alone: U * 0x100 >> 3 is 32 U, less 0x18 then plus it.
    const YuvCoefficients blue_is_u = {0, 0, 0, 0, 0x100, 0, 0, -0x18};
    YuvConversion conversion = Conversion(8, 4, blue_is_u);
    conversion.input = YuvFormat::Planar420;
    Planes planes(conversion);
    ASSERT_EQ(planes.u.size(), 8U) << "four samples a line pair";
    std::iota(planes.u.begin(), planes.u.end(), std::uint8_t{10});
    const std::vector<std::uint8_t> out = ConvertYuvToRgb(conversion, planes.View());
    const auto blue = [&out](std::size_t x, std::size_t line) {
        return out.at(((line * 8) + x) * 3);
    };
    EXPECT_EQ(blue(0, 0), 10);
    EXPECT_EQ(blue(1, 0), 10) << "two pixels of a line";
    EXPECT_EQ(blue(1, 1), 10) << "and two lines";
    EXPECT_EQ(blue(2, 1), 11);
    EXPECT_EQ(blue(0, 2), 14);
}

TEST(ConvertYuvToRgb, TakesTheLowByteOfAWideSampleAsGathered) {
    YuvConversion conversion = Conversion(8, 1, LumaOnly);
    conversion.input = YuvFormat::Planar422Wide;
    EXPECT_EQ(BytesPerSample(conversion.input), 2U);
    const Planes planes(conversion);
    EXPECT_EQ(Reds(ConvertYuvToRgb(conversion, planes.View())), planes.y);
}

TEST(ConvertYuvToRgb, ReadsInterleavedPairs) {
    YuvConversion conversion = Conversion(8, 1, LumaOnly);
    conversion.input = YuvFormat::Interleaved422;
    std::vector<std::uint8_t> interleaved(SamplesNeeded(conversion, YuvPlane::Interleaved));
    ASSERT_EQ(interleaved.size(), 16U);
    for (std::size_t x = 0; x < 8; ++x) {
        interleaved[x * 2] = static_cast<std::uint8_t>(100 + x);
        interleaved[(x * 2) + 1] = 0x80;
    }
    const std::vector<std::uint8_t> reds = Reds(ConvertYuvToRgb(
        conversion, YuvPlanes{.y = {}, .u = {}, .v = {}, .interleaved = interleaved}));
    EXPECT_EQ(reds, (std::vector<std::uint8_t>{100, 101, 102, 103, 104, 105, 106, 107}));
}

TEST(ConvertYuvToRgb, LaysTilesOutInMortonOrder) {
    YuvConversion conversion = Conversion(16, 8, LumaOnly);
    conversion.layout = OutputLayout::Tiles;
    const Planes planes(conversion);
    const std::vector<std::uint8_t> reds = Reds(ConvertYuvToRgb(conversion, planes.View()));
    ASSERT_EQ(reds.size(), 128U);
    // The start of GBATEK's two-tile example, in which a pixel is its
    // position in the 16x8 bitmap.
    EXPECT_EQ(std::vector<std::uint8_t>(reds.begin(), reds.begin() + 8),
              (std::vector<std::uint8_t>{0x00, 0x01, 0x10, 0x11, 0x02, 0x03, 0x12, 0x13}));
    EXPECT_EQ(reds.at(64), 0x08) << "the second tile starts at column eight";
}

TEST(ConvertYuvToRgb, TurnsEachBlockAQuarterClockwise) {
    YuvConversion conversion = Conversion(8, 8, LumaOnly);
    conversion.rotation = Rotation::Quarter;
    const Planes planes(conversion);
    const std::vector<std::uint8_t> reds = Reds(ConvertYuvToRgb(conversion, planes.View()));
    // Turned clockwise, the bottom-left pixel comes to the top left and the
    // top-left to the top right.
    EXPECT_EQ(reds.at(0), 56);
    EXPECT_EQ(reds.at(7), 0);
    EXPECT_EQ(reds.at(63), 7) << "the top-right pixel ends bottom right";
}

TEST(ConvertYuvToRgb, ReversesTheBlocksOfAStripTurnedHalfway) {
    YuvConversion conversion = Conversion(16, 1, LumaOnly);
    conversion.rotation = Rotation::Half;
    const Planes planes(conversion);
    const std::vector<std::uint8_t> reds = Reds(ConvertYuvToRgb(conversion, planes.View()));
    EXPECT_EQ(reds.front(), 15);
    EXPECT_EQ(reds.back(), 0);
}

TEST(ConvertYuvToRgb, SendsAShortLastStrip) {
    const YuvConversion conversion = Conversion(8, 11, LumaOnly);
    const Planes planes(conversion);
    EXPECT_EQ(Reds(ConvertYuvToRgb(conversion, planes.View())), planes.y);
}

TEST(ConvertYuvToRgb, EncodesTheSixteenBitFormats) {
    YuvConversion conversion = Conversion(8, 1, LumaOnly);
    Planes planes(conversion);
    planes.y.assign(planes.y.size(), 0xFF);
    conversion.output = RgbFormat::Rgb565;
    std::vector<std::uint8_t> out = ConvertYuvToRgb(conversion, planes.View());
    ASSERT_EQ(out.size(), 16U);
    EXPECT_EQ(out[0] | (out[1] << 8U), 0xFFFF);
    conversion.output = RgbFormat::Rgb5A1;
    conversion.alpha = 0x7F;
    out = ConvertYuvToRgb(conversion, planes.View());
    EXPECT_EQ(out[0] | (out[1] << 8U), 0xFFFE) << "alpha takes the top bit of 0x7F";
}

TEST(IsConvertible, RefusesWhatTheHardwareCannotDo) {
    EXPECT_TRUE(IsConvertible(Conversion(8, 3, LumaOnly)));
    EXPECT_FALSE(IsConvertible(Conversion(12, 8, LumaOnly))) << "a width not a multiple of 8";
    EXPECT_FALSE(IsConvertible(Conversion(1032, 8, LumaOnly)));
    YuvConversion tiles = Conversion(8, 4, LumaOnly);
    tiles.layout = OutputLayout::Tiles;
    EXPECT_FALSE(IsConvertible(tiles)) << "tiles need whole strips";
    YuvConversion odd = Conversion(8, 3, LumaOnly);
    odd.input = YuvFormat::Planar420;
    EXPECT_FALSE(IsConvertible(odd)) << "4:2:0 needs line pairs";
    EXPECT_TRUE(ConvertYuvToRgb(tiles, YuvPlanes{}).empty());
}

}  // namespace
}  // namespace hexlith::domain::video
