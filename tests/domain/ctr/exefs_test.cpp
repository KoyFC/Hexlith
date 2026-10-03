#include "core/sha256.h"
#include "domain/ctr/backwards_lzss.h"
#include "domain/ctr/exefs.h"
#include "domain/ctr/exheader.h"

#include "support/crafted_lzss_stream.h"
#include "support/synthetic_ctr_image.h"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::domain::ctr {
namespace {

using test::BuildSyntheticImage;
using test::DefaultImageSpec;
using test::SyntheticImage;
using test::SyntheticImageSpec;

constexpr std::uint8_t UnreadableNameByte = 0x01;

// Enough of a size field to put a file past the end of the region.
constexpr std::uint8_t OversizedSizeByte = 0xFF;

core::ByteSpan Region(const SyntheticImage& image) {
    return image.Span().subspan(image.exefs_offset, image.exefs_size);
}

ExeFs ParseOrFail(const SyntheticImage& image) {
    const core::Result<ExeFs> filesystem = ParseExeFs(Region(image));
    EXPECT_TRUE(filesystem.Ok()) << filesystem.Failure().Message();
    return filesystem.Ok() ? filesystem.Value() : ExeFs{};
}

TEST(ParseExeFs, ReadsEveryDeclaredFile) {
    const SyntheticImageSpec spec = DefaultImageSpec();
    const ExeFs filesystem = ParseOrFail(BuildSyntheticImage(spec));

    ASSERT_EQ(filesystem.files.size(), 3U);
    EXPECT_EQ(filesystem.files[0].name, ".code");
    EXPECT_EQ(filesystem.files[0].offset, 0U);
    EXPECT_EQ(filesystem.files[0].size, spec.exefs_files[0].data.size());
    EXPECT_EQ(filesystem.files[1].name, "banner");
    EXPECT_EQ(filesystem.files[2].name, "icon");
    EXPECT_EQ(filesystem.files[2].index, 2U);
}

TEST(ParseExeFs, FindsAFileByName) {
    const ExeFs filesystem = ParseOrFail(BuildSyntheticImage(DefaultImageSpec()));

    ASSERT_NE(filesystem.Find(exefs::CodeFileName), nullptr);
    EXPECT_EQ(filesystem.Find(exefs::CodeFileName)->index, 0U);
    EXPECT_EQ(filesystem.Find("missing"), nullptr);
}

TEST(ParseExeFs, ReadsTheHashesFromTheReversedTable) {
    const SyntheticImageSpec spec = DefaultImageSpec();
    const ExeFs filesystem = ParseOrFail(BuildSyntheticImage(spec));

    for (std::size_t index = 0; index < filesystem.files.size(); ++index) {
        const core::Digest expected =
            core::Sha256::Of(core::ByteSpan{spec.exefs_files[index].data});
        EXPECT_EQ(filesystem.files[index].hash, expected) << filesystem.files[index].name;
    }
}

TEST(ParseExeFs, ReadsAllTenEntriesRatherThanEight) {
    // Ten files, which Azahar's extractor and the script derived from it
    // would not see the last two of.
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.exefs_files.clear();
    for (unsigned index = 0; index < exefs::MaxFileCount; ++index) {
        spec.exefs_files.push_back(test::ExeFsFileSpec{
            .name = "file" + std::to_string(index),
            .data = test::PatternBytes(0x40, static_cast<std::uint8_t>(index + 1))});
    }

    const ExeFs filesystem = ParseOrFail(BuildSyntheticImage(spec));
    ASSERT_EQ(filesystem.files.size(), exefs::MaxFileCount);
    EXPECT_EQ(filesystem.files[8].name, "file8");
    EXPECT_EQ(filesystem.files[9].name, "file9");
}

TEST(ExeFsFileContents, ReturnsTheBytesOfTheFile) {
    const SyntheticImageSpec spec = DefaultImageSpec();
    const SyntheticImage image = BuildSyntheticImage(spec);
    const ExeFs filesystem = ParseOrFail(image);

    for (std::size_t index = 0; index < filesystem.files.size(); ++index) {
        const core::Result<core::ByteSpan> contents =
            ExeFsFileContents(Region(image), filesystem.files[index]);
        ASSERT_TRUE(contents.Ok()) << contents.Failure().Message();
        EXPECT_TRUE(std::equal(spec.exefs_files[index].data.begin(),
                               spec.exefs_files[index].data.end(),
                               contents.Value().begin()))
            << filesystem.files[index].name;
    }
}

TEST(ExeFsFileContents, PlacesTheDataAreaAfterTheHeader) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const ExeFs filesystem = ParseOrFail(image);

    const core::Result<core::ByteSpan> code = ExeFsFileContents(Region(image), filesystem.files[0]);
    ASSERT_TRUE(code.Ok()) << code.Failure().Message();
    EXPECT_EQ(code.Value().size(), filesystem.files[0].size);
    EXPECT_EQ(
        core::ReadBytesAt(image.Span(), image.exefs_offset + exefs::HeaderSize, code.Value().size())
            .Value()[0],
        code.Value()[0]);
}

TEST(ParseExeFs, RefusesARegionTooShortForTheHeader) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const core::Result<ExeFs> filesystem =
        ParseExeFs(Region(image).subspan(0, exefs::HeaderSize - 1));
    ASSERT_FALSE(filesystem.Ok());
    EXPECT_NE(filesystem.Failure().Message().find("an ExeFS header is"), std::string::npos);
}

TEST(ParseExeFs, RefusesAFileRunningPastTheEnd) {
    SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    image.bytes[image.exefs_offset + exefs::FileSizeOffset + 2] = OversizedSizeByte;

    const core::Result<ExeFs> filesystem = ParseExeFs(Region(image));
    ASSERT_FALSE(filesystem.Ok());
    EXPECT_NE(filesystem.Failure().Message().find("past the end"), std::string::npos)
        << filesystem.Failure().Message();
}

TEST(ParseExeFs, RefusesAnEntryThatIsNeitherAFileNorUnused) {
    SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());

    // A name on an entry with no size, in one of the slots the default image
    // leaves zeroed.
    const std::size_t entry = image.exefs_offset + (3 * exefs::FileHeaderSize);
    image.bytes[entry] = 'x';

    const core::Result<ExeFs> filesystem = ParseExeFs(Region(image));
    ASSERT_FALSE(filesystem.Ok());
    EXPECT_NE(filesystem.Failure().Message().find("neither a file nor an unused entry"),
              std::string::npos)
        << filesystem.Failure().Message();
}

TEST(ParseExeFs, RefusesTwoFilesWithTheSameName) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.exefs_files[1].name = ".code";
    const SyntheticImage image = BuildSyntheticImage(spec);

    const core::Result<ExeFs> filesystem = ParseExeFs(Region(image));
    ASSERT_FALSE(filesystem.Ok());
    EXPECT_NE(filesystem.Failure().Message().find("are both named"), std::string::npos)
        << filesystem.Failure().Message();
}

TEST(ParseExeFs, RefusesOverlappingFiles) {
    SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());

    // Point the second file at the first one's data.
    const std::size_t entry = image.exefs_offset + exefs::FileHeaderSize;
    for (std::size_t index = 0; index < 4; ++index) {
        image.bytes[entry + exefs::FileOffsetOffset + index] = 0;
    }

    const core::Result<ExeFs> filesystem = ParseExeFs(Region(image));
    ASSERT_FALSE(filesystem.Ok());
    EXPECT_NE(filesystem.Failure().Message().find("overlaps"), std::string::npos)
        << filesystem.Failure().Message();
}

TEST(ParseExeFs, RefusesANameThatIsNotText) {
    SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    image.bytes[image.exefs_offset + 1] = UnreadableNameByte;

    const core::Result<ExeFs> filesystem = ParseExeFs(Region(image));
    ASSERT_FALSE(filesystem.Ok());
    EXPECT_NE(filesystem.Failure().Message().find("ExeFS entry 0"), std::string::npos)
        << filesystem.Failure().Message();
}

TEST(ParseExeFs, RefusesAnEmptyExeFs) {
    SyntheticImageSpec spec = DefaultImageSpec();
    const SyntheticImage image = BuildSyntheticImage(spec);

    std::vector<std::uint8_t> empty(exefs::HeaderSize, 0);
    const core::Result<ExeFs> filesystem = ParseExeFs(core::ByteSpan{empty});
    ASSERT_FALSE(filesystem.Ok());
    EXPECT_NE(filesystem.Failure().Message().find("declares no files"), std::string::npos);
}

// The extended header is the only thing that says .code is compressed, so
// this is the path the extractor takes: read the flag there, then decompress
// what the ExeFS hands over.
TEST(ExeFs, DecompressesACompressedCodeSectionThroughTheExtendedHeaderFlag) {
    const test::CraftedLzssStream crafted = test::RepeatedByteLzssStream();

    SyntheticImageSpec spec = DefaultImageSpec();
    spec.compress_exefs_code = true;
    spec.exefs_files[0].data = crafted.compressed;
    const SyntheticImage image = BuildSyntheticImage(spec);

    const core::Result<ExHeader> exheader =
        ParseExHeader(image.Span().subspan(image.exheader_offset, exheader::SizeOnDisk));
    ASSERT_TRUE(exheader.Ok()) << exheader.Failure().Message();
    ASSERT_TRUE(exheader.Value().compress_exefs_code);

    const ExeFs filesystem = ParseOrFail(image);
    const ExeFsFile* code = filesystem.Find(exefs::CodeFileName);
    ASSERT_NE(code, nullptr);

    const core::Result<core::ByteSpan> section = ExeFsFileContents(Region(image), *code);
    ASSERT_TRUE(section.Ok()) << section.Failure().Message();

    const core::Result<std::vector<std::uint8_t>> decompressed =
        DecompressBackwardsLzss(section.Value());
    ASSERT_TRUE(decompressed.Ok()) << decompressed.Failure().Message();
    EXPECT_EQ(decompressed.Value(), crafted.expected);
}

}  // namespace
}  // namespace hexlith::domain::ctr
