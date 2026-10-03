#ifndef HEXLITH_CORE_BYTE_READER_H
#define HEXLITH_CORE_BYTE_READER_H

#include "core/result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace hexlith::core {

using ByteSpan = std::span<const std::uint8_t>;

// Every CTR container field is little-endian, and every one of them arrives
// in a file Hexlith treats as untrusted input. These readers are the only way
// the project reads a multi-byte field, for two reasons.
//
// They are bounds checked, which a parser written against a struct overlay is
// not: a truncated or hostile image would otherwise read past the buffer.
//
// They are built from byte shifts rather than from a cast over the buffer, so
// they make no alignment assumption and give the same answer on a big-endian
// host. An NCCH field at an odd offset is legal in the format and is
// undefined behaviour to read through a pointer cast.

Result<std::uint8_t> ReadU8At(ByteSpan data, std::size_t offset);

Result<std::uint16_t> ReadU16At(ByteSpan data, std::size_t offset);

Result<std::uint32_t> ReadU32At(ByteSpan data, std::size_t offset);

Result<std::uint64_t> ReadU64At(ByteSpan data, std::size_t offset);

// A subrange of data, as a view. The returned span borrows from data and does
// not outlive it.
Result<ByteSpan> ReadBytesAt(ByteSpan data, std::size_t offset, std::size_t count);

// A fixed-width NUL-padded ASCII field, such as the NCCH product code or a
// CRO module name, truncated at the first NUL.
//
// A byte outside printable ASCII is a failure rather than a mangled string.
// The field is only ever read at an offset the format fixes, so a control
// byte in it means the offset is wrong, and reporting that is more useful
// than returning a name with a stray byte in it.
Result<std::string> ReadFixedAsciiAt(ByteSpan data, std::size_t offset, std::size_t size);

// Whether data holds magic at offset. Out of range reads as no match, which
// is the answer a caller asking "is this a CRO?" wants.
bool MatchesAt(ByteSpan data, std::size_t offset, std::string_view magic);

// A cursor over a byte span, for the tables that are read in sequence rather
// than at fixed offsets: the NCSD partition table, the ExeFS file headers,
// the CRO segment table. Reads advance the cursor and leave it untouched on
// failure, so a failed parse can report where it stopped.
class ByteReader {
public:
    explicit ByteReader(ByteSpan data) : data_(data) {}

    [[nodiscard]] std::size_t Offset() const {
        return offset_;
    }

    [[nodiscard]] std::size_t Size() const {
        return data_.size();
    }

    [[nodiscard]] std::size_t Remaining() const {
        return data_.size() - offset_;
    }

    Status Seek(std::size_t offset);

    Status Skip(std::size_t count);

    Result<std::uint8_t> ReadU8();

    Result<std::uint16_t> ReadU16();

    Result<std::uint32_t> ReadU32();

    Result<std::uint64_t> ReadU64();

    Result<ByteSpan> ReadBytes(std::size_t count);

private:
    ByteSpan data_;
    std::size_t offset_ = 0;
};

}  // namespace hexlith::core

#endif  // HEXLITH_CORE_BYTE_READER_H
