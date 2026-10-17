#include "domain/services/stand_in_font.h"

#include <algorithm>
#include <string_view>

namespace hexlith::domain::services {
namespace {

// Where each part sits in the block. The header APT writes is 0x80 bytes and
// the font follows it; the sections are laid out one after another, and the
// glyph sheet, being a texture, starts on a 0x80-byte boundary.
constexpr std::uint32_t FontOffset = 0x80;
constexpr std::uint32_t FontHeaderSize = 0x14;
constexpr std::uint32_t InfoOffset = FontOffset + FontHeaderSize;
constexpr std::uint32_t InfoSize = 0x20;
constexpr std::uint32_t GlyphsOffset = InfoOffset + InfoSize;
constexpr std::uint32_t GlyphsSize = 0x20;
constexpr std::uint32_t WidthsOffset = GlyphsOffset + GlyphsSize;
constexpr std::uint32_t WidthsSize = 0x14;
constexpr std::uint32_t MapOffset = WidthsOffset + WidthsSize;
// The CMAP header and, for a direct map, one halfword of data, padded to a
// word.
constexpr std::uint32_t MapHeaderSize = 0x14;
constexpr std::uint32_t MapSize = 0x18;
constexpr std::uint32_t SheetOffset = 0x180;
constexpr std::uint32_t SectionCount = 4;

// A section's offset in FINF points past its eight-byte magic and size.
constexpr std::uint32_t SectionHeaderSize = 8;

// The APT header: loaded, for region 1.
constexpr std::uint32_t Loaded = 2;
constexpr std::uint32_t FontRegion = 1;

// The font header.
constexpr std::uint16_t LittleEndian = 0xFEFF;
constexpr std::uint32_t Version = 0x03000000;

// FINF: a texture font, encoded in UTF-16, eight pixels square with a line
// feed of ten, every unmapped character drawn as glyph 0.
constexpr std::uint8_t TextureFont = 1;
constexpr std::uint8_t Utf16 = 1;
constexpr std::uint8_t LineFeed = 10;
constexpr std::uint8_t CellSize = 8;
constexpr std::uint8_t Ascent = 7;

// TGLP: one sheet of 8 by 8 four-bit alpha texels, one glyph in it.
constexpr std::uint16_t AlphaFourBits = 11;
constexpr std::uint32_t SheetBytes = CellSize * CellSize / 2;

// CMAP: the space character mapped directly to glyph 0.
constexpr std::uint16_t Space = 0x20;
constexpr std::uint16_t DirectMapping = 0;

// Field offsets, from the start of each structure. Source: 3dbrew, BCFNT;
// CFNT, FINF, TGLP, CWDH and CMAP in Azahar's shared_font.h.
namespace apt_header {
constexpr std::uint32_t Status = 0x0;
constexpr std::uint32_t Region = 0x4;
constexpr std::uint32_t Size = 0x8;
}  // namespace apt_header

namespace font_header {
constexpr std::uint32_t ByteOrder = 0x4;
constexpr std::uint32_t HeaderSize = 0x6;
constexpr std::uint32_t Version = 0x8;
constexpr std::uint32_t FileSize = 0xC;
constexpr std::uint32_t Sections = 0x10;
}  // namespace font_header

namespace finf {
constexpr std::uint32_t FontType = 0x8;
constexpr std::uint32_t LineFeed = 0x9;
constexpr std::uint32_t AlternateGlyph = 0xA;
constexpr std::uint32_t DefaultLeft = 0xC;
constexpr std::uint32_t DefaultGlyphWidth = 0xD;
constexpr std::uint32_t DefaultCharacterWidth = 0xE;
constexpr std::uint32_t Encoding = 0xF;
constexpr std::uint32_t Glyphs = 0x10;
constexpr std::uint32_t Widths = 0x14;
constexpr std::uint32_t Map = 0x18;
constexpr std::uint32_t Height = 0x1C;
constexpr std::uint32_t Width = 0x1D;
constexpr std::uint32_t Ascent = 0x1E;
}  // namespace finf

namespace tglp {
constexpr std::uint32_t CellWidth = 0x8;
constexpr std::uint32_t CellHeight = 0x9;
constexpr std::uint32_t Baseline = 0xA;
constexpr std::uint32_t MaximumWidth = 0xB;
constexpr std::uint32_t SheetSize = 0xC;
constexpr std::uint32_t SheetCount = 0x10;
constexpr std::uint32_t SheetFormat = 0x12;
constexpr std::uint32_t Columns = 0x14;
constexpr std::uint32_t Rows = 0x16;
constexpr std::uint32_t SheetWidth = 0x18;
constexpr std::uint32_t SheetHeight = 0x1A;
constexpr std::uint32_t SheetData = 0x1C;
}  // namespace tglp

namespace cwdh {
constexpr std::uint32_t FirstGlyph = 0x8;
constexpr std::uint32_t LastGlyph = 0xA;
constexpr std::uint32_t Next = 0xC;
constexpr std::uint32_t Left = 0x10;
constexpr std::uint32_t GlyphWidth = 0x11;
constexpr std::uint32_t CharacterWidth = 0x12;
}  // namespace cwdh

namespace cmap {
constexpr std::uint32_t FirstCode = 0x8;
constexpr std::uint32_t LastCode = 0xA;
constexpr std::uint32_t Method = 0xC;
constexpr std::uint32_t Next = 0x10;
}  // namespace cmap

void Put8(std::span<std::uint8_t> block, std::uint32_t offset, std::uint8_t value) {
    block[offset] = value;
}

void Put16(std::span<std::uint8_t> block, std::uint32_t offset, std::uint16_t value) {
    block[offset] = static_cast<std::uint8_t>(value);
    block[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
}

void Put32(std::span<std::uint8_t> block, std::uint32_t offset, std::uint32_t value) {
    Put16(block, offset, static_cast<std::uint16_t>(value));
    Put16(block, offset + 2, static_cast<std::uint16_t>(value >> 16U));
}

void PutMagic(std::span<std::uint8_t> block, std::uint32_t offset, std::string_view magic) {
    for (std::size_t index = 0; index < magic.size(); ++index) {
        block[offset + index] = static_cast<std::uint8_t>(magic[index]);
    }
}

void PutSection(std::span<std::uint8_t> block,
                std::uint32_t offset,
                std::string_view magic,
                std::uint32_t size) {
    PutMagic(block, offset, magic);
    Put32(block, offset + 4, size);
}

}  // namespace

void WriteStandInFont(std::span<std::uint8_t> block, std::uint32_t address) {
    std::ranges::fill(block.first(SheetOffset + SheetBytes), 0);
    const std::uint32_t file_size = SheetOffset + SheetBytes - FontOffset;

    Put32(block, apt_header::Status, Loaded);
    Put32(block, apt_header::Region, FontRegion);
    Put32(block, apt_header::Size, file_size);

    PutMagic(block, FontOffset, "CFNU");
    Put16(block, FontOffset + font_header::ByteOrder, LittleEndian);
    Put16(block, FontOffset + font_header::HeaderSize, FontHeaderSize);
    Put32(block, FontOffset + font_header::Version, Version);
    Put32(block, FontOffset + font_header::FileSize, file_size);
    Put32(block, FontOffset + font_header::Sections, SectionCount);

    PutSection(block, InfoOffset, "FINF", InfoSize);
    Put8(block, InfoOffset + finf::FontType, TextureFont);
    Put8(block, InfoOffset + finf::LineFeed, LineFeed);
    Put16(block, InfoOffset + finf::AlternateGlyph, 0);
    Put8(block, InfoOffset + finf::DefaultLeft, 0);
    Put8(block, InfoOffset + finf::DefaultGlyphWidth, CellSize);
    Put8(block, InfoOffset + finf::DefaultCharacterWidth, CellSize);
    Put8(block, InfoOffset + finf::Encoding, Utf16);
    Put32(block, InfoOffset + finf::Glyphs, address + GlyphsOffset + SectionHeaderSize);
    Put32(block, InfoOffset + finf::Widths, address + WidthsOffset + SectionHeaderSize);
    Put32(block, InfoOffset + finf::Map, address + MapOffset + SectionHeaderSize);
    Put8(block, InfoOffset + finf::Height, CellSize);
    Put8(block, InfoOffset + finf::Width, CellSize);
    Put8(block, InfoOffset + finf::Ascent, Ascent);

    PutSection(block, GlyphsOffset, "TGLP", GlyphsSize);
    Put8(block, GlyphsOffset + tglp::CellWidth, CellSize);
    Put8(block, GlyphsOffset + tglp::CellHeight, CellSize);
    Put8(block, GlyphsOffset + tglp::Baseline, Ascent);
    Put8(block, GlyphsOffset + tglp::MaximumWidth, CellSize);
    Put32(block, GlyphsOffset + tglp::SheetSize, SheetBytes);
    Put16(block, GlyphsOffset + tglp::SheetCount, 1);
    Put16(block, GlyphsOffset + tglp::SheetFormat, AlphaFourBits);
    Put16(block, GlyphsOffset + tglp::Columns, 1);
    Put16(block, GlyphsOffset + tglp::Rows, 1);
    Put16(block, GlyphsOffset + tglp::SheetWidth, CellSize);
    Put16(block, GlyphsOffset + tglp::SheetHeight, CellSize);
    Put32(block, GlyphsOffset + tglp::SheetData, address + SheetOffset);

    // One width entry, glyph 0: no left bearing, as wide as its cell.
    PutSection(block, WidthsOffset, "CWDH", WidthsSize);
    Put16(block, WidthsOffset + cwdh::FirstGlyph, 0);
    Put16(block, WidthsOffset + cwdh::LastGlyph, 0);
    Put32(block, WidthsOffset + cwdh::Next, 0);
    Put8(block, WidthsOffset + cwdh::Left, 0);
    Put8(block, WidthsOffset + cwdh::GlyphWidth, CellSize);
    Put8(block, WidthsOffset + cwdh::CharacterWidth, CellSize);

    PutSection(block, MapOffset, "CMAP", MapSize);
    Put16(block, MapOffset + cmap::FirstCode, Space);
    Put16(block, MapOffset + cmap::LastCode, Space);
    Put16(block, MapOffset + cmap::Method, DirectMapping);
    Put32(block, MapOffset + cmap::Next, 0);
    // The glyph index of the first code, which is the data of a direct map.
    Put16(block, MapOffset + MapHeaderSize, 0);
}

}  // namespace hexlith::domain::services
