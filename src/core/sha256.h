#ifndef HEXLITH_CORE_SHA256_H
#define HEXLITH_CORE_SHA256_H

#include "core/byte_reader.h"
#include "core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace hexlith::core {

// A SHA-256 digest. Hexlith uses one to identify content, not to secure it:
// the manifest records what was extracted, and the GameProfile records which
// image a profile was written against. ADR 0009 covers where the
// implementation comes from.
struct Digest {
    static constexpr std::size_t SizeInBytes = 32;

    std::array<std::uint8_t, SizeInBytes> bytes{};

    // Lowercase, unseparated, which is what sha256sum prints and therefore
    // what a manifest can be checked against by hand.
    [[nodiscard]] std::string ToHex() const;

    friend bool operator==(const Digest& left, const Digest& right) = default;
};

// The inverse of ToHex, for comparing a measured digest against one written
// down as text. Rejects anything that is not exactly 64 hexadecimal digits,
// in either case.
Result<Digest> ParseDigestHex(std::string_view text);

// Incremental SHA-256, for content too large to hold in memory. The 2 GiB
// cartridge image is hashed in chunks by the filesystem adapter rather than
// read into a buffer first.
//
// Finish returns the digest and resets the state, so one instance can hash
// several inputs in sequence.
//
// An OpenSSL failure throws std::runtime_error. The only way these calls
// fail is a failed allocation inside the library, and there is nothing a
// caller could do about it that returning a Result would express.
class Sha256 {
public:
    Sha256();
    ~Sha256();

    Sha256(Sha256&& other) noexcept;
    Sha256& operator=(Sha256&& other) noexcept;

    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void Update(ByteSpan data);

    [[nodiscard]] Digest Finish();

    // The whole input at once, for the common case.
    [[nodiscard]] static Digest Of(ByteSpan data);

private:
    // Holds the OpenSSL context. Defined in the translation unit so that no
    // OpenSSL header reaches a Hexlith header, which keeps the dependency
    // confined to this one target.
    struct Context;

    std::unique_ptr<Context> context_;
};

}  // namespace hexlith::core

#endif  // HEXLITH_CORE_SHA256_H
