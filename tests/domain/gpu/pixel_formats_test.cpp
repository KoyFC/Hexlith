#include "domain/gpu/pixel_formats.h"

#include <array>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::gpu {
namespace {

// A texture of the format and size whose every byte is zero but those set.
std::vector<std::uint8_t> Blank(TextureFormat format, std::uint32_t width, std::uint32_t height) {
    return std::vector<std::uint8_t>(TextureBytes(format, width, height));
}

TEST(TextureBytes, CountsWholeTiles) {
    EXPECT_EQ(TextureBytes(TextureFormat::Rgba8, 16, 8), 512U);
    EXPECT_EQ(TextureBytes(TextureFormat::A4, 8, 8), 32U);
    EXPECT_EQ(TextureBytes(TextureFormat::Etc1, 8, 8), 32U) << "four blocks of eight bytes";
    EXPECT_EQ(TextureBytes(TextureFormat::Etc1A4, 8, 8), 64U);
}

TEST(DecodeTexel, FindsATexelInItsTile) {
    // L8, 16x16: the texel at (9, 10) is in the fourth tile (second row,
    // second column), at Morton index 1 + 4 ... of (1, 2): 0b001001 = 9.
    std::vector<std::uint8_t> texture = Blank(TextureFormat::L8, 16, 16);
    texture.at((3 * 64) + 9) = 0x77;
    EXPECT_EQ(DecodeTexel(texture, TextureFormat::L8, 16, 9, 10),
              (Color{.red = 0x77, .green = 0x77, .blue = 0x77, .alpha = 0xFF}));
}

// The first texel of an 8x8 texture whose first bytes are given.
Color First(TextureFormat format, std::vector<std::uint8_t> bytes) {
    bytes.resize(TextureBytes(format, 8, 8));
    return DecodeTexel(bytes, format, 8, 0, 0);
}

TEST(DecodeTexel, ReadsTheColourFormats) {
    EXPECT_EQ(First(TextureFormat::Rgba8, {0x44, 0x33, 0x22, 0x11}),
              (Color{.red = 0x11, .green = 0x22, .blue = 0x33, .alpha = 0x44}));
    EXPECT_EQ(First(TextureFormat::Rgb8, {0x33, 0x22, 0x11}),
              (Color{.red = 0x11, .green = 0x22, .blue = 0x33, .alpha = 0xFF}));
    // 0b11111_00000_10000_1: red full, green none, blue half, alpha set.
    EXPECT_EQ(First(TextureFormat::Rgb5A1, {0x21, 0xF8}),
              (Color{.red = 0xFF, .green = 0, .blue = 0x84, .alpha = 0xFF}));
    EXPECT_EQ(First(TextureFormat::Rgb565, {0xE0, 0x07}),
              (Color{.red = 0, .green = 0xFF, .blue = 0, .alpha = 0xFF}));
    EXPECT_EQ(First(TextureFormat::Rgba4, {0x34, 0x12}),
              (Color{.red = 0x11, .green = 0x22, .blue = 0x33, .alpha = 0x44}));
}

TEST(DecodeTexel, ReadsTheLuminanceAndAlphaFormats) {
    EXPECT_EQ(First(TextureFormat::La8, {0x80, 0x40}),
              (Color{.red = 0x40, .green = 0x40, .blue = 0x40, .alpha = 0x80}));
    EXPECT_EQ(First(TextureFormat::HiLo8, {0x80, 0x40}),
              (Color{.red = 0x40, .green = 0x80, .blue = 0, .alpha = 0xFF}));
    EXPECT_EQ(First(TextureFormat::A8, {0x90}),
              (Color{.red = 0, .green = 0, .blue = 0, .alpha = 0x90}));
    EXPECT_EQ(First(TextureFormat::La4, {0x3C}),
              (Color{.red = 0x33, .green = 0x33, .blue = 0x33, .alpha = 0xCC}));
}

TEST(DecodeTexel, PutsTheFirstOfTwoNibblesLow) {
    std::vector<std::uint8_t> texture = Blank(TextureFormat::A4, 8, 8);
    texture.at(0) = 0x2B;
    EXPECT_EQ(DecodeTexel(texture, TextureFormat::A4, 8, 0, 0).alpha, 0xBB);
    EXPECT_EQ(DecodeTexel(texture, TextureFormat::A4, 8, 1, 0).alpha, 0x22);
}

// One ETC1 block, little-endian. Individual mode: first half red 8, green
// 4, blue 2 as nibbles, table 0 (2, 8); second half all 0, table 7. Pixel 0
// takes the small modifier added; pixel 5, at (1, 1), the large one
// subtracted.
constexpr std::uint64_t Etc1Block = (0x8ULL << 60) | (0x4ULL << 52) | (0x2ULL << 44) |
                                    (0x7ULL << 34) | (1ULL << 5) | (1ULL << (16 + 5));

std::vector<std::uint8_t> Etc1Texture(bool alpha) {
    std::vector<std::uint8_t> texture(
        TextureBytes(alpha ? TextureFormat::Etc1A4 : TextureFormat::Etc1, 8, 8));
    const std::size_t at = alpha ? 8 : 0;
    for (std::size_t byte = 0; byte < 8; ++byte) {
        texture.at(at + byte) = static_cast<std::uint8_t>(Etc1Block >> (8 * byte));
    }
    if (alpha) {
        // Alpha nibble of pixel 5 is F, pixel 0's is 3.
        texture.at(0) = 0x03;
        texture.at(2) = 0xF0;
    }
    return texture;
}

TEST(DecodeTexel, DecodesAnEtc1Block) {
    const std::vector<std::uint8_t> texture = Etc1Texture(false);
    EXPECT_EQ(DecodeTexel(texture, TextureFormat::Etc1, 8, 0, 0),
              (Color{.red = 0x8A, .green = 0x46, .blue = 0x24, .alpha = 0xFF}));
    EXPECT_EQ(DecodeTexel(texture, TextureFormat::Etc1, 8, 1, 1),
              (Color{.red = 0x80, .green = 0x3C, .blue = 0x1A, .alpha = 0xFF}));
    EXPECT_EQ(DecodeTexel(texture, TextureFormat::Etc1, 8, 3, 0),
              (Color{.red = 47, .green = 47, .blue = 47, .alpha = 0xFF}))
        << "the second half: zero plus table 7's small step";
}

TEST(DecodeTexel, ReadsEtc1A4AlphaInTheSameOrder) {
    const std::vector<std::uint8_t> texture = Etc1Texture(true);
    EXPECT_EQ(DecodeTexel(texture, TextureFormat::Etc1A4, 8, 0, 0).alpha, 0x33);
    EXPECT_EQ(DecodeTexel(texture, TextureFormat::Etc1A4, 8, 1, 1).alpha, 0xFF);
    EXPECT_EQ(DecodeTexel(texture, TextureFormat::Etc1A4, 8, 1, 1).red, 0x80);
}

TEST(PixelFormats, ReadFromBothRegisterOrders) {
    EXPECT_EQ(ColorBufferFormat(2), PixelFormat::Rgb5A1);
    EXPECT_EQ(TransferFormat(2), PixelFormat::Rgb565);
    EXPECT_EQ(TransferFormat(6), PixelFormat::Rgba4) << "4 to 7 are all 4444";
    EXPECT_FALSE(ColorBufferFormat(5).has_value());
}

TEST(PixelFormats, KeepTheTopBitsAndWidenThemBack) {
    const Color color{.red = 0xFE, .green = 0x81, .blue = 0x40, .alpha = 0x7F};
    std::array<std::uint8_t, 4> bytes{};
    for (const PixelFormat format : {PixelFormat::Rgba8,
                                     PixelFormat::Rgb8,
                                     PixelFormat::Rgb5A1,
                                     PixelFormat::Rgb565,
                                     PixelFormat::Rgba4}) {
        EncodePixel(format, color, bytes);
        const Color back = DecodePixel(format, bytes);
        EncodePixel(format, back, bytes);
        EXPECT_EQ(DecodePixel(format, bytes), back) << "a decoded value encodes to itself";
    }
    EncodePixel(PixelFormat::Rgb565, color, bytes);
    EXPECT_EQ(DecodePixel(PixelFormat::Rgb565, bytes),
              (Color{.red = 0xFF, .green = 0x82, .blue = 0x42, .alpha = 0xFF}));
}

TEST(DepthFormats, StoreDepthLowAndStencilHigh) {
    std::array<std::uint8_t, 4> bytes{};
    EncodeDepth(DepthFormat::D24S8, 0x123456, bytes);
    EncodeStencil(0x9A, bytes);
    EXPECT_EQ(bytes, (std::array<std::uint8_t, 4>{0x56, 0x34, 0x12, 0x9A}));
    EXPECT_EQ(DecodeDepth(DepthFormat::D24S8, bytes), 0x123456U);
    EXPECT_EQ(DecodeStencil(bytes), 0x9A);
    EXPECT_EQ(DecodeDepth(DepthFormat::D16, bytes), 0x3456U);
    EXPECT_EQ(MaximumDepth(DepthFormat::D16), 0xFFFFU);
}

TEST(TiledOffset, PlacesPixelsInMortonTilesARowAtATime) {
    EXPECT_EQ(TiledOffset(0, 0, 16, 4), 0U);
    EXPECT_EQ(TiledOffset(1, 1, 16, 4), 3U * 4);
    EXPECT_EQ(TiledOffset(8, 0, 16, 4), 64U * 4) << "the next tile";
    EXPECT_EQ(TiledOffset(0, 8, 16, 4), 128U * 4) << "the next row of tiles";
}

}  // namespace
}  // namespace hexlith::domain::gpu
