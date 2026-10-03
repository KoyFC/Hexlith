#ifndef HEXLITH_CORE_TEXT_H
#define HEXLITH_CORE_TEXT_H

#include "core/byte_reader.h"
#include "core/result.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::core {

// RomFS stores every name as UTF-16LE, so an extractor has to transcode and
// cannot assume one byte per character.
//
// Both directions are here because they are the same table read twice: the
// decoder is what the RomFS parser needs, and the encoder is what lets a
// synthetic filesystem be built with names worth testing against.
//
// Malformed input is refused rather than replaced with a substitution
// character. A name that does not decode means the bytes are not a name, and
// writing a file called "???" would turn that into a file nobody can trace
// back to anything.
Result<std::string> Utf16LeToUtf8(ByteSpan utf16);

Result<std::vector<std::uint8_t>> Utf8ToUtf16Le(std::string_view utf8);

}  // namespace hexlith::core

#endif  // HEXLITH_CORE_TEXT_H
