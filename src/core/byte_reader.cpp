#include "core/byte_reader.h"

#include <format>

namespace hexlith::core {
namespace {

constexpr std::uint8_t FirstPrintableAscii = 0x20;
constexpr std::uint8_t LastPrintableAscii = 0x7E;

// The addition is written as a subtraction so that a count near the maximum
// of std::size_t cannot wrap and pass a check it should fail. A hostile or
// corrupt image is the expected source of such a value.
bool RangeFits(ByteSpan data, std::size_t offset, std::size_t count) {
    return offset <= data.size() && count <= data.size() - offset;
}

Error OutOfRange(ByteSpan data, std::size_t offset, std::size_t count) {
    return Error{std::format("{} byte(s) at offset {:#x} reach past the end of {:#x} bytes",
                             count,
                             offset,
                             data.size())};
}

// Assembles a little-endian field from its bytes. Width is a template
// parameter so that the shift count is a constant and the loop unrolls.
template<typename T, std::size_t Width>
Result<T> ReadLittleEndianAt(ByteSpan data, std::size_t offset) {
    static_assert(sizeof(T) >= Width, "the destination type has to hold the field");
    if (!RangeFits(data, offset, Width)) {
        return OutOfRange(data, offset, Width);
    }
    T value = 0;
    for (std::size_t index = 0; index < Width; ++index) {
        value = static_cast<T>(value |
                               static_cast<T>(static_cast<T>(data[offset + index]) << (8 * index)));
    }
    return value;
}

}  // namespace

Result<std::uint8_t> ReadU8At(ByteSpan data, std::size_t offset) {
    return ReadLittleEndianAt<std::uint8_t, 1>(data, offset);
}

Result<std::uint16_t> ReadU16At(ByteSpan data, std::size_t offset) {
    return ReadLittleEndianAt<std::uint16_t, 2>(data, offset);
}

Result<std::uint32_t> ReadU32At(ByteSpan data, std::size_t offset) {
    return ReadLittleEndianAt<std::uint32_t, 4>(data, offset);
}

Result<std::uint64_t> ReadU64At(ByteSpan data, std::size_t offset) {
    return ReadLittleEndianAt<std::uint64_t, 8>(data, offset);
}

Result<ByteSpan> ReadBytesAt(ByteSpan data, std::size_t offset, std::size_t count) {
    if (!RangeFits(data, offset, count)) {
        return OutOfRange(data, offset, count);
    }
    return data.subspan(offset, count);
}

Result<std::string> ReadFixedAsciiAt(ByteSpan data, std::size_t offset, std::size_t size) {
    const Result<ByteSpan> field = ReadBytesAt(data, offset, size);
    if (!field) {
        return field.Failure();
    }

    std::string text;
    text.reserve(size);
    for (const std::uint8_t byte : field.Value()) {
        if (byte == 0) {
            break;
        }
        if (byte < FirstPrintableAscii || byte > LastPrintableAscii) {
            return Error{std::format("byte {:#04x} at offset {:#x} is not printable ASCII, so "
                                     "the field at offset {:#x} is not a name",
                                     byte,
                                     offset + text.size(),
                                     offset)};
        }
        text.push_back(static_cast<char>(byte));
    }
    return text;
}

bool MatchesAt(ByteSpan data, std::size_t offset, std::string_view magic) {
    if (!RangeFits(data, offset, magic.size())) {
        return false;
    }
    for (std::size_t index = 0; index < magic.size(); ++index) {
        if (data[offset + index] != static_cast<std::uint8_t>(magic[index])) {
            return false;
        }
    }
    return true;
}

Status ByteReader::Seek(std::size_t offset) {
    if (offset > data_.size()) {
        return OutOfRange(data_, offset, 0);
    }
    offset_ = offset;
    return {};
}

Status ByteReader::Skip(std::size_t count) {
    if (!RangeFits(data_, offset_, count)) {
        return OutOfRange(data_, offset_, count);
    }
    offset_ += count;
    return {};
}

Result<std::uint8_t> ByteReader::ReadU8() {
    const Result<std::uint8_t> value = ReadU8At(data_, offset_);
    if (value) {
        offset_ += 1;
    }
    return value;
}

Result<std::uint16_t> ByteReader::ReadU16() {
    const Result<std::uint16_t> value = ReadU16At(data_, offset_);
    if (value) {
        offset_ += 2;
    }
    return value;
}

Result<std::uint32_t> ByteReader::ReadU32() {
    const Result<std::uint32_t> value = ReadU32At(data_, offset_);
    if (value) {
        offset_ += 4;
    }
    return value;
}

Result<std::uint64_t> ByteReader::ReadU64() {
    const Result<std::uint64_t> value = ReadU64At(data_, offset_);
    if (value) {
        offset_ += 8;
    }
    return value;
}

Result<ByteSpan> ByteReader::ReadBytes(std::size_t count) {
    const Result<ByteSpan> bytes = ReadBytesAt(data_, offset_, count);
    if (bytes) {
        offset_ += count;
    }
    return bytes;
}

}  // namespace hexlith::core
