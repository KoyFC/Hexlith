#include "core/sha256.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::core {
namespace {

// The vectors are from FIPS 180-4 and its published test values. They test
// Hexlith's wrapper rather than the algorithm: that the right digest was
// asked for, that nothing is truncated, and that an incremental hash agrees
// with a single call. A wrapper that selected SHA-1 or dropped the last block
// would still produce plausible looking output, so the check is against known
// values and not against itself.
constexpr std::string_view EmptyDigest =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr std::string_view AbcDigest =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr std::string_view TwoBlockDigest =
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
constexpr std::string_view MillionLettersDigest =
    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

constexpr std::size_t MillionLetters = 1000000;

// Chunk sizes that are never a multiple of SHA-256's 64 byte block, so the
// library is left holding a partial block between calls.
constexpr std::size_t FirstChunk = 1;
constexpr std::size_t ChunkGrowth = 3;
constexpr std::size_t ChunkStep = 7;

ByteSpan AsBytes(std::string_view text) {
    return ByteSpan{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

TEST(Sha256, HashesTheEmptyInput) {
    EXPECT_EQ(Sha256::Of(ByteSpan{}).ToHex(), EmptyDigest);
}

TEST(Sha256, HashesAShortMessage) {
    EXPECT_EQ(Sha256::Of(AsBytes("abc")).ToHex(), AbcDigest);
}

TEST(Sha256, HashesAMessageThatSpansTwoBlocks) {
    EXPECT_EQ(
        Sha256::Of(AsBytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")).ToHex(),
        TwoBlockDigest);
}

// A million bytes is past the point where a 32-bit length counter would be
// fine but a 64-bit one is needed, which is the mistake this vector exists to
// catch in any implementation.
TEST(Sha256, HashesAMillionBytes) {
    const std::vector<std::uint8_t> letters(MillionLetters, static_cast<std::uint8_t>('a'));
    EXPECT_EQ(Sha256::Of(ByteSpan{letters}).ToHex(), MillionLettersDigest);
}

TEST(Sha256, IncrementalHashingAgreesWithOneCall) {
    const std::vector<std::uint8_t> letters(MillionLetters, static_cast<std::uint8_t>('a'));

    Sha256 hasher;
    std::size_t offset = 0;
    std::size_t chunk = FirstChunk;
    while (offset < letters.size()) {
        const std::size_t count = std::min(chunk, letters.size() - offset);
        hasher.Update(ByteSpan{letters}.subspan(offset, count));
        offset += count;
        chunk = (chunk * ChunkGrowth) + ChunkStep;
    }

    EXPECT_EQ(hasher.Finish().ToHex(), MillionLettersDigest);
}

TEST(Sha256, UpdatingWithNothingChangesNothing) {
    Sha256 hasher;
    hasher.Update(ByteSpan{});
    hasher.Update(AsBytes("abc"));
    hasher.Update(ByteSpan{});
    EXPECT_EQ(hasher.Finish().ToHex(), AbcDigest);
}

TEST(Sha256, FinishResetsTheState) {
    Sha256 hasher;
    hasher.Update(AsBytes("abc"));
    EXPECT_EQ(hasher.Finish().ToHex(), AbcDigest);
    EXPECT_EQ(hasher.Finish().ToHex(), EmptyDigest) << "Finish has to leave a fresh state";
    hasher.Update(AsBytes("abc"));
    EXPECT_EQ(hasher.Finish().ToHex(), AbcDigest);
}

TEST(Sha256, TheSameInputHashesTheSameWayTwice) {
    EXPECT_EQ(Sha256::Of(AsBytes("kujira-1")), Sha256::Of(AsBytes("kujira-1")));
    EXPECT_NE(Sha256::Of(AsBytes("kujira-1")), Sha256::Of(AsBytes("kujira-2")));
}

TEST(Digest, ToHexIsLowercaseAndSixtyFourDigits) {
    const std::string text = Sha256::Of(AsBytes("abc")).ToHex();
    EXPECT_EQ(text.size(), Digest::SizeInBytes * 2);
    EXPECT_EQ(text, AbcDigest);
    EXPECT_EQ(text.find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST(ParseDigestHex, RoundTripsThroughToHex) {
    const Digest measured = Sha256::Of(AsBytes("abc"));
    const Result<Digest> parsed = ParseDigestHex(AbcDigest);
    ASSERT_TRUE(parsed.Ok()) << parsed.Failure().Message();
    EXPECT_EQ(parsed.Value(), measured);
}

TEST(ParseDigestHex, AcceptsUppercase) {
    const Result<Digest> parsed =
        ParseDigestHex("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    ASSERT_TRUE(parsed.Ok()) << parsed.Failure().Message();
    EXPECT_EQ(parsed.Value().ToHex(), AbcDigest);
}

TEST(ParseDigestHex, RejectsTheWrongLength) {
    const Result<Digest> parsed = ParseDigestHex("ba7816bf");
    ASSERT_FALSE(parsed.Ok());
    EXPECT_EQ(parsed.Failure().Message(), "a SHA-256 digest is 64 hexadecimal digits, not 8");
}

TEST(ParseDigestHex, RejectsANonHexadecimalDigit) {
    std::string text{AbcDigest};
    text.back() = 'z';
    const Result<Digest> parsed = ParseDigestHex(text);
    ASSERT_FALSE(parsed.Ok());
    EXPECT_EQ(parsed.Failure().Message(), "'z' is not a hexadecimal digit");
}

}  // namespace
}  // namespace hexlith::core
