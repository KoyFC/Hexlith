#ifndef HEXLITH_DOMAIN_SERVICES_STAND_IN_FONT_H
#define HEXLITH_DOMAIN_SERVICES_STAND_IN_FONT_H

#include <cstdint>
#include <span>

namespace hexlith::domain::services {

// The size of the block APT hands out for the shared font. Source: the shared
// memory APT creates for it in Azahar, src/core/hle/service/apt/apt.cpp.
constexpr std::uint32_t SharedFontBlockSize = 0x332000;

// Writes a stand-in for the console's shared font into a block mapped at the
// given address: the 0x80-byte header APT puts before the font, then a font in
// the BCFNT format with one blank glyph that every character maps to.
//
// The console's font is a system title and not part of the cartridge, and the
// reference title stops at once without one. This font is built here from the
// format alone, 3dbrew's BCFNT page and the structures Azahar reads; it holds
// no byte of any font Nintendo ships. Text drawn with it is blank. ADR 0014
// records why it exists.
//
// The font is already relocated, as APT leaves the real one: its magic is
// CFNU, and every offset inside it is an absolute address in the block.
void WriteStandInFont(std::span<std::uint8_t> block, std::uint32_t address);

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_STAND_IN_FONT_H
