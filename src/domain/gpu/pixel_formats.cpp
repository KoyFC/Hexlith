#include "domain/gpu/pixel_formats.h"

#include "domain/video/morton.h"

#include <algorithm>
#include <array>

namespace hexlith::domain::gpu {
namespace {

using video::MortonIndex;
using video::TilePixels;
using video::TileSide;

// Widening a channel repeats its top bits: what GBATEK calls 4bit*11h and
// 5bit*21h/4. Source: GBATEK, Texture Formats; Convert4To8 and its
// neighbours in Azahar's src/common/color.h.
constexpr std::uint8_t Widen1(std::uint32_t value) {
    return static_cast<std::uint8_t>((value & 1U) * 0xFF);
}

constexpr std::uint8_t Widen4(std::uint32_t value) {
    return static_cast<std::uint8_t>(((value & 0xFU) << 4) | (value & 0xFU));
}

constexpr std::uint8_t Widen5(std::uint32_t value) {
    return static_cast<std::uint8_t>((value << 3) | ((value & 0x1FU) >> 2));
}

constexpr std::uint8_t Widen6(std::uint32_t value) {
    return static_cast<std::uint8_t>(((value & 0x3FU) << 2) | ((value & 0x3FU) >> 4));
}

std::uint32_t Read16(std::span<const std::uint8_t> bytes) {
    return bytes[0] | (static_cast<std::uint32_t>(bytes[1]) << 8);
}

void Write16(std::uint32_t value, std::span<std::uint8_t> bytes) {
    bytes[0] = static_cast<std::uint8_t>(value);
    bytes[1] = static_cast<std::uint8_t>(value >> 8);
}

// ETC1's modifier pairs, small and large, by table. Source: GBATEK, ETC1.
constexpr std::array<std::array<int, 2>, 8> Etc1Modifiers = {{
    {2, 8},
    {5, 17},
    {9, 29},
    {13, 42},
    {18, 60},
    {24, 80},
    {33, 106},
    {47, 183},
}};

std::uint64_t Read64(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        value |= static_cast<std::uint64_t>(bytes[byte]) << (8 * byte);
    }
    return value;
}

std::uint32_t Bits(std::uint64_t value, unsigned low, unsigned width) {
    return static_cast<std::uint32_t>((value >> low) & ((std::uint64_t{1} << width) - 1));
}

// A five-bit base plus a signed three-bit difference. A sum outside the five
// bits, which the format says never happens, is kept to a byte and widened
// as a byte, as Azahar does.
std::uint8_t
DifferentialChannel(std::uint64_t block, unsigned base_low, unsigned delta_low, bool second) {
    int value = static_cast<int>(Bits(block, base_low, 5));
    if (second) {
        const auto delta = static_cast<int>(Bits(block, delta_low, 3));
        value += delta >= 4 ? delta - 8 : delta;
    }
    const auto byte = static_cast<std::uint8_t>(value);
    return static_cast<std::uint8_t>((byte << 3) | (byte >> 2));
}

// The pixel at (x, y) of an ETC1 block, column by column as GBATEK orders
// them. Source: GBATEK, ETC1; the Khronos ETC1 specification it cites.
Color DecodeEtc1(std::uint64_t block, std::uint32_t x, std::uint32_t y) {
    const std::uint32_t pixel = (4 * x) + y;
    const bool flip = Bits(block, 32, 1) != 0;
    const bool differential = Bits(block, 33, 1) != 0;
    // Without flip the halves are left and right, with it top and bottom.
    const bool second = (flip ? y : x) >= 2;

    std::array<int, 3> rgb{};
    if (differential) {
        rgb = {DifferentialChannel(block, 59, 56, second),
               DifferentialChannel(block, 51, 48, second),
               DifferentialChannel(block, 43, 40, second)};
    } else {
        rgb = {Widen4(Bits(block, second ? 56 : 60, 4)),
               Widen4(Bits(block, second ? 48 : 52, 4)),
               Widen4(Bits(block, second ? 40 : 44, 4))};
    }
    const std::uint32_t table = Bits(block, second ? 34 : 37, 3);
    int modifier = Etc1Modifiers.at(table).at(Bits(block, pixel, 1));
    if (Bits(block, 16 + pixel, 1) != 0) {
        modifier = -modifier;
    }
    const auto channel = [modifier](int value) {
        return static_cast<std::uint8_t>(std::clamp(value + modifier, 0, 0xFF));
    };
    return Color{
        .red = channel(rgb[0]), .green = channel(rgb[1]), .blue = channel(rgb[2]), .alpha = 0xFF};
}

// A texel inside its 8x8 tile.
Color DecodeInTile(std::span<const std::uint8_t> tile,
                   TextureFormat format,
                   std::uint32_t x,
                   std::uint32_t y) {
    const std::uint32_t index = MortonIndex(x, y);
    const auto texel = [&](std::size_t size) { return tile.subspan(index * size, size); };
    switch (format) {
    case TextureFormat::Rgba8:
        return DecodePixel(PixelFormat::Rgba8, texel(4));
    case TextureFormat::Rgb8:
        return DecodePixel(PixelFormat::Rgb8, texel(3));
    case TextureFormat::Rgb5A1:
        return DecodePixel(PixelFormat::Rgb5A1, texel(2));
    case TextureFormat::Rgb565:
        return DecodePixel(PixelFormat::Rgb565, texel(2));
    case TextureFormat::Rgba4:
        return DecodePixel(PixelFormat::Rgba4, texel(2));
    case TextureFormat::La8: {
        const std::span<const std::uint8_t> bytes = texel(2);
        return Color{.red = bytes[1], .green = bytes[1], .blue = bytes[1], .alpha = bytes[0]};
    }
    case TextureFormat::HiLo8: {
        const std::span<const std::uint8_t> bytes = texel(2);
        return Color{.red = bytes[1], .green = bytes[0], .blue = 0, .alpha = 0xFF};
    }
    case TextureFormat::L8: {
        const std::uint8_t value = tile[index];
        return Color{.red = value, .green = value, .blue = value, .alpha = 0xFF};
    }
    case TextureFormat::A8:
        return Color{.red = 0, .green = 0, .blue = 0, .alpha = tile[index]};
    case TextureFormat::La4: {
        const std::uint8_t value = Widen4(tile[index] >> 4U);
        return Color{.red = value, .green = value, .blue = value, .alpha = Widen4(tile[index])};
    }
    case TextureFormat::L4:
    case TextureFormat::A4: {
        // Two texels a byte, the first in the low nibble.
        const std::uint8_t byte = tile[index / 2];
        const std::uint8_t value = Widen4(index % 2 == 0 ? byte : byte >> 4U);
        if (format == TextureFormat::A4) {
            return Color{.red = 0, .green = 0, .blue = 0, .alpha = value};
        }
        return Color{.red = value, .green = value, .blue = value, .alpha = 0xFF};
    }
    case TextureFormat::Etc1:
    case TextureFormat::Etc1A4: {
        // Four 4x4 blocks, left to right and then down; ETC1A4 puts 64 bits
        // of alpha, a nibble a pixel in the same order, before each block.
        const bool alpha = format == TextureFormat::Etc1A4;
        const std::size_t block_bytes = alpha ? 16 : 8;
        const std::uint32_t block = (x / 4) + (2 * (y / 4));
        const std::span<const std::uint8_t> bytes = tile.subspan(block * block_bytes, block_bytes);
        const std::uint32_t bx = x % 4;
        const std::uint32_t by = y % 4;
        Color color = DecodeEtc1(Read64(bytes.subspan(alpha ? 8 : 0, 8)), bx, by);
        if (alpha) {
            color.alpha = Widen4(Bits(Read64(bytes.first(8)), 4 * ((4 * bx) + by), 4));
        }
        return color;
    }
    }
    return Color{};
}

}  // namespace

std::size_t TextureTileBytes(TextureFormat format) {
    switch (format) {
    case TextureFormat::Rgba8:
        return std::size_t{4} * TilePixels;
    case TextureFormat::Rgb8:
        return std::size_t{3} * TilePixels;
    case TextureFormat::Rgb5A1:
    case TextureFormat::Rgb565:
    case TextureFormat::Rgba4:
    case TextureFormat::La8:
    case TextureFormat::HiLo8:
        return std::size_t{2} * TilePixels;
    case TextureFormat::L8:
    case TextureFormat::A8:
    case TextureFormat::La4:
    case TextureFormat::Etc1A4:
        return TilePixels;
    case TextureFormat::L4:
    case TextureFormat::A4:
    case TextureFormat::Etc1:
        return TilePixels / 2;
    }
    return 0;
}

std::size_t TextureBytes(TextureFormat format, std::uint32_t width, std::uint32_t height) {
    return TextureTileBytes(format) * (width / TileSide) * (height / TileSide);
}

Color DecodeTexel(std::span<const std::uint8_t> texture,
                  TextureFormat format,
                  std::uint32_t width,
                  std::uint32_t x,
                  std::uint32_t y) {
    const std::size_t tile_bytes = TextureTileBytes(format);
    const std::size_t tile = ((y / TileSide) * (width / TileSide)) + (x / TileSide);
    return DecodeInTile(
        texture.subspan(tile * tile_bytes, tile_bytes), format, x % TileSide, y % TileSide);
}

std::vector<Color> DecodeTexture(std::span<const std::uint8_t> texture,
                                 TextureFormat format,
                                 std::uint32_t width,
                                 std::uint32_t height) {
    std::vector<Color> texels;
    texels.reserve(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            texels.push_back(DecodeTexel(texture, format, width, x, y));
        }
    }
    return texels;
}

std::optional<PixelFormat> ColorBufferFormat(std::uint32_t value) {
    constexpr std::array<PixelFormat, 5> Formats = {PixelFormat::Rgba8,
                                                    PixelFormat::Rgb8,
                                                    PixelFormat::Rgb5A1,
                                                    PixelFormat::Rgb565,
                                                    PixelFormat::Rgba4};
    return value < Formats.size() ? std::optional{Formats.at(value)} : std::nullopt;
}

std::optional<PixelFormat> TransferFormat(std::uint32_t value) {
    constexpr std::array<PixelFormat, 8> Formats = {PixelFormat::Rgba8,
                                                    PixelFormat::Rgb8,
                                                    PixelFormat::Rgb565,
                                                    PixelFormat::Rgb5A1,
                                                    PixelFormat::Rgba4,
                                                    PixelFormat::Rgba4,
                                                    PixelFormat::Rgba4,
                                                    PixelFormat::Rgba4};
    return value < Formats.size() ? std::optional{Formats.at(value)} : std::nullopt;
}

std::size_t BytesPerPixel(PixelFormat format) {
    switch (format) {
    case PixelFormat::Rgba8:
        return 4;
    case PixelFormat::Rgb8:
        return 3;
    case PixelFormat::Rgb5A1:
    case PixelFormat::Rgb565:
    case PixelFormat::Rgba4:
        return 2;
    }
    return 0;
}

namespace {

constexpr std::size_t BytesPerPixelOf(PixelFormat format) {
    return format == PixelFormat::Rgba8 ? 4 : (format == PixelFormat::Rgb8 ? 3 : 2);
}

// Red in the most significant bits, alpha in the least, little-endian.
// Source: GBATEK, Texture Formats.
template<PixelFormat Format>
void EncodeAs(const Color& color, std::span<std::uint8_t> bytes) {
    // Widened before shifting, or each channel would be promoted to int and the
    // packed value would change signedness on its way into Write16.
    const std::uint32_t red = color.red;
    const std::uint32_t green = color.green;
    const std::uint32_t blue = color.blue;
    const std::uint32_t alpha = color.alpha;
    if constexpr (Format == PixelFormat::Rgba8) {
        bytes[0] = color.alpha;
        bytes[1] = color.blue;
        bytes[2] = color.green;
        bytes[3] = color.red;
    } else if constexpr (Format == PixelFormat::Rgb8) {
        bytes[0] = color.blue;
        bytes[1] = color.green;
        bytes[2] = color.red;
    } else if constexpr (Format == PixelFormat::Rgb5A1) {
        Write16(((red >> 3U) << 11U) | ((green >> 3U) << 6U) | ((blue >> 3U) << 1U) | (alpha >> 7U),
                bytes);
    } else if constexpr (Format == PixelFormat::Rgb565) {
        Write16(((red >> 3U) << 11U) | ((green >> 2U) << 5U) | (blue >> 3U), bytes);
    } else {
        Write16(((red >> 4U) << 12U) | ((green >> 4U) << 8U) | ((blue >> 4U) << 4U) | (alpha >> 4U),
                bytes);
    }
}

template<PixelFormat Format>
Color DecodeAs(std::span<const std::uint8_t> bytes) {
    if constexpr (Format == PixelFormat::Rgba8) {
        return Color{.red = bytes[3], .green = bytes[2], .blue = bytes[1], .alpha = bytes[0]};
    } else if constexpr (Format == PixelFormat::Rgb8) {
        return Color{.red = bytes[2], .green = bytes[1], .blue = bytes[0], .alpha = 0xFF};
    } else if constexpr (Format == PixelFormat::Rgb5A1) {
        const std::uint32_t value = Read16(bytes);
        return Color{.red = Widen5(value >> 11U),
                     .green = Widen5((value >> 6U) & 0x1FU),
                     .blue = Widen5((value >> 1U) & 0x1FU),
                     .alpha = Widen1(value)};
    } else if constexpr (Format == PixelFormat::Rgb565) {
        const std::uint32_t value = Read16(bytes);
        return Color{.red = Widen5(value >> 11U),
                     .green = Widen6(value >> 5U),
                     .blue = Widen5(value & 0x1FU),
                     .alpha = 0xFF};
    } else {
        const std::uint32_t value = Read16(bytes);
        return Color{.red = Widen4(value >> 12U),
                     .green = Widen4(value >> 8U),
                     .blue = Widen4(value >> 4U),
                     .alpha = Widen4(value)};
    }
}

template<PixelFormat Format>
void EncodeRun(std::span<const Color> colors, std::span<std::uint8_t> bytes) {
    constexpr std::size_t Size = BytesPerPixelOf(Format);
    for (std::size_t pixel = 0; pixel < colors.size(); ++pixel) {
        EncodeAs<Format>(colors[pixel], bytes.subspan(pixel * Size, Size));
    }
}

template<PixelFormat Format>
void DecodeRun(std::span<const std::uint8_t> bytes, std::span<Color> colors) {
    constexpr std::size_t Size = BytesPerPixelOf(Format);
    for (std::size_t pixel = 0; pixel < colors.size(); ++pixel) {
        colors[pixel] = DecodeAs<Format>(bytes.subspan(pixel * Size, Size));
    }
}

}  // namespace

void EncodePixel(PixelFormat format, const Color& color, std::span<std::uint8_t> bytes) {
    switch (format) {
    case PixelFormat::Rgba8:
        return EncodeAs<PixelFormat::Rgba8>(color, bytes);
    case PixelFormat::Rgb8:
        return EncodeAs<PixelFormat::Rgb8>(color, bytes);
    case PixelFormat::Rgb5A1:
        return EncodeAs<PixelFormat::Rgb5A1>(color, bytes);
    case PixelFormat::Rgb565:
        return EncodeAs<PixelFormat::Rgb565>(color, bytes);
    case PixelFormat::Rgba4:
        return EncodeAs<PixelFormat::Rgba4>(color, bytes);
    }
}

Color DecodePixel(PixelFormat format, std::span<const std::uint8_t> bytes) {
    switch (format) {
    case PixelFormat::Rgba8:
        return DecodeAs<PixelFormat::Rgba8>(bytes);
    case PixelFormat::Rgb8:
        return DecodeAs<PixelFormat::Rgb8>(bytes);
    case PixelFormat::Rgb5A1:
        return DecodeAs<PixelFormat::Rgb5A1>(bytes);
    case PixelFormat::Rgb565:
        return DecodeAs<PixelFormat::Rgb565>(bytes);
    case PixelFormat::Rgba4:
        return DecodeAs<PixelFormat::Rgba4>(bytes);
    }
    return Color{};
}

void EncodePixels(PixelFormat format,
                  std::span<const Color> colors,
                  std::span<std::uint8_t> bytes) {
    switch (format) {
    case PixelFormat::Rgba8:
        return EncodeRun<PixelFormat::Rgba8>(colors, bytes);
    case PixelFormat::Rgb8:
        return EncodeRun<PixelFormat::Rgb8>(colors, bytes);
    case PixelFormat::Rgb5A1:
        return EncodeRun<PixelFormat::Rgb5A1>(colors, bytes);
    case PixelFormat::Rgb565:
        return EncodeRun<PixelFormat::Rgb565>(colors, bytes);
    case PixelFormat::Rgba4:
        return EncodeRun<PixelFormat::Rgba4>(colors, bytes);
    }
}

void DecodePixels(PixelFormat format,
                  std::span<const std::uint8_t> bytes,
                  std::span<Color> colors) {
    switch (format) {
    case PixelFormat::Rgba8:
        return DecodeRun<PixelFormat::Rgba8>(bytes, colors);
    case PixelFormat::Rgb8:
        return DecodeRun<PixelFormat::Rgb8>(bytes, colors);
    case PixelFormat::Rgb5A1:
        return DecodeRun<PixelFormat::Rgb5A1>(bytes, colors);
    case PixelFormat::Rgb565:
        return DecodeRun<PixelFormat::Rgb565>(bytes, colors);
    case PixelFormat::Rgba4:
        return DecodeRun<PixelFormat::Rgba4>(bytes, colors);
    }
}

namespace {

bool HoldsWholeTiles(std::uint32_t width,
                     std::uint32_t height,
                     std::size_t tiled_size,
                     std::size_t rgba_size,
                     PixelFormat format) {
    return width % TileSide == 0 && height % TileSide == 0 &&
           tiled_size >= std::size_t{width} * height * BytesPerPixel(format) &&
           rgba_size >= std::size_t{width} * height * 4;
}

}  // namespace

bool DecodeTiledImage(PixelFormat format,
                      std::uint32_t width,
                      std::uint32_t height,
                      std::span<const std::uint8_t> tiled,
                      std::span<std::uint8_t> rgba) {
    if (!HoldsWholeTiles(width, height, tiled.size(), rgba.size(), format)) {
        return false;
    }
    const std::size_t bytes = BytesPerPixel(format);
    std::array<Color, TilePixels> tile{};
    for (std::uint32_t tile_y = 0; tile_y < height; tile_y += TileSide) {
        for (std::uint32_t tile_x = 0; tile_x < width; tile_x += TileSide) {
            DecodePixels(
                format,
                tiled.subspan(TiledOffset(tile_x, tile_y, width, bytes), TilePixels * bytes),
                tile);
            for (std::uint32_t at = 0; at < TilePixels; ++at) {
                const video::TilePosition place = video::MortonPosition(at);
                const std::size_t pixel =
                    ((std::size_t{tile_y + place.y} * width) + tile_x + place.x) * 4;
                rgba[pixel] = tile.at(at).red;
                rgba[pixel + 1] = tile.at(at).green;
                rgba[pixel + 2] = tile.at(at).blue;
                rgba[pixel + 3] = tile.at(at).alpha;
            }
        }
    }
    return true;
}

bool EncodeTiledImage(PixelFormat format,
                      std::uint32_t width,
                      std::uint32_t height,
                      std::span<const std::uint8_t> rgba,
                      std::span<std::uint8_t> tiled) {
    if (!HoldsWholeTiles(width, height, tiled.size(), rgba.size(), format)) {
        return false;
    }
    const std::size_t bytes = BytesPerPixel(format);
    std::array<Color, TilePixels> tile{};
    for (std::uint32_t tile_y = 0; tile_y < height; tile_y += TileSide) {
        for (std::uint32_t tile_x = 0; tile_x < width; tile_x += TileSide) {
            for (std::uint32_t at = 0; at < TilePixels; ++at) {
                const video::TilePosition place = video::MortonPosition(at);
                const std::size_t pixel =
                    ((std::size_t{tile_y + place.y} * width) + tile_x + place.x) * 4;
                tile.at(at) = Color{.red = rgba[pixel],
                                    .green = rgba[pixel + 1],
                                    .blue = rgba[pixel + 2],
                                    .alpha = rgba[pixel + 3]};
            }
            EncodePixels(
                format,
                tile,
                tiled.subspan(TiledOffset(tile_x, tile_y, width, bytes), TilePixels * bytes));
        }
    }
    return true;
}

std::optional<DepthFormat> DepthBufferFormat(std::uint32_t value) {
    switch (value) {
    case 0:
        return DepthFormat::D16;
    case 2:
        return DepthFormat::D24;
    case 3:
        return DepthFormat::D24S8;
    default:
        return std::nullopt;
    }
}

std::size_t BytesPerPixel(DepthFormat format) {
    switch (format) {
    case DepthFormat::D16:
        return 2;
    case DepthFormat::D24:
        return 3;
    case DepthFormat::D24S8:
        return 4;
    }
    return 0;
}

std::uint32_t MaximumDepth(DepthFormat format) {
    return format == DepthFormat::D16 ? 0xFFFFU : 0xFFFFFFU;
}

std::uint32_t DecodeDepth(DepthFormat format, std::span<const std::uint8_t> bytes) {
    if (format == DepthFormat::D16) {
        return Read16(bytes);
    }
    return bytes[0] | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16);
}

std::uint8_t DecodeStencil(std::span<const std::uint8_t> bytes) {
    return bytes[3];
}

void EncodeDepth(DepthFormat format, std::uint32_t depth, std::span<std::uint8_t> bytes) {
    bytes[0] = static_cast<std::uint8_t>(depth);
    bytes[1] = static_cast<std::uint8_t>(depth >> 8);
    if (format != DepthFormat::D16) {
        bytes[2] = static_cast<std::uint8_t>(depth >> 16);
    }
}

void EncodeStencil(std::uint8_t stencil, std::span<std::uint8_t> bytes) {
    bytes[3] = stencil;
}

std::size_t
TiledOffset(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::size_t bytes_per_pixel) {
    const std::size_t tile_row = static_cast<std::size_t>(y / TileSide) * TileSide * width;
    const std::size_t tile = static_cast<std::size_t>(x / TileSide) * TilePixels;
    return (tile_row + tile + MortonIndex(x % TileSide, y % TileSide)) * bytes_per_pixel;
}

}  // namespace hexlith::domain::gpu
