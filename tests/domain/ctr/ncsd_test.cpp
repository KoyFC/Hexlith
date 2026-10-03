#include "domain/ctr/ncsd.h"

#include "support/synthetic_ctr_image.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::domain::ctr {
namespace {

using test::BuildSyntheticImage;
using test::DefaultImageSpec;
using test::ManualPartitionIndex;
using test::PokemonXProgramId;
using test::SyntheticImage;
using test::UpdatePartitionIndex;

constexpr std::size_t AbsentPartition = 3;

// Enough of a size field to put the partition past the end of any image the
// builder produces.
constexpr std::uint8_t OversizedSizeByte = 0xFF;

TEST(ParseNcsdHeader, ReadsTheHeaderOfAWellFormedImage) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());

    const core::Result<NcsdHeader> header = ParseNcsdHeader(image.Span());
    ASSERT_TRUE(header.Ok()) << header.Failure().Message();
    EXPECT_EQ(header.Value().media_id, PokemonXProgramId);
    EXPECT_EQ(header.Value().image_size, image.bytes.size());
}

TEST(ParseNcsdHeader, ConvertsThePartitionTableFromMediaUnitsToBytes) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());

    const core::Result<NcsdHeader> header = ParseNcsdHeader(image.Span());
    ASSERT_TRUE(header.Ok()) << header.Failure().Message();

    const PartitionEntry& executable = header.Value().Partition(ncsd::ExecutableContentPartition);
    EXPECT_TRUE(executable.Present());
    EXPECT_EQ(executable.offset, image.partition_offset);
    EXPECT_EQ(executable.size, image.partition_size);
    EXPECT_EQ(executable.index, ncsd::ExecutableContentPartition);
}

TEST(ParseNcsdHeader, ReportsWhichPartitionsArePresent) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());

    const core::Result<NcsdHeader> header = ParseNcsdHeader(image.Span());
    ASSERT_TRUE(header.Ok()) << header.Failure().Message();

    EXPECT_EQ(header.Value().PresentPartitionCount(), 3U);
    EXPECT_TRUE(header.Value().Partition(ManualPartitionIndex).Present());
    EXPECT_TRUE(header.Value().Partition(UpdatePartitionIndex).Present());
    EXPECT_FALSE(header.Value().Partition(AbsentPartition).Present());
    EXPECT_EQ(header.Value().Partition(AbsentPartition).offset, 0U);
    EXPECT_EQ(header.Value().Partition(AbsentPartition).size, 0U);
}

TEST(ParseNcsdHeader, ReadsTheCardInfoHeader) {
    test::SyntheticImageSpec spec = DefaultImageSpec();
    spec.title_version = 32;
    const SyntheticImage image = BuildSyntheticImage(spec);

    const core::Result<NcsdHeader> header = ParseNcsdHeader(image.Span());
    ASSERT_TRUE(header.Ok()) << header.Failure().Message();
    EXPECT_TRUE(header.Value().card_info_present);
    EXPECT_EQ(header.Value().title_version, 32U);
    EXPECT_EQ(header.Value().card_revision, 0U);
    EXPECT_EQ(header.Value().filled_size, image.bytes.size());
}

TEST(ParseNcsdHeader, RefusesSomethingThatIsNotAnNcsd) {
    SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    image.bytes[ncsd::MagicOffset] = 'X';

    const core::Result<NcsdHeader> header = ParseNcsdHeader(image.Span());
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("no NCSD magic"), std::string::npos)
        << header.Failure().Message();
}

TEST(ParseNcsdHeader, RefusesABufferTooShortForTheHeader) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const core::Result<NcsdHeader> header =
        ParseNcsdHeader(image.Span().subspan(0, ncsd::HeaderSize - 1));
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("an NCSD header is"), std::string::npos);
}

TEST(ParseNcsdHeader, RefusesATruncatedImage) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());

    // The header still declares the full size, so the shortened span is an
    // image that was cut off rather than one that was written small.
    const core::Result<NcsdHeader> header =
        ParseNcsdHeader(image.Span().subspan(0, image.bytes.size() - MediaUnit));
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("truncated"), std::string::npos)
        << header.Failure().Message();
}

TEST(ParseNcsdHeader, RefusesADeclaredSizeOfZero) {
    SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    for (std::size_t index = 0; index < 4; ++index) {
        image.bytes[ncsd::ImageSizeOffset + index] = 0;
    }

    const core::Result<NcsdHeader> header = ParseNcsdHeader(image.Span());
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("image size of zero"), std::string::npos);
}

TEST(ParseNcsdHeader, RefusesAPartitionThatRunsPastTheEnd) {
    SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());

    // Enlarge partition 0 beyond the image without changing anything else.
    const std::size_t size_field =
        ncsd::PartitionTableOffset +
        (ncsd::ExecutableContentPartition * ncsd::PartitionTableEntrySize) + 4;
    image.bytes[size_field + 2] = OversizedSizeByte;

    const core::Result<NcsdHeader> header = ParseNcsdHeader(image.Span());
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("past the end"), std::string::npos)
        << header.Failure().Message();
}

TEST(ParseNcsdHeader, RefusesAPartitionThatIsNeitherPresentNorAbsent) {
    SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());

    // A size with no offset. Deciding which half to believe is how an
    // extractor ends up reading from offset zero.
    const std::size_t entry =
        ncsd::PartitionTableOffset + (AbsentPartition * ncsd::PartitionTableEntrySize);
    image.bytes[entry + 4] = 1;

    const core::Result<NcsdHeader> header = ParseNcsdHeader(image.Span());
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("neither present nor absent"), std::string::npos)
        << header.Failure().Message();
}

TEST(PartitionContents, ReturnsTheBytesOfThePartition) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const core::Result<NcsdHeader> header = ParseNcsdHeader(image.Span());
    ASSERT_TRUE(header.Ok()) << header.Failure().Message();

    const core::Result<core::ByteSpan> partition =
        PartitionContents(image.Span(), header.Value(), ncsd::ExecutableContentPartition);
    ASSERT_TRUE(partition.Ok()) << partition.Failure().Message();
    EXPECT_EQ(partition.Value().size(), image.partition_size);
    EXPECT_TRUE(core::MatchesAt(partition.Value(), ncch::MagicOffset, ncch::Magic));
}

TEST(PartitionContents, RefusesAnAbsentPartition) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const core::Result<NcsdHeader> header = ParseNcsdHeader(image.Span());
    ASSERT_TRUE(header.Ok()) << header.Failure().Message();

    const core::Result<core::ByteSpan> partition =
        PartitionContents(image.Span(), header.Value(), AbsentPartition);
    ASSERT_FALSE(partition.Ok());
    EXPECT_NE(partition.Failure().Message().find("is absent"), std::string::npos);
}

TEST(PartitionContents, RefusesAnIndexOutsideTheTable) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const core::Result<NcsdHeader> header = ParseNcsdHeader(image.Span());
    ASSERT_TRUE(header.Ok()) << header.Failure().Message();

    const core::Result<core::ByteSpan> partition =
        PartitionContents(image.Span(), header.Value(), ncsd::PartitionCount);
    ASSERT_FALSE(partition.Ok());
    EXPECT_NE(partition.Failure().Message().find("does not exist"), std::string::npos);
}

TEST(PartitionContents, SelectsTheNamedPartitionRatherThanTheFirstUsableOne) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const core::Result<NcsdHeader> header = ParseNcsdHeader(image.Span());
    ASSERT_TRUE(header.Ok()) << header.Failure().Message();

    const core::Result<core::ByteSpan> manual =
        PartitionContents(image.Span(), header.Value(), ManualPartitionIndex);
    ASSERT_TRUE(manual.Ok()) << manual.Failure().Message();
    EXPECT_NE(manual.Value().data(), image.Partition().data());
    EXPECT_EQ(manual.Value().size(), header.Value().Partition(ManualPartitionIndex).size);
}

}  // namespace
}  // namespace hexlith::domain::ctr
