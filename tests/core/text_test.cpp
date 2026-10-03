#include "core/text.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::core {
namespace {

constexpr std::uint16_t ByteMask = 0xFF;
constexpr unsigned ByteBits = 8;

std::vector<std::uint8_t> Units(std::initializer_list<std::uint16_t> values) {
    std::vector<std::uint8_t> bytes;
    for (const std::uint16_t value : values) {
        bytes.push_back(static_cast<std::uint8_t>(value & ByteMask));
        bytes.push_back(static_cast<std::uint8_t>(value >> ByteBits));
    }
    return bytes;
}

TEST(Utf16LeToUtf8, DecodesAscii) {
    const std::vector<std::uint8_t> name = Units({'D', 'l', 'l', 'F', 'i', 'e', 'l', 'd'});
    EXPECT_EQ(Utf16LeToUtf8(ByteSpan{name}).Value(), "DllField");
}

TEST(Utf16LeToUtf8, DecodesAnEmptyString) {
    EXPECT_EQ(Utf16LeToUtf8(ByteSpan{}).Value(), "");
}

TEST(Utf16LeToUtf8, DecodesTwoAndThreeByteCodePoints) {
    // U+00F1 and U+65E5, which take two and three bytes in UTF-8.
    const std::vector<std::uint8_t> name = Units({0x00F1, 0x65E5});
    EXPECT_EQ(Utf16LeToUtf8(ByteSpan{name}).Value(), "\xC3\xB1\xE6\x97\xA5");
}

TEST(Utf16LeToUtf8, DecodesASurrogatePair) {
    // U+1F600, which UTF-16 carries as D83D DE00.
    const std::vector<std::uint8_t> name = Units({0xD83D, 0xDE00});
    EXPECT_EQ(Utf16LeToUtf8(ByteSpan{name}).Value(), "\xF0\x9F\x98\x80");
}

TEST(Utf16LeToUtf8, RefusesAnOddNumberOfBytes) {
    const std::vector<std::uint8_t> name = {'a', 0, 'b'};
    const Result<std::string> text = Utf16LeToUtf8(ByteSpan{name});
    ASSERT_FALSE(text.Ok());
    EXPECT_NE(text.Failure().Message().find("even number of bytes"), std::string::npos);
}

TEST(Utf16LeToUtf8, RefusesAnUnpairedLeadingSurrogate) {
    const std::vector<std::uint8_t> name = Units({0xD83D});
    const Result<std::string> text = Utf16LeToUtf8(ByteSpan{name});
    ASSERT_FALSE(text.Ok());
    EXPECT_NE(text.Failure().Message().find("has no pair"), std::string::npos);
}

TEST(Utf16LeToUtf8, RefusesALeadingSurrogateFollowedByAnythingElse) {
    const std::vector<std::uint8_t> name = Units({0xD83D, 'a'});
    const Result<std::string> text = Utf16LeToUtf8(ByteSpan{name});
    ASSERT_FALSE(text.Ok());
    EXPECT_NE(text.Failure().Message().find("rather than a trailing surrogate"), std::string::npos);
}

TEST(Utf16LeToUtf8, RefusesATrailingSurrogateOnItsOwn) {
    const std::vector<std::uint8_t> name = Units({0xDE00});
    const Result<std::string> text = Utf16LeToUtf8(ByteSpan{name});
    ASSERT_FALSE(text.Ok());
    EXPECT_NE(text.Failure().Message().find("nothing before it"), std::string::npos);
}

TEST(Utf8ToUtf16Le, EncodesAscii) {
    EXPECT_EQ(Utf8ToUtf16Le("DllField").Value(), Units({'D', 'l', 'l', 'F', 'i', 'e', 'l', 'd'}));
}

TEST(Utf8ToUtf16Le, EncodesTwoAndThreeByteCodePoints) {
    EXPECT_EQ(Utf8ToUtf16Le("\xC3\xB1\xE6\x97\xA5").Value(), Units({0x00F1, 0x65E5}));
}

TEST(Utf8ToUtf16Le, EncodesASurrogatePair) {
    EXPECT_EQ(Utf8ToUtf16Le("\xF0\x9F\x98\x80").Value(), Units({0xD83D, 0xDE00}));
}

TEST(Utf8ToUtf16Le, RefusesAContinuationByteOnItsOwn) {
    const Result<std::vector<std::uint8_t>> units = Utf8ToUtf16Le("\x80");
    ASSERT_FALSE(units.Ok());
    EXPECT_NE(units.Failure().Message().find("does not start a UTF-8 sequence"), std::string::npos);
}

TEST(Utf8ToUtf16Le, RefusesATruncatedSequence) {
    const Result<std::vector<std::uint8_t>> units = Utf8ToUtf16Le("\xE6\x97");
    ASSERT_FALSE(units.Ok());
    EXPECT_NE(units.Failure().Message().find("bytes and only"), std::string::npos);
}

TEST(Utf8ToUtf16Le, RefusesAMissingContinuationByte) {
    const Result<std::vector<std::uint8_t>> units = Utf8ToUtf16Le("\xE6\x97\x41");
    ASSERT_FALSE(units.Ok());
    EXPECT_NE(units.Failure().Message().find("not a continuation byte"), std::string::npos);
}

TEST(Text, RoundTripsEveryShapeOfCodePoint) {
    const std::string_view original = "a\xC3\xB1\xE6\x97\xA5\xF0\x9F\x98\x80z";
    const Result<std::vector<std::uint8_t>> units = Utf8ToUtf16Le(original);
    ASSERT_TRUE(units.Ok()) << units.Failure().Message();
    EXPECT_EQ(Utf16LeToUtf8(ByteSpan{units.Value()}).Value(), original);
}

}  // namespace
}  // namespace hexlith::core
