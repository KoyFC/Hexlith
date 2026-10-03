#include "core/sha256.h"
#include "domain/ctr/ncch.h"

#include "support/synthetic_ctr_image.h"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>

namespace hexlith::domain::ctr {
namespace {

using test::BuildSyntheticImage;
using test::DefaultImageSpec;
using test::PokemonXProgramId;
using test::SyntheticImage;
using test::SyntheticImageSpec;

// Enough of a size field to put a region past the end of any image the builder
// produces.
constexpr std::uint8_t OversizedSizeByte = 0xFF;

// Data content without the executable bit, which is what a CFA reports.
constexpr std::uint8_t DataOnlyContent = 0x01;

NcchHeader ParseOrFail(const SyntheticImage& image) {
    const core::Result<NcchHeader> header = ParseNcchHeader(image.Partition());
    EXPECT_TRUE(header.Ok()) << header.Failure().Message();
    return header.Ok() ? header.Value() : NcchHeader{};
}

TEST(ParseNcchHeader, ReadsTheIdentityFields) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.product_code = "CTR-P-EKJA";
    const SyntheticImage image = BuildSyntheticImage(spec);

    const NcchHeader header = ParseOrFail(image);
    EXPECT_EQ(header.program_id, PokemonXProgramId);
    EXPECT_EQ(header.partition_id, PokemonXProgramId);
    EXPECT_EQ(header.product_code, "CTR-P-EKJA");
    EXPECT_EQ(header.maker_code, test::NintendoMakerCode);
    EXPECT_EQ(header.format_version, test::StandardNcchVersion);
    EXPECT_EQ(header.content_size, image.partition_size);
}

TEST(ParseNcchHeader, ReadsTheFlags) {
    const NcchHeader header = ParseOrFail(BuildSyntheticImage(DefaultImageSpec()));

    EXPECT_TRUE(header.flags.NoCrypto());
    EXPECT_FALSE(header.flags.FixedCryptoKey());
    EXPECT_FALSE(header.flags.NoMountRomFs());
    EXPECT_TRUE(header.flags.Executable());
    EXPECT_EQ(header.flags.content_platform, test::ContentPlatformCtr);
    EXPECT_EQ(header.flags.ContentUnitSize(), MediaUnit);
}

TEST(ParseNcchHeader, ReportsADataOnlyPartitionAsNotExecutable) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.content_type = DataOnlyContent;
    const NcchHeader header = ParseOrFail(BuildSyntheticImage(spec));
    EXPECT_FALSE(header.flags.Executable());
}

TEST(ParseNcchHeader, ReportsTheRegionsInBytesRelativeToThePartition) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const NcchHeader header = ParseOrFail(image);

    EXPECT_EQ(header.logo.offset, image.logo_offset - image.partition_offset);
    EXPECT_EQ(header.logo.size, image.logo_size);
    EXPECT_EQ(header.plain.offset, image.plain_offset - image.partition_offset);
    EXPECT_EQ(header.plain.size, image.plain_size);
    EXPECT_EQ(header.exefs.offset, image.exefs_offset - image.partition_offset);
    EXPECT_EQ(header.exefs.size, image.exefs_size);
    EXPECT_EQ(header.romfs.offset, image.romfs_offset - image.partition_offset);
    EXPECT_EQ(header.romfs.size, image.romfs_size);
}

// Reading the RomFS fields four bytes late yields the hash region size and a
// zero, which looks like a title with no RomFS. docs/formats/ncch.md records
// that this mistake was made and caught during the initial survey.
TEST(ParseNcchHeader, DoesNotReadTheRomFsFieldsFourBytesLate) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const NcchHeader header = ParseOrFail(image);

    ASSERT_TRUE(header.romfs.Present());
    EXPECT_NE(header.romfs.offset, header.romfs_hash_region_size);
    EXPECT_EQ(header.romfs_hash_region_size, MediaUnit);
}

TEST(ParseNcchHeader, ReportsTheExtendedHeaderRegionRatherThanTheDeclaredSize) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const NcchHeader header = ParseOrFail(image);

    EXPECT_EQ(header.declared_exheader_size, exheader::HashedSize);
    EXPECT_EQ(header.exheader.size, exheader::SizeOnDisk);
    EXPECT_EQ(header.exheader.offset, ncch::HeaderSize);
    EXPECT_EQ(header.exheader.offset, image.exheader_offset - image.partition_offset);
}

TEST(ParseNcchHeader, ReadsTheStoredHashes) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const NcchHeader header = ParseOrFail(image);

    const core::Digest exheader =
        core::Sha256::Of(image.Span().subspan(image.exheader_offset, exheader::HashedSize));
    EXPECT_EQ(header.exheader_hash, exheader);

    const core::Digest logo =
        core::Sha256::Of(image.Span().subspan(image.logo_offset, image.logo_size));
    EXPECT_EQ(header.logo_hash, logo);

    const core::Digest exefs =
        core::Sha256::Of(image.Span().subspan(image.exefs_offset, MediaUnit));
    EXPECT_EQ(header.exefs_superblock_hash, exefs);
}

TEST(ParseNcchHeader, RefusesAnEncryptedPartition) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.no_crypto = false;
    const core::Result<NcchHeader> header = ParseNcchHeader(BuildSyntheticImage(spec).Partition());

    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("handles no keys"), std::string::npos)
        << header.Failure().Message();
}

TEST(ParseNcchHeader, RefusesSomethingThatIsNotAnNcch) {
    SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    image.bytes[image.partition_offset + ncch::MagicOffset + 1] = 'X';

    const core::Result<NcchHeader> header = ParseNcchHeader(image.Partition());
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("no NCCH magic"), std::string::npos);
}

TEST(ParseNcchHeader, RefusesABufferTooShortForTheHeader) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const core::Result<NcchHeader> header =
        ParseNcchHeader(image.Partition().subspan(0, ncch::HeaderSize - 1));
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("an NCCH header is"), std::string::npos);
}

TEST(ParseNcchHeader, RefusesAPartitionShorterThanItsDeclaredContent) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const core::Result<NcchHeader> header =
        ParseNcchHeader(image.Partition().subspan(0, image.partition_size - MediaUnit));
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("truncated"), std::string::npos)
        << header.Failure().Message();
}

TEST(ParseNcchHeader, RefusesARegionThatRunsPastTheEnd) {
    SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    image.bytes[image.partition_offset + ncch::RomFsSizeOffset + 2] = OversizedSizeByte;

    const core::Result<NcchHeader> header = ParseNcchHeader(image.Partition());
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("past the end"), std::string::npos)
        << header.Failure().Message();
}

TEST(ParseNcchHeader, RefusesARegionPlacedOverTheHeader) {
    SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    for (std::size_t index = 0; index < 4; ++index) {
        image.bytes[image.partition_offset + ncch::LogoRegionOffsetOffset + index] = 0;
    }

    const core::Result<NcchHeader> header = ParseNcchHeader(image.Partition());
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("offset zero"), std::string::npos)
        << header.Failure().Message();
}

TEST(ParseNcchHeader, RefusesAnUnexpectedExtendedHeaderSize) {
    SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    image.bytes[image.partition_offset + ncch::ExHeaderSizeOffset] = 0x10;

    const core::Result<NcchHeader> header = ParseNcchHeader(image.Partition());
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("extended header size field"), std::string::npos)
        << header.Failure().Message();
}

TEST(ParseNcchHeader, AcceptsAPartitionWithNoExtendedHeader) {
    SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    for (std::size_t index = 0; index < 4; ++index) {
        image.bytes[image.partition_offset + ncch::ExHeaderSizeOffset + index] = 0;
    }

    const core::Result<NcchHeader> header = ParseNcchHeader(image.Partition());
    ASSERT_TRUE(header.Ok()) << header.Failure().Message();
    EXPECT_FALSE(header.Value().exheader.Present());
}

TEST(RegionContents, ReturnsTheBytesOfTheRegion) {
    const SyntheticImageSpec spec = DefaultImageSpec();
    const SyntheticImage image = BuildSyntheticImage(spec);
    const NcchHeader header = ParseOrFail(image);

    const core::Result<core::ByteSpan> logo =
        RegionContents(image.Partition(), header.logo, "logo region");
    ASSERT_TRUE(logo.Ok()) << logo.Failure().Message();
    EXPECT_EQ(logo.Value().size(), image.logo_size);
    EXPECT_TRUE(std::equal(spec.logo.begin(), spec.logo.end(), logo.Value().begin()));
}

TEST(RegionContents, RefusesAnAbsentRegion) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.romfs.clear();
    const SyntheticImage image = BuildSyntheticImage(spec);
    const NcchHeader header = ParseOrFail(image);

    const core::Result<core::ByteSpan> romfs =
        RegionContents(image.Partition(), header.romfs, "RomFS");
    ASSERT_FALSE(romfs.Ok());
    EXPECT_NE(romfs.Failure().Message().find("is absent"), std::string::npos);
}

}  // namespace
}  // namespace hexlith::domain::ctr
