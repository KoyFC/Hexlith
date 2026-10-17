#include "domain/services/stand_in_font.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::domain::services {
namespace {

constexpr std::uint32_t Address = 0x18000000;
constexpr std::uint32_t FontOffset = 0x80;

// Room for the font, and what the block holds before it is written.
constexpr std::size_t BlockSize = 0x1000;
constexpr std::uint8_t Filler = 0xAA;

std::uint32_t WordAt(const std::vector<std::uint8_t>& block, std::uint32_t offset) {
    std::uint32_t value = 0;
    for (unsigned byte = 0; byte < 4; ++byte) {
        value |= static_cast<std::uint32_t>(block.at(offset + byte)) << (8U * byte);
    }
    return value;
}

std::string MagicAt(const std::vector<std::uint8_t>& block, std::uint32_t offset) {
    return {block.begin() + offset, block.begin() + offset + 4};
}

// A section pointer in FINF is an address eight bytes into the section, past
// its magic and size.
std::string SectionAt(const std::vector<std::uint8_t>& block, std::uint32_t pointer) {
    return MagicAt(block, pointer - Address - 8);
}

std::vector<std::uint8_t> WrittenFont() {
    std::vector<std::uint8_t> block(BlockSize, Filler);
    WriteStandInFont(block, Address);
    return block;
}

TEST(StandInFont, StartsWithTheHeaderAptWrites) {
    const std::vector<std::uint8_t> block = WrittenFont();
    EXPECT_EQ(WordAt(block, 0), 2U) << "loaded";
    EXPECT_EQ(WordAt(block, 8), WordAt(block, FontOffset + 0xC)) << "the font's size";
}

TEST(StandInFont, IsARelocatedFontWithFourSections) {
    const std::vector<std::uint8_t> block = WrittenFont();
    EXPECT_EQ(MagicAt(block, FontOffset), "CFNU");
    EXPECT_EQ(WordAt(block, FontOffset + 0x10), 4U);
    const std::uint32_t info = FontOffset + 0x14;
    EXPECT_EQ(MagicAt(block, info), "FINF");
    EXPECT_EQ(SectionAt(block, WordAt(block, info + 0x10)), "TGLP");
    EXPECT_EQ(SectionAt(block, WordAt(block, info + 0x14)), "CWDH");
    EXPECT_EQ(SectionAt(block, WordAt(block, info + 0x18)), "CMAP");
}

TEST(StandInFont, HasOneBlankGlyphSheet) {
    const std::vector<std::uint8_t> block = WrittenFont();
    const std::uint32_t glyphs = WordAt(block, FontOffset + 0x14 + 0x10) - Address;
    const std::uint32_t sheet = WordAt(block, glyphs + 0x14) - Address;
    const std::uint32_t sheet_size = WordAt(block, glyphs + 0x4);
    ASSERT_LE(sheet + sheet_size, block.size());
    for (std::uint32_t offset = 0; offset < sheet_size; ++offset) {
        EXPECT_EQ(block.at(sheet + offset), 0) << "at " << offset;
    }
}

}  // namespace
}  // namespace hexlith::domain::services
