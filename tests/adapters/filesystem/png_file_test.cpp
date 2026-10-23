#include "adapters/filesystem/png_file.h"

#include <gtest/gtest.h>
#include <vector>

namespace hexlith::adapters::filesystem {
namespace {

std::uint32_t BigEndian(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t byte = 0; byte < 4; ++byte) {
        value = (value << 8U) | bytes.at(at + byte);
    }
    return value;
}

// Two pixels whose channels count up from 1, so that the stored row reads
// 0 (the filter byte) to 6.
domain::gpu::Picture TwoByOne() {
    domain::gpu::Picture picture{.width = 2, .height = 1, .pixels = {}};
    std::uint8_t next = 1;
    for (std::size_t pixel = 0; pixel < 2; ++pixel) {
        domain::gpu::Color color;
        color.red = next++;
        color.green = next++;
        color.blue = next++;
        picture.pixels.push_back(color);
    }
    return picture;
}

TEST(EncodePng, StartsWithTheSignatureAndTheHeader) {
    const std::vector<std::uint8_t> png = EncodePng(TwoByOne());
    EXPECT_EQ(std::vector<std::uint8_t>(png.begin(), png.begin() + 8),
              (std::vector<std::uint8_t>{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'}));
    EXPECT_EQ(BigEndian(png, 8), 13U) << "the header's length";
    EXPECT_EQ(std::string(png.begin() + 12, png.begin() + 16), "IHDR");
    EXPECT_EQ(BigEndian(png, 16), 2U);
    EXPECT_EQ(BigEndian(png, 20), 1U);
    // CRC of "IHDR" and its data, from a reference implementation (zlib's
    // crc32 of these 17 bytes).
    EXPECT_EQ(BigEndian(png, 29), 0x7B40E8DDU);
}

TEST(EncodePng, StoresTheRowsUncompressed) {
    const std::vector<std::uint8_t> png = EncodePng(TwoByOne());
    // IDAT after the 33 bytes of signature and header: length, type, then
    // the zlib header, one final stored block of seven bytes, the row.
    const std::size_t idat = 33;
    EXPECT_EQ(std::string(png.begin() + idat + 4, png.begin() + idat + 8), "IDAT");
    const std::size_t data = idat + 8;
    EXPECT_EQ(png.at(data), 0x78);
    EXPECT_EQ(png.at(data + 2), 1) << "final, stored";
    EXPECT_EQ(png.at(data + 3), 7);
    EXPECT_EQ(png.at(data + 4), 0);
    EXPECT_EQ(std::vector<std::uint8_t>(png.begin() + static_cast<std::ptrdiff_t>(data + 7),
                                        png.begin() + static_cast<std::ptrdiff_t>(data + 14)),
              (std::vector<std::uint8_t>{0, 1, 2, 3, 4, 5, 6}));
}

}  // namespace
}  // namespace hexlith::adapters::filesystem
