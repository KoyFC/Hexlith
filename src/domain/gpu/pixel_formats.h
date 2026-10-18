#ifndef HEXLITH_DOMAIN_GPU_PIXEL_FORMATS_H
#define HEXLITH_DOMAIN_GPU_PIXEL_FORMATS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hexlith::domain::gpu {

// How the GPU lays pixels out in memory: textures, colour and depth buffers,
// and the pixels display transfers convert between. docs/formats/
// pica-pixels.md has the sources for every layout.

struct Color {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 0;

    bool operator==(const Color&) const = default;
};

// Texture formats by GPUREG_TEXUNITi_TYPE's value. Source: GBATEK, 3DS GPU
// Texture Formats.
enum class TextureFormat : std::uint8_t {
    Rgba8 = 0x0,
    Rgb8 = 0x1,
    Rgb5A1 = 0x2,
    Rgb565 = 0x3,
    Rgba4 = 0x4,
    La8 = 0x5,
    HiLo8 = 0x6,
    L8 = 0x7,
    A8 = 0x8,
    La4 = 0x9,
    L4 = 0xA,
    A4 = 0xB,
    Etc1 = 0xC,
    Etc1A4 = 0xD,
};

constexpr std::uint32_t TextureFormatCount = 14;

// The bytes an 8x8 tile of the format takes.
std::size_t TextureTileBytes(TextureFormat format);

// The bytes a texture of the format and size takes, its sides multiples of
// eight.
std::size_t TextureBytes(TextureFormat format, std::uint32_t width, std::uint32_t height);

// The texel at (x, y) of a texture, y counting the rows in the order they
// are stored. Textures are stored in 8x8 tiles, the tiles a row at a time
// and each tile in Morton order; ETC1 tiles are four 4x4 blocks instead.
// The bytes must hold the whole texture.
Color DecodeTexel(std::span<const std::uint8_t> texture,
                  TextureFormat format,
                  std::uint32_t width,
                  std::uint32_t x,
                  std::uint32_t y);

// A whole texture as rows of texels, in stored order.
std::vector<Color> DecodeTexture(std::span<const std::uint8_t> texture,
                                 TextureFormat format,
                                 std::uint32_t width,
                                 std::uint32_t height);

// The formats of colour buffers, display transfers and the LCD.
enum class PixelFormat : std::uint8_t { Rgba8, Rgb8, Rgb5A1, Rgb565, Rgba4 };

// GPUREG_COLORBUFFER_FORMAT bits 16 to 18. Source: GBATEK, Framebuffer
// registers; Azahar's ColorFormat, which also has RGB8 at 1.
std::optional<PixelFormat> ColorBufferFormat(std::uint32_t value);

// A display transfer's formats and the LCD's, in their own order: RGB565 is 2
// and RGB5A1 3. Source: GBATEK, Memfill and Memcopy; Azahar's PixelFormat.
std::optional<PixelFormat> TransferFormat(std::uint32_t value);

std::size_t BytesPerPixel(PixelFormat format);

// A pixel's bytes in memory order, and back. Narrower channels keep their
// top bits, and widen by repeating them. Source: GBATEK, Texture Formats, for
// the bit order; Azahar's Encode and Decode functions in src/common/color.h
// for what a narrowing does.
void EncodePixel(PixelFormat format, const Color& color, std::span<std::uint8_t> bytes);
Color DecodePixel(PixelFormat format, std::span<const std::uint8_t> bytes);

// Depth buffer formats by GPUREG_DEPTHBUFFER_FORMAT's value. Source: GBATEK,
// Framebuffer registers.
enum class DepthFormat : std::uint8_t { D16 = 0, D24 = 2, D24S8 = 3 };

std::optional<DepthFormat> DepthBufferFormat(std::uint32_t value);

std::size_t BytesPerPixel(DepthFormat format);

// The largest depth the format holds: 0xFFFF or 0xFFFFFF.
std::uint32_t MaximumDepth(DepthFormat format);

// Depth in the low bytes, little-endian; D24S8 has the stencil in the fourth.
std::uint32_t DecodeDepth(DepthFormat format, std::span<const std::uint8_t> bytes);
std::uint8_t DecodeStencil(std::span<const std::uint8_t> bytes);
void EncodeDepth(DepthFormat format, std::uint32_t depth, std::span<std::uint8_t> bytes);
void EncodeStencil(std::uint8_t stencil, std::span<std::uint8_t> bytes);

// Where pixel (x, y) of a tiled buffer `width` pixels wide starts, in bytes:
// 8x8 tiles a row at a time, each in Morton order, as textures and colour
// and depth buffers are laid out. Source: GBATEK, Video Texture Swizzling;
// GetMortonOffset in Azahar's video_core/utils.h.
std::size_t
TiledOffset(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::size_t bytes_per_pixel);

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_PIXEL_FORMATS_H
