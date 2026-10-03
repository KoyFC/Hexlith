#include "domain/ctr/backwards_lzss.h"

#include "support/crafted_lzss_stream.h"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

// The streams come from support/crafted_lzss_stream.h, which lays them out
// against the algorithm docs/formats/exefs.md describes and states what each
// one decodes to. The malformed cases here are built from them by changing
// one field, so what is being tested stays visible.
namespace hexlith::domain::ctr {
namespace {

using test::AppendLzssFooter;
using test::ControlOneReference;
using test::CraftedLzssStream;
using test::LiteralsOnlyLzssStream;
using test::LongestRun;
using test::LzssFooterSize;
using test::RepeatedByte;
using test::RepeatedByteLzssStream;
using test::VerbatimPrefixSize;

// Positions inside the eight byte footer, counted from its start.
constexpr std::size_t EncodedSizeLowByte = 0;
constexpr std::size_t EncodedSizeTopByte = 2;
constexpr std::size_t FooterSizeByte = 3;
constexpr std::size_t AdditionalSizeByte = 4;

constexpr std::uint8_t ImpossiblyLarge = 0xFF;

// A distance field of 0x0FFF, which reaches 4097 positions above the write
// cursor and so outside any output these streams produce.
constexpr std::uint8_t FarDistanceLowByte = 0xFF;
constexpr std::uint8_t FarDistanceHighByte = 0x0F;

std::size_t FooterPosition(const std::vector<std::uint8_t>& stream, std::size_t field) {
    return stream.size() - LzssFooterSize + field;
}

TEST(ReadBackwardsLzssFooter, ReadsTheDeclaredSizes) {
    const CraftedLzssStream crafted = LiteralsOnlyLzssStream();
    const core::Result<BackwardsLzssFooter> footer =
        ReadBackwardsLzssFooter(core::ByteSpan{crafted.compressed});

    ASSERT_TRUE(footer.Ok()) << footer.Failure().Message();
    EXPECT_EQ(footer.Value().footer_size, LzssFooterSize);
    EXPECT_EQ(footer.Value().encoded_size, crafted.compressed.size());
    EXPECT_EQ(footer.Value().additional_size, 0U);
    EXPECT_EQ(footer.Value().decompressed_size, crafted.compressed.size());
}

TEST(ReadBackwardsLzssFooter, RefusesSomethingShorterThanTheFooter) {
    const std::vector<std::uint8_t> stream(LzssFooterSize - 1, 0);
    const core::Result<BackwardsLzssFooter> footer =
        ReadBackwardsLzssFooter(core::ByteSpan{stream});
    ASSERT_FALSE(footer.Ok());
    EXPECT_NE(footer.Failure().Message().find("byte footer"), std::string::npos);
}

TEST(ReadBackwardsLzssFooter, RefusesAFooterLargerThanTheSection) {
    std::vector<std::uint8_t> stream = LiteralsOnlyLzssStream().compressed;
    stream[FooterPosition(stream, FooterSizeByte)] = ImpossiblyLarge;

    const core::Result<BackwardsLzssFooter> footer =
        ReadBackwardsLzssFooter(core::ByteSpan{stream});
    ASSERT_FALSE(footer.Ok());
    EXPECT_NE(footer.Failure().Message().find("declares itself"), std::string::npos)
        << footer.Failure().Message();
}

TEST(ReadBackwardsLzssFooter, RefusesAnEncodedRegionLargerThanTheSection) {
    std::vector<std::uint8_t> stream = LiteralsOnlyLzssStream().compressed;
    stream[FooterPosition(stream, EncodedSizeTopByte)] = ImpossiblyLarge;

    const core::Result<BackwardsLzssFooter> footer =
        ReadBackwardsLzssFooter(core::ByteSpan{stream});
    ASSERT_FALSE(footer.Ok());
    EXPECT_NE(footer.Failure().Message().find("encoded bytes"), std::string::npos)
        << footer.Failure().Message();
}

TEST(ReadBackwardsLzssFooter, RefusesAFooterLargerThanTheEncodedRegion) {
    std::vector<std::uint8_t> stream = LiteralsOnlyLzssStream().compressed;
    stream[FooterPosition(stream, EncodedSizeLowByte)] = 1;

    const core::Result<BackwardsLzssFooter> footer =
        ReadBackwardsLzssFooter(core::ByteSpan{stream});
    ASSERT_FALSE(footer.Ok());
    EXPECT_NE(footer.Failure().Message().find("below where it has to stop"), std::string::npos)
        << footer.Failure().Message();
}

TEST(DecompressBackwardsLzss, DecodesLiterals) {
    const CraftedLzssStream crafted = LiteralsOnlyLzssStream();
    const core::Result<std::vector<std::uint8_t>> output =
        DecompressBackwardsLzss(core::ByteSpan{crafted.compressed});

    ASSERT_TRUE(output.Ok()) << output.Failure().Message();
    EXPECT_EQ(output.Value(), crafted.expected);
}

TEST(DecompressBackwardsLzss, DecodesBackReferences) {
    const CraftedLzssStream crafted = RepeatedByteLzssStream();
    const core::Result<std::vector<std::uint8_t>> output =
        DecompressBackwardsLzss(core::ByteSpan{crafted.compressed});

    ASSERT_TRUE(output.Ok()) << output.Failure().Message();
    EXPECT_EQ(output.Value(), crafted.expected);
    EXPECT_GT(output.Value().size(), crafted.compressed.size());
}

TEST(DecompressBackwardsLzss, LeavesTheVerbatimBottomAlone) {
    const CraftedLzssStream crafted = RepeatedByteLzssStream();
    const core::Result<std::vector<std::uint8_t>> output =
        DecompressBackwardsLzss(core::ByteSpan{crafted.compressed});

    ASSERT_TRUE(output.Ok()) << output.Failure().Message();
    for (std::size_t index = 0; index < VerbatimPrefixSize; ++index) {
        EXPECT_EQ(output.Value()[index], crafted.compressed[index]) << index;
    }
    EXPECT_EQ(output.Value()[VerbatimPrefixSize], RepeatedByte);
}

TEST(DecompressBackwardsLzss, RefusesABackReferenceReadingPastTheTop) {
    // A run whose distance reaches far above anything that has been written.
    // The encoded region stops at the end of the verbatim prefix, so the
    // reference is reached rather than stopped short of.
    std::vector<std::uint8_t> stream(VerbatimPrefixSize, RepeatedByte);
    stream.push_back(FarDistanceLowByte);
    stream.push_back(FarDistanceHighByte);
    stream.push_back(ControlOneReference);
    AppendLzssFooter(stream, stream.size() + LzssFooterSize - VerbatimPrefixSize, 0);

    const core::Result<std::vector<std::uint8_t>> output =
        DecompressBackwardsLzss(core::ByteSpan{stream});
    ASSERT_FALSE(output.Ok());
    EXPECT_NE(output.Failure().Message().find("outside an output"), std::string::npos)
        << output.Failure().Message();
}

TEST(DecompressBackwardsLzss, RefusesAReferenceThatRunsOutOfEncodedBytes) {
    // A control byte asking for a reference with only one encoded byte left
    // below it, which no well-formed stream can produce.
    std::vector<std::uint8_t> stream(VerbatimPrefixSize, RepeatedByte);
    stream.push_back(0);
    stream.push_back(ControlOneReference);
    AppendLzssFooter(stream, stream.size() + LzssFooterSize - VerbatimPrefixSize, 0);

    const core::Result<std::vector<std::uint8_t>> output =
        DecompressBackwardsLzss(core::ByteSpan{stream});
    ASSERT_FALSE(output.Ok());
    EXPECT_NE(output.Failure().Message().find("needs two bytes"), std::string::npos)
        << output.Failure().Message();
}

TEST(DecompressBackwardsLzss, RefusesAStreamThatWritesBelowTheStart) {
    // The stream that decodes correctly, with its additional size cleared.
    // The output is then no larger than the input while the encoding still
    // asks for 152 bytes, so the write cursor runs off the bottom.
    std::vector<std::uint8_t> stream = RepeatedByteLzssStream().compressed;
    for (std::size_t index = 0; index < 4; ++index) {
        stream[FooterPosition(stream, AdditionalSizeByte + index)] = 0;
    }

    const core::Result<std::vector<std::uint8_t>> output =
        DecompressBackwardsLzss(core::ByteSpan{stream});
    ASSERT_FALSE(output.Ok());
    EXPECT_NE(output.Failure().Message().find("below the start"), std::string::npos)
        << output.Failure().Message();
}

TEST(DecompressBackwardsLzss, StopsWhenTheEncodedRegionEndsRatherThanWhenTheOutputIsFull) {
    const CraftedLzssStream crafted = RepeatedByteLzssStream();
    const core::Result<std::vector<std::uint8_t>> output =
        DecompressBackwardsLzss(core::ByteSpan{crafted.compressed});

    ASSERT_TRUE(output.Ok()) << output.Failure().Message();

    // Decoding wrote everything above the verbatim prefix and stopped, with
    // the run length that produced the last of it being the longest the
    // encoding expresses.
    EXPECT_EQ(output.Value().size() - VerbatimPrefixSize, 8 + (8 * LongestRun));
}

}  // namespace
}  // namespace hexlith::domain::ctr
