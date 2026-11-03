#ifndef HEXLITH_DOMAIN_GPU_TEXTURE_CACHE_H
#define HEXLITH_DOMAIN_GPU_TEXTURE_CACHE_H

#include "domain/gpu/pixel_formats.h"

#include <cstdint>
#include <map>
#include <span>
#include <tuple>
#include <vector>

namespace hexlith::domain::gpu {

// Textures decoded to RGBA8 texels, kept for as long as their bytes in memory
// stay what they were. A texture is found by where it lies, its format and its
// size, and trusted only after its bytes are compared with the copy kept from
// when it was decoded, which costs a memory comparison where hashing them
// would cost a pass byte by byte. ADR 0030 says why textures are decoded here
// and not on the device.
class TextureCache {
public:
    // The most texels kept at once: a texture is a few hundred thousand at
    // most, and the title uses a few dozen at a time.
    static constexpr std::size_t DefaultLimitTexels = 16U * 1024U * 1024U;

    explicit TextureCache(std::size_t limit_texels = DefaultLimitTexels)
        : limit_texels_(limit_texels) {}

    // The texture's texels, red in the low byte, as long as it is not looked
    // up again with another texture in the cache's way.
    [[nodiscard]] std::span<const std::uint32_t> Texels(std::uint32_t address,
                                                        TextureFormat format,
                                                        std::uint32_t width,
                                                        std::uint32_t height,
                                                        std::span<const std::uint8_t> bytes);

    // How many textures were decoded, not found.
    [[nodiscard]] std::uint64_t Decoded() const {
        return decoded_;
    }

private:
    struct Entry {
        std::vector<std::uint8_t> bytes;
        std::vector<std::uint32_t> texels;
    };

    using Key = std::tuple<std::uint32_t, TextureFormat, std::uint32_t, std::uint32_t>;

    std::size_t limit_texels_;
    std::size_t held_texels_ = 0;
    std::uint64_t decoded_ = 0;
    std::map<Key, Entry> entries_;
};

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_TEXTURE_CACHE_H
