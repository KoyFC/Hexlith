#include "core/sha256.h"

#include <format>
#include <openssl/evp.h>
#include <stdexcept>
#include <utility>

namespace hexlith::core {
namespace {

constexpr std::size_t HexDigitsPerByte = 2;

// The value the letters stand for in hexadecimal, and the mask that takes the
// second digit of a byte.
constexpr std::uint8_t FirstLetterValue = 10;
constexpr std::uint8_t LowNibbleMask = 0x0F;
constexpr std::size_t HexDigestLength = Digest::SizeInBytes * HexDigitsPerByte;

// A deleter rather than a lambda so that the unique_ptr type can be named in
// the Context definition below.
struct DigestContextDeleter {
    void operator()(EVP_MD_CTX* context) const {
        EVP_MD_CTX_free(context);
    }
};

using DigestContext = std::unique_ptr<EVP_MD_CTX, DigestContextDeleter>;

DigestContext NewStartedContext() {
    DigestContext context{EVP_MD_CTX_new()};
    if (!context) {
        throw std::runtime_error("OpenSSL could not allocate a digest context");
    }
    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("OpenSSL could not start a SHA-256 digest");
    }
    return context;
}

Result<std::uint8_t> ParseHexDigit(char character) {
    if (character >= '0' && character <= '9') {
        return static_cast<std::uint8_t>(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return static_cast<std::uint8_t>(character - 'a' + FirstLetterValue);
    }
    if (character >= 'A' && character <= 'F') {
        return static_cast<std::uint8_t>(character - 'A' + FirstLetterValue);
    }
    return Error{std::format("'{}' is not a hexadecimal digit", character)};
}

}  // namespace

struct Sha256::Context {
    DigestContext digest = NewStartedContext();
};

std::string Digest::ToHex() const {
    static constexpr std::string_view HexDigits = "0123456789abcdef";

    std::string text;
    text.reserve(HexDigestLength);
    for (const std::uint8_t byte : bytes) {
        text.push_back(HexDigits[byte >> 4U]);
        text.push_back(HexDigits[byte & LowNibbleMask]);
    }
    return text;
}

Result<Digest> ParseDigestHex(std::string_view text) {
    if (text.size() != HexDigestLength) {
        return Error{std::format(
            "a SHA-256 digest is {} hexadecimal digits, not {}", HexDigestLength, text.size())};
    }

    Digest digest;
    for (std::size_t index = 0; index < Digest::SizeInBytes; ++index) {
        const Result<std::uint8_t> high = ParseHexDigit(text[index * HexDigitsPerByte]);
        if (!high) {
            return high.Failure();
        }
        const Result<std::uint8_t> low = ParseHexDigit(text[(index * HexDigitsPerByte) + 1]);
        if (!low) {
            return low.Failure();
        }
        digest.bytes[index] = static_cast<std::uint8_t>((high.Value() << 4U) | low.Value());
    }
    return digest;
}

Result<Digest> ReadDigestAt(ByteSpan data, std::size_t offset) {
    const Result<ByteSpan> stored = ReadBytesAt(data, offset, Digest::SizeInBytes);
    if (!stored) {
        return stored.Failure();
    }

    Digest digest;
    for (std::size_t index = 0; index < Digest::SizeInBytes; ++index) {
        digest.bytes[index] = stored.Value()[index];
    }
    return digest;
}

Sha256::Sha256() : context_(std::make_unique<Context>()) {}

Sha256::~Sha256() = default;

Sha256::Sha256(Sha256&& other) noexcept = default;

Sha256& Sha256::operator=(Sha256&& other) noexcept = default;

void Sha256::Update(ByteSpan data) {
    if (data.empty()) {
        return;
    }
    if (EVP_DigestUpdate(context_->digest.get(), data.data(), data.size()) != 1) {
        throw std::runtime_error("OpenSSL could not hash a block");
    }
}

Digest Sha256::Finish() {
    Digest digest;
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(context_->digest.get(), digest.bytes.data(), &length) != 1) {
        throw std::runtime_error("OpenSSL could not finish a SHA-256 digest");
    }
    if (length != Digest::SizeInBytes) {
        throw std::runtime_error(
            std::format("OpenSSL produced a {} byte digest, not {}", length, Digest::SizeInBytes));
    }
    context_->digest = NewStartedContext();
    return digest;
}

Digest Sha256::Of(ByteSpan data) {
    Sha256 hasher;
    hasher.Update(data);
    return hasher.Finish();
}

}  // namespace hexlith::core
