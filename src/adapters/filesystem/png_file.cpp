#include "adapters/filesystem/png_file.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <string_view>

namespace hexlith::adapters::filesystem {
namespace {

constexpr std::array<std::uint8_t, 8> Signature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

// CRC-32 with the polynomial the PNG specification gives, reflected.
std::uint32_t Crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0xFFFFFFFF) {
    constexpr std::uint32_t Polynomial = 0xEDB88320;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0 ? (crc >> 1U) ^ Polynomial : crc >> 1U;
        }
    }
    return crc;
}

// RFC 1950's Adler-32, modulo the largest prime below 65536.
std::uint32_t Adler32(std::span<const std::uint8_t> bytes) {
    constexpr std::uint32_t Modulus = 65521;
    std::uint32_t low = 1;
    std::uint32_t high = 0;
    for (const std::uint8_t byte : bytes) {
        low = (low + byte) % Modulus;
        high = (high + low) % Modulus;
    }
    return (high << 16U) | low;
}

void PutBigEndian(std::vector<std::uint8_t>& out, std::uint32_t value) {
    constexpr unsigned ByteBits = 8;
    constexpr unsigned WordBytes = 4;
    for (unsigned byte = WordBytes; byte > 0; --byte) {
        out.push_back(static_cast<std::uint8_t>(value >> (ByteBits * (byte - 1))));
    }
}

void PutChunk(std::vector<std::uint8_t>& out,
              std::string_view type,
              std::span<const std::uint8_t> data) {
    PutBigEndian(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), data.begin(), data.end());
    const std::uint32_t crc = Crc32(std::span<const std::uint8_t>(out).subspan(start)) ^ 0xFFFFFFFF;
    PutBigEndian(out, crc);
}

// The rows, each after a filter byte of zero, in a zlib stream of stored
// blocks of at most 65535 bytes.
std::vector<std::uint8_t> ImageData(const domain::gpu::Picture& picture) {
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(picture.height) * ((picture.width * 3) + 1));
    for (std::uint32_t y = 0; y < picture.height; ++y) {
        raw.push_back(0);
        for (std::uint32_t x = 0; x < picture.width; ++x) {
            const domain::gpu::Color& color =
                picture.pixels.at((static_cast<std::size_t>(y) * picture.width) + x);
            raw.insert(raw.end(), {color.red, color.green, color.blue});
        }
    }
    // Deflate with a 32 KiB window and no dictionary; the check bits make
    // the header a multiple of 31.
    constexpr std::uint8_t Method = 0x78;
    constexpr std::uint8_t Flags = 0x01;
    constexpr std::size_t MaximumBlock = 0xFFFF;
    std::vector<std::uint8_t> stream = {Method, Flags};
    // An empty picture still needs its one final block.
    std::size_t at = 0;
    for (bool first = true; first || at < raw.size(); first = false) {
        const std::size_t size = std::min(MaximumBlock, raw.size() - at);
        const bool last = at + size == raw.size();
        stream.push_back(last ? 1 : 0);
        const auto length = static_cast<std::uint16_t>(size);
        const auto complement = static_cast<std::uint16_t>(~length);
        stream.insert(stream.end(),
                      {static_cast<std::uint8_t>(length),
                       static_cast<std::uint8_t>(length >> 8U),
                       static_cast<std::uint8_t>(complement),
                       static_cast<std::uint8_t>(complement >> 8U)});
        stream.insert(stream.end(),
                      raw.begin() + static_cast<std::ptrdiff_t>(at),
                      raw.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
    }
    PutBigEndian(stream, Adler32(raw));
    return stream;
}

}  // namespace

std::vector<std::uint8_t> EncodePng(const domain::gpu::Picture& picture) {
    constexpr std::uint8_t BitDepth = 8;
    constexpr std::uint8_t TrueColour = 2;
    std::vector<std::uint8_t> out(Signature.begin(), Signature.end());
    std::vector<std::uint8_t> header;
    PutBigEndian(header, picture.width);
    PutBigEndian(header, picture.height);
    // Bit depth, colour type, compression, filter, interlace.
    header.insert(header.end(), {BitDepth, TrueColour, 0, 0, 0});
    PutChunk(out, "IHDR", header);
    PutChunk(out, "IDAT", ImageData(picture));
    PutChunk(out, "IEND", {});
    return out;
}

core::Status WritePng(const std::filesystem::path& path, const domain::gpu::Picture& picture) {
    const std::vector<std::uint8_t> bytes = EncodePng(picture);
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        return core::Error{std::format("could not write {}", path.string())};
    }
    return {};
}

}  // namespace hexlith::adapters::filesystem
