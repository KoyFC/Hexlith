#ifndef HEXLITH_DOMAIN_VIDEO_MORTON_H
#define HEXLITH_DOMAIN_VIDEO_MORTON_H

#include <cstdint>

namespace hexlith::domain::video {

// The pixels of an 8x8 tile, as the GPU stores textures and as the Y2R unit
// can write its output.
constexpr std::uint32_t TileSide = 8;
constexpr std::uint32_t TilePixels = TileSide * TileSide;

// Where the pixel at (x, y) of an 8x8 tile sits in the tile's storage: the
// bits of x and y interleaved, x's lowest first, so that the tile is four
// 4x4 quarters, each four 2x2 blocks, each four pixels in the order
// top-left, top-right, bottom-left, bottom-right. Source: GBATEK, 3DS Video
// Texture Swizzling.
constexpr std::uint32_t MortonIndex(std::uint32_t x, std::uint32_t y) {
    std::uint32_t index = 0;
    for (std::uint32_t bit = 0; bit < 3; ++bit) {
        index |= ((x >> bit) & 1U) << (2 * bit);
        index |= ((y >> bit) & 1U) << ((2 * bit) + 1);
    }
    return index;
}

}  // namespace hexlith::domain::video

#endif  // HEXLITH_DOMAIN_VIDEO_MORTON_H
