#include "domain/gpu/texture_cache.h"

#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::gpu {
namespace {

constexpr std::uint32_t Address = 0x18001000;
constexpr std::uint32_t Side = 8;

std::vector<std::uint8_t> Texture(std::uint8_t fill) {
    return std::vector<std::uint8_t>(TextureBytes(TextureFormat::Rgba8, Side, Side), fill);
}

TEST(TextureCache, DecodesATextureOnceWhileItsBytesStay) {
    TextureCache cache;
    const std::vector<std::uint8_t> bytes = Texture(0x40);

    const std::span<const std::uint32_t> first =
        cache.Texels(Address, TextureFormat::Rgba8, Side, Side, bytes);
    ASSERT_EQ(first.size(), std::size_t{Side} * Side);
    const std::uint32_t texel = first.front();
    const std::span<const std::uint32_t> second =
        cache.Texels(Address, TextureFormat::Rgba8, Side, Side, bytes);

    EXPECT_EQ(cache.Decoded(), 1U);
    EXPECT_EQ(second.front(), texel);
}

TEST(TextureCache, DecodesAgainWhenTheBytesChange) {
    TextureCache cache;
    const std::uint32_t before =
        cache.Texels(Address, TextureFormat::Rgba8, Side, Side, Texture(0x40)).front();
    const std::uint32_t after =
        cache.Texels(Address, TextureFormat::Rgba8, Side, Side, Texture(0x41)).front();

    EXPECT_EQ(cache.Decoded(), 2U);
    EXPECT_NE(before, after);
}

// The last byte is where a comparison that stops early would go wrong.
TEST(TextureCache, NoticesAChangeInTheLastByte) {
    TextureCache cache;
    std::vector<std::uint8_t> bytes = Texture(0x40);
    static_cast<void>(cache.Texels(Address, TextureFormat::Rgba8, Side, Side, bytes));
    bytes.back() = 0x99;
    static_cast<void>(cache.Texels(Address, TextureFormat::Rgba8, Side, Side, bytes));

    EXPECT_EQ(cache.Decoded(), 2U);
}

TEST(TextureCache, KeepsTexturesApartByAddressAndFormat) {
    TextureCache cache;
    const std::vector<std::uint8_t> bytes = Texture(0x40);
    static_cast<void>(cache.Texels(Address, TextureFormat::Rgba8, Side, Side, bytes));
    static_cast<void>(cache.Texels(Address + 0x1000, TextureFormat::Rgba8, Side, Side, bytes));
    static_cast<void>(cache.Texels(Address, TextureFormat::Rgb8, Side, Side, Texture(0x40)));

    EXPECT_EQ(cache.Decoded(), 3U);
}

TEST(TextureCache, ForgetsWhatDoesNotFitInItsLimit) {
    TextureCache cache{/*limit_texels=*/Side * Side};
    const std::vector<std::uint8_t> bytes = Texture(0x40);
    static_cast<void>(cache.Texels(Address, TextureFormat::Rgba8, Side, Side, bytes));
    static_cast<void>(cache.Texels(Address + 0x1000, TextureFormat::Rgba8, Side, Side, bytes));
    static_cast<void>(cache.Texels(Address, TextureFormat::Rgba8, Side, Side, bytes));

    EXPECT_EQ(cache.Decoded(), 3U) << "the first was dropped to make room for the second";
}

}  // namespace
}  // namespace hexlith::domain::gpu
