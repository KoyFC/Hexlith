#ifndef HEXLITH_PORTS_ROM_SOURCE_H
#define HEXLITH_PORTS_ROM_SOURCE_H

#include "core/byte_reader.h"
#include "core/result.h"

#include <string>

namespace hexlith::ports {

// Where a cartridge image is read from.
//
// The whole image is handed over as a span, which in practice means the
// adapter memory maps it: the file is two gigabytes and the parsers read
// scattered offsets across all of it, so streaming would mean either holding
// it all in memory anyway or reopening it constantly.
//
// The image is read-only input. Nothing in this interface can write to it,
// which is the first of the two places ADR 0008 makes that structural rather
// than a rule to remember.
class RomSource {
public:
    RomSource() = default;
    virtual ~RomSource() = default;

    RomSource(const RomSource&) = delete;
    RomSource& operator=(const RomSource&) = delete;
    RomSource(RomSource&&) = delete;
    RomSource& operator=(RomSource&&) = delete;

    [[nodiscard]] virtual core::Result<core::ByteSpan> Contents() = 0;

    // Where this came from, for a message a person has to act on.
    [[nodiscard]] virtual std::string Describe() const = 0;
};

}  // namespace hexlith::ports

#endif  // HEXLITH_PORTS_ROM_SOURCE_H
