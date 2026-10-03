#include "core/byte_reader.h"
#include "core/sha256.h"

#include "support/synthetic_ctr_image.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>

// The builder and the parsers share domain/ctr/layout.h, so a wrong constant
// there would be invisible to a test that used the same constant. Every
// offset below is therefore written as the literal from docs/formats/, which
// is what makes this test able to catch one.
namespace hexlith::test {
namespace {

using core::ReadFixedAsciiAt;
using core::ReadU16At;
using core::ReadU32At;
using core::ReadU64At;
using core::ReadU8At;

constexpr std::uint64_t PokemonXProgramId = 0x0004000000055D00;
constexpr std::size_t MediaUnit = 0x200;
constexpr std::size_t PartitionStart = 0x4000;
constexpr std::size_t PartitionTableEntrySize = 8;
constexpr std::size_t FirstAbsentPartition = 2;
constexpr std::size_t LastAbsentPartition = 6;
constexpr std::size_t ExeFsFileHeaderCount = 10;
constexpr std::size_t ExeFsFileHeaderSize = 0x10;

TEST(SyntheticImage, WritesTheNcsdHeaderWhereTheFormatSaysItGoes) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());

    EXPECT_TRUE(core::MatchesAt(image.Span(), 0x100, "NCSD"));
    EXPECT_EQ(ReadU64At(image.Span(), 0x108).Value(), PokemonXProgramId);
    EXPECT_EQ(ReadU32At(image.Span(), 0x104).Value(), image.bytes.size() / MediaUnit);
}

TEST(SyntheticImage, DeclaresTheExecutablePartitionInMediaUnits) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());

    EXPECT_EQ(ReadU32At(image.Span(), 0x120).Value(), PartitionStart / MediaUnit);
    EXPECT_EQ(ReadU32At(image.Span(), 0x124).Value(), image.partition_size / MediaUnit);
    EXPECT_EQ(image.partition_offset, PartitionStart);
    EXPECT_EQ(image.partition_size % MediaUnit, 0U);
}

TEST(SyntheticImage, DeclaresTheExtraPartitionsAndLeavesTheAbsentOnesZero) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());

    // Partitions 1 and 7 are present by default, 2 through 6 are not, which
    // is the shape of a retail cartridge with no Download Play child.
    EXPECT_NE(ReadU32At(image.Span(), 0x120 + PartitionTableEntrySize).Value(), 0U);
    EXPECT_NE(ReadU32At(image.Span(), 0x120 + (7 * PartitionTableEntrySize)).Value(), 0U);
    for (std::size_t index = FirstAbsentPartition; index <= LastAbsentPartition; ++index) {
        const std::size_t entry = 0x120 + (index * PartitionTableEntrySize);
        EXPECT_EQ(ReadU32At(image.Span(), entry).Value(), 0U) << index;
        EXPECT_EQ(ReadU32At(image.Span(), entry + 4).Value(), 0U) << index;
    }
}

TEST(SyntheticImage, WritesTheCardInfoHeader) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.title_version = 32;
    const SyntheticImage image = BuildSyntheticImage(spec);

    EXPECT_EQ(ReadU32At(image.Span(), 0x200).Value(), 0xFFFFFFFFU);
    EXPECT_EQ(ReadU16At(image.Span(), 0x310).Value(), 32U);
}

TEST(SyntheticImage, WritesTheNcchHeaderWhereTheFormatSaysItGoes) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const core::ByteSpan partition = image.Partition();

    EXPECT_TRUE(core::MatchesAt(partition, 0x100, "NCCH"));
    EXPECT_EQ(ReadU64At(partition, 0x118).Value(), PokemonXProgramId);
    EXPECT_EQ(ReadFixedAsciiAt(partition, 0x150, 0x10).Value(), "CTR-P-TEST");
    EXPECT_EQ(ReadU16At(partition, 0x112).Value(), 2U);

    // The size field reports the hashed half while the region on disk is
    // twice that, which is the discrepancy docs/formats/exheader.md explains.
    EXPECT_EQ(ReadU32At(partition, 0x180).Value(), 0x400U);
    EXPECT_EQ(image.exheader_offset, image.partition_offset + 0x200);
}

TEST(SyntheticImage, SetsTheNoCryptoFlag) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    EXPECT_EQ(ReadU8At(image.Partition(), 0x18F).Value(), 0x04U);
    EXPECT_EQ(ReadU8At(image.Partition(), 0x18D).Value(), 0x03U);
    EXPECT_EQ(ReadU8At(image.Partition(), 0x18C).Value(), 0x01U);
}

TEST(SyntheticImage, CanBuildAnEncryptedPartitionForAParserToRefuse) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.no_crypto = false;
    const SyntheticImage image = BuildSyntheticImage(spec);
    EXPECT_EQ(ReadU8At(image.Partition(), 0x18F).Value(), 0x00U);
}

TEST(SyntheticImage, PointsTheRegionFieldsAtTheRegions) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const core::ByteSpan partition = image.Partition();

    const std::size_t relative_logo = image.logo_offset - image.partition_offset;
    const std::size_t relative_plain = image.plain_offset - image.partition_offset;
    const std::size_t relative_exefs = image.exefs_offset - image.partition_offset;
    const std::size_t relative_romfs = image.romfs_offset - image.partition_offset;

    EXPECT_EQ(ReadU32At(partition, 0x198).Value(), relative_logo / MediaUnit);
    EXPECT_EQ(ReadU32At(partition, 0x190).Value(), relative_plain / MediaUnit);
    EXPECT_EQ(ReadU32At(partition, 0x1A0).Value(), relative_exefs / MediaUnit);
    EXPECT_EQ(ReadU32At(partition, 0x1B0).Value(), relative_romfs / MediaUnit);

    EXPECT_EQ(ReadU32At(partition, 0x19C).Value(), image.logo_size / MediaUnit);
    EXPECT_EQ(ReadU32At(partition, 0x194).Value(), image.plain_size / MediaUnit);
    EXPECT_EQ(ReadU32At(partition, 0x1A4).Value(), image.exefs_size / MediaUnit);
    EXPECT_EQ(ReadU32At(partition, 0x1B4).Value(), image.romfs_size / MediaUnit);
}

TEST(SyntheticImage, LeavesAnAbsentRegionReadingZero) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.romfs.clear();
    spec.logo.clear();
    const SyntheticImage image = BuildSyntheticImage(spec);

    EXPECT_EQ(ReadU32At(image.Partition(), 0x1B0).Value(), 0U);
    EXPECT_EQ(ReadU32At(image.Partition(), 0x1B4).Value(), 0U);
    EXPECT_EQ(ReadU32At(image.Partition(), 0x198).Value(), 0U);
    EXPECT_EQ(image.romfs_offset, 0U);
    EXPECT_EQ(image.romfs_size, 0U);
}

TEST(SyntheticImage, WritesTheExtendedHeader) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.application_title = "kujira-1";
    spec.compress_exefs_code = true;
    const SyntheticImage image = BuildSyntheticImage(spec);
    const core::ByteSpan exheader = image.Span().subspan(image.exheader_offset, 0x800);

    EXPECT_EQ(ReadFixedAsciiAt(exheader, 0x00, 8).Value(), "kujira-1");
    EXPECT_EQ(ReadU8At(exheader, 0x0D).Value(), 0x01U);

    // Text code set: address, pages, size.
    EXPECT_EQ(ReadU32At(exheader, 0x10).Value(), 0x00100000U);
    EXPECT_EQ(ReadU32At(exheader, 0x14).Value(), 2U);
    EXPECT_EQ(ReadU32At(exheader, 0x18).Value(), 0x1800U);
    EXPECT_EQ(ReadU32At(exheader, 0x1C).Value(), 0x4000U);
    EXPECT_EQ(ReadU32At(exheader, 0x3C).Value(), 0x200U);

    // The access control info repeats the program ID, and the limitation
    // copy carries the wildcard that identifies it.
    EXPECT_EQ(ReadU64At(exheader, 0x200).Value(), PokemonXProgramId);
    EXPECT_EQ(ReadU64At(exheader, 0x600).Value(), 0x00040000FFFFFF00U);

    EXPECT_EQ(ReadFixedAsciiAt(exheader, 0x250, 8).Value(), "fs:USER");
    EXPECT_EQ(ReadFixedAsciiAt(exheader, 0x258, 8).Value(), "ldr:ro");
}

TEST(SyntheticImage, ClearsTheCompressionFlagWhenTheCodeIsNotCompressed) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    EXPECT_EQ(ReadU8At(image.Span(), image.exheader_offset + 0x0D).Value(), 0x00U);
}

TEST(SyntheticImage, WritesTheExeFsFileHeaders) {
    const SyntheticImageSpec spec = DefaultImageSpec();
    const SyntheticImage image = BuildSyntheticImage(spec);
    const core::ByteSpan exefs = image.Span().subspan(image.exefs_offset, image.exefs_size);

    ASSERT_EQ(spec.exefs_files.size(), 3U);
    EXPECT_EQ(ReadFixedAsciiAt(exefs, 0x00, 8).Value(), ".code");
    EXPECT_EQ(ReadU32At(exefs, 0x08).Value(), 0U);
    EXPECT_EQ(ReadU32At(exefs, 0x0C).Value(), spec.exefs_files[0].data.size());
    EXPECT_EQ(ReadFixedAsciiAt(exefs, 0x10, 8).Value(), "banner");
    EXPECT_EQ(ReadFixedAsciiAt(exefs, 0x20, 8).Value(), "icon");
}

TEST(SyntheticImage, ZeroesTheUnusedExeFsFileHeaders) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const core::ByteSpan exefs = image.Span().subspan(image.exefs_offset, image.exefs_size);

    // The format allows ten entries, and the default image uses three.
    for (std::size_t index = 3; index < ExeFsFileHeaderCount; ++index) {
        EXPECT_EQ(ReadU64At(exefs, index * ExeFsFileHeaderSize).Value(), 0U) << index;
    }
}

TEST(SyntheticImage, PlacesExeFsFileDataAfterTheHeader) {
    const SyntheticImageSpec spec = DefaultImageSpec();
    const SyntheticImage image = BuildSyntheticImage(spec);

    // A file's data begins at the ExeFS offset plus 0x200 plus the entry's
    // offset.
    ASSERT_EQ(image.exefs_file_offsets.size(), spec.exefs_files.size());
    EXPECT_EQ(image.exefs_file_offsets[0], image.exefs_offset + 0x200);
    EXPECT_EQ(image.bytes[image.exefs_file_offsets[0]], spec.exefs_files[0].data[0]);
    EXPECT_EQ(image.bytes[image.exefs_file_offsets[1]], spec.exefs_files[1].data[0]);
}

TEST(SyntheticImage, StoresTheExeFsHashesInReverseOrder) {
    const SyntheticImageSpec spec = DefaultImageSpec();
    const SyntheticImage image = BuildSyntheticImage(spec);
    const core::ByteSpan exefs = image.Span().subspan(image.exefs_offset, image.exefs_size);

    // The hash at 0x1E0 belongs to the first file and the one at 0x0C0 to the
    // tenth, so the three present files occupy 0x1E0, 0x1C0 and 0x1A0.
    const core::Digest code = core::Sha256::Of(core::ByteSpan{spec.exefs_files[0].data});
    const core::Digest banner = core::Sha256::Of(core::ByteSpan{spec.exefs_files[1].data});
    const core::Digest icon = core::Sha256::Of(core::ByteSpan{spec.exefs_files[2].data});

    EXPECT_EQ(core::ReadBytesAt(exefs, 0x1E0, 0x20).Value()[0], code.bytes[0]);
    EXPECT_TRUE(std::equal(code.bytes.begin(),
                           code.bytes.end(),
                           core::ReadBytesAt(exefs, 0x1E0, 0x20).Value().begin()));
    EXPECT_TRUE(std::equal(banner.bytes.begin(),
                           banner.bytes.end(),
                           core::ReadBytesAt(exefs, 0x1C0, 0x20).Value().begin()));
    EXPECT_TRUE(std::equal(icon.bytes.begin(),
                           icon.bytes.end(),
                           core::ReadBytesAt(exefs, 0x1A0, 0x20).Value().begin()));
}

TEST(SyntheticImage, HashesTheExtendedHeaderOverItsHashedHalfOnly) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const core::Digest expected =
        core::Sha256::Of(image.Span().subspan(image.exheader_offset, 0x400));

    EXPECT_TRUE(std::equal(expected.bytes.begin(),
                           expected.bytes.end(),
                           core::ReadBytesAt(image.Partition(), 0x160, 0x20).Value().begin()));
    EXPECT_TRUE(std::equal(expected.bytes.begin(),
                           expected.bytes.end(),
                           core::ReadBytesAt(image.Span(), 0x160, 0x20).Value().begin()));
}

TEST(SyntheticImage, KeepsTheCodeSizeEqualToThePageAlignedSegmentSum) {
    const SyntheticImageSpec spec = DefaultImageSpec();
    const std::size_t pages = spec.text.pages + spec.read_only.pages + spec.data.pages;
    EXPECT_EQ(spec.exefs_files[0].data.size(), pages * 0x1000);
}

TEST(SyntheticImage, BuildsTheSameBytesTwice) {
    const SyntheticImage first = BuildSyntheticImage(DefaultImageSpec());
    const SyntheticImage second = BuildSyntheticImage(DefaultImageSpec());
    EXPECT_EQ(first.bytes, second.bytes);
}

TEST(PatternBytes, IsDeterministicAndNotConstant) {
    const std::vector<std::uint8_t> first = PatternBytes(300, 0x20);
    EXPECT_EQ(first, PatternBytes(300, 0x20));
    EXPECT_EQ(first.size(), 300U);
    EXPECT_NE(first[0], first[1]);
    EXPECT_NE(PatternBytes(16, 0x20), PatternBytes(16, 0x40));
}

}  // namespace
}  // namespace hexlith::test
