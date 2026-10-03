#include "core/byte_reader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <vector>

namespace hexlith::core {
namespace {

// Deliberately 7 bytes long, so that a read of the last field runs off the
// end and the odd length catches an off-by-one that an aligned size hides.
constexpr std::array<std::uint8_t, 7> Sample = {0x78, 0x56, 0x34, 0x12, 0xFF, 0xEE, 0xDD};

ByteSpan Span() {
    return ByteSpan{Sample};
}

TEST(ReadAt, AssemblesLittleEndianFields) {
    EXPECT_EQ(ReadU8At(Span(), 0).Value(), 0x78U);
    EXPECT_EQ(ReadU16At(Span(), 0).Value(), 0x5678U);
    EXPECT_EQ(ReadU32At(Span(), 0).Value(), 0x12345678U);
}

TEST(ReadAt, ReadsAtAnUnalignedOffset) {
    // 0x56, 0x34, 0x12, 0xFF read from offset 1, which no pointer cast could
    // do without undefined behaviour.
    EXPECT_EQ(ReadU32At(Span(), 1).Value(), 0xFF123456U);
}

TEST(ReadAt, RejectsAFieldThatRunsOffTheEnd) {
    const Result<std::uint32_t> result = ReadU32At(Span(), 4);
    ASSERT_FALSE(result.Ok());
    EXPECT_EQ(result.Failure().Message(),
              "4 byte(s) at offset 0x4 reach past the end of 0x7 bytes");
}

TEST(ReadAt, RejectsAnOffsetBeyondTheBuffer) {
    EXPECT_FALSE(ReadU8At(Span(), Sample.size()).Ok());
    EXPECT_FALSE(ReadU8At(Span(), 1000).Ok());
}

TEST(ReadAt, RejectsAnEmptyBuffer) {
    EXPECT_FALSE(ReadU8At(ByteSpan{}, 0).Ok());
}

TEST(ReadAt, ReadsAnEightByteField) {
    const std::vector<std::uint8_t> wide = {0x00, 0x5D, 0x05, 0x00, 0x00, 0x00, 0x04, 0x00};
    // The program ID of Pokemon X, as the NCCH header stores it.
    EXPECT_EQ(ReadU64At(ByteSpan{wide}, 0).Value(), 0x0004000000055D00U);
}

TEST(ReadBytesAt, ReturnsASubrange) {
    const Result<ByteSpan> bytes = ReadBytesAt(Span(), 2, 3);
    ASSERT_TRUE(bytes.Ok());
    ASSERT_EQ(bytes.Value().size(), 3U);
    EXPECT_EQ(bytes.Value()[0], 0x34U);
    EXPECT_EQ(bytes.Value()[2], 0xFFU);
}

TEST(ReadBytesAt, AcceptsAnEmptyRangeAtTheEnd) {
    EXPECT_TRUE(ReadBytesAt(Span(), Sample.size(), 0).Ok());
}

TEST(ReadBytesAt, DoesNotWrapOnAHugeCount) {
    // offset + count overflows std::size_t. A bounds check written as an
    // addition would wrap and accept this.
    const Result<ByteSpan> bytes = ReadBytesAt(Span(), 1, std::numeric_limits<std::size_t>::max());
    EXPECT_FALSE(bytes.Ok());
}

TEST(ReadFixedAsciiAt, TruncatesAtTheFirstNul) {
    const std::vector<std::uint8_t> field = {
        'C', 'T', 'R', '-', 'P', '-', 'E', 'K', 'J', 'A', 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(ReadFixedAsciiAt(ByteSpan{field}, 0, field.size()).Value(), "CTR-P-EKJA");
}

TEST(ReadFixedAsciiAt, AcceptsAFieldWithNoNul) {
    const std::vector<std::uint8_t> field = {'k', 'u', 'j', 'i', 'r', 'a', '-', '1'};
    EXPECT_EQ(ReadFixedAsciiAt(ByteSpan{field}, 0, field.size()).Value(), "kujira-1");
}

TEST(ReadFixedAsciiAt, RejectsAControlByte) {
    const std::vector<std::uint8_t> field = {'D', 'l', 'l', 0x01, 0};
    const Result<std::string> name = ReadFixedAsciiAt(ByteSpan{field}, 0, field.size());
    ASSERT_FALSE(name.Ok());
    EXPECT_EQ(name.Failure().Message(),
              "byte 0x01 at offset 0x3 is not printable ASCII, so the field at offset 0x0 is "
              "not a name");
}

TEST(ReadFixedAsciiAt, RejectsAFieldThatRunsOffTheEnd) {
    EXPECT_FALSE(ReadFixedAsciiAt(Span(), 4, 8).Ok());
}

TEST(MatchesAt, FindsMagicWhereTheFormatPutsIt) {
    const std::vector<std::uint8_t> magic = {0, 0, 'N', 'C', 'S', 'D'};
    EXPECT_TRUE(MatchesAt(ByteSpan{magic}, 2, "NCSD"));
    EXPECT_FALSE(MatchesAt(ByteSpan{magic}, 2, "NCCH"));
    EXPECT_FALSE(MatchesAt(ByteSpan{magic}, 0, "NCSD"));
}

TEST(MatchesAt, ReportsNoMatchOutOfRange) {
    EXPECT_FALSE(MatchesAt(Span(), 6, "NCSD"));
    EXPECT_FALSE(MatchesAt(ByteSpan{}, 0, "NCSD"));
}

TEST(ByteReader, AdvancesThroughFields) {
    ByteReader reader{Span()};
    EXPECT_EQ(reader.Size(), 7U);
    EXPECT_EQ(reader.Remaining(), 7U);

    EXPECT_EQ(reader.ReadU32().Value(), 0x12345678U);
    EXPECT_EQ(reader.Offset(), 4U);
    EXPECT_EQ(reader.ReadU16().Value(), 0xEEFFU);
    EXPECT_EQ(reader.ReadU8().Value(), 0xDDU);
    EXPECT_EQ(reader.Remaining(), 0U);
}

TEST(ByteReader, LeavesTheCursorAloneOnAFailedRead) {
    ByteReader reader{Span()};
    ASSERT_TRUE(reader.Seek(5).Ok());
    EXPECT_FALSE(reader.ReadU32().Ok());
    EXPECT_EQ(reader.Offset(), 5U) << "a failed read must not consume anything";
    EXPECT_EQ(reader.ReadU16().Value(), 0xDDEEU);
}

TEST(ByteReader, SeeksAndSkips) {
    ByteReader reader{Span()};
    ASSERT_TRUE(reader.Skip(2).Ok());
    EXPECT_EQ(reader.ReadU8().Value(), 0x34U);
    ASSERT_TRUE(reader.Seek(0).Ok());
    EXPECT_EQ(reader.ReadU8().Value(), 0x78U);
}

TEST(ByteReader, SeekToTheEndIsAllowedAndReadingThereIsNot) {
    ByteReader reader{Span()};
    ASSERT_TRUE(reader.Seek(Sample.size()).Ok());
    EXPECT_EQ(reader.Remaining(), 0U);
    EXPECT_FALSE(reader.ReadU8().Ok());
}

TEST(ByteReader, RejectsASeekPastTheEnd) {
    ByteReader reader{Span()};
    EXPECT_FALSE(reader.Seek(Sample.size() + 1).Ok());
    EXPECT_EQ(reader.Offset(), 0U);
}

TEST(ByteReader, ReadsASubrange) {
    ByteReader reader{Span()};
    ASSERT_TRUE(reader.Skip(4).Ok());
    const Result<ByteSpan> bytes = reader.ReadBytes(3);
    ASSERT_TRUE(bytes.Ok());
    EXPECT_EQ(bytes.Value()[0], 0xFFU);
    EXPECT_EQ(reader.Offset(), 7U);
}

}  // namespace
}  // namespace hexlith::core
