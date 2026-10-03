#include "domain/ctr/romfs.h"

#include "support/synthetic_romfs.h"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <ranges>
#include <string>
#include <vector>

namespace hexlith::domain::ctr {
namespace {

using test::BuildSyntheticRomFs;
using test::DefaultRomFsSpec;
using test::SyntheticRomFsFile;
using test::SyntheticRomFsSpec;

// A header length field that is not 0x28, which is how a parser finds out it
// looked somewhere other than the start of level 3.
constexpr std::uint8_t WrongHeaderLength = 0x30;

// Enough of a length field to run a file past the end of the region.
constexpr std::uint8_t OversizedLengthByte = 0xFF;

RomFs ParseOrFail(const std::vector<std::uint8_t>& region) {
    const core::Result<RomFs> filesystem = ParseRomFs(core::ByteSpan{region});
    EXPECT_TRUE(filesystem.Ok()) << filesystem.Failure().Message();
    return filesystem.Ok() ? filesystem.Value() : RomFs{};
}

TEST(ParseRomFs, FindsEveryFileInTheTree) {
    const SyntheticRomFsSpec spec = DefaultRomFsSpec();
    const RomFs filesystem = ParseOrFail(BuildSyntheticRomFs(spec));

    EXPECT_EQ(filesystem.files.size(), spec.files.size());
    for (const SyntheticRomFsFile& file : spec.files) {
        EXPECT_NE(filesystem.Find(file.path), nullptr) << file.path;
    }
}

TEST(ParseRomFs, BuildsPathsFromTheDirectoryTree) {
    const RomFs filesystem = ParseOrFail(BuildSyntheticRomFs(DefaultRomFsSpec()));

    ASSERT_NE(filesystem.Find("a/0/1"), nullptr);
    ASSERT_NE(filesystem.Find("sound/stream/bgm.bcstm"), nullptr);
    ASSERT_NE(filesystem.Find(".crr/static.crr"), nullptr);
    EXPECT_EQ(filesystem.Find("0"), nullptr) << "a nested file is not reachable by its name alone";
}

TEST(ParseRomFs, DecodesNamesThatAreNotAscii) {
    const RomFs filesystem = ParseOrFail(BuildSyntheticRomFs(DefaultRomFsSpec()));
    EXPECT_NE(filesystem.Find("m/\xC3\xB1\xE6\x97\xA5.moflex"), nullptr);
}

TEST(ParseRomFs, ReportsDirectoriesIncludingEmptyOnes) {
    const RomFs filesystem = ParseOrFail(BuildSyntheticRomFs(DefaultRomFsSpec()));

    const auto has = [&filesystem](std::string_view path) {
        return std::ranges::find(filesystem.directories, path) != filesystem.directories.end();
    };
    EXPECT_TRUE(has("a"));
    EXPECT_TRUE(has("a/0"));
    EXPECT_TRUE(has("sound/stream"));
    EXPECT_TRUE(has("shaders")) << "a directory with nothing in it is still a directory";
    EXPECT_FALSE(has("")) << "the root is not listed as one of its own entries";
}

TEST(ParseRomFs, SortsBothListsByPath) {
    const RomFs filesystem = ParseOrFail(BuildSyntheticRomFs(DefaultRomFsSpec()));

    EXPECT_TRUE(std::ranges::is_sorted(filesystem.directories));
    EXPECT_TRUE(
        std::ranges::is_sorted(filesystem.files, [](const RomFsFile& left, const RomFsFile& right) {
            return left.path < right.path;
        }));
}

TEST(ParseRomFs, ReportsOffsetsRelativeToTheRegion) {
    const SyntheticRomFsSpec spec = DefaultRomFsSpec();
    const std::vector<std::uint8_t> region = BuildSyntheticRomFs(spec);
    const RomFs filesystem = ParseOrFail(region);

    EXPECT_EQ(filesystem.level3_offset, romfs::Level3Offset);
    EXPECT_GT(filesystem.file_data_offset, romfs::Level3Offset);
    for (const RomFsFile& file : filesystem.files) {
        EXPECT_GE(file.offset, filesystem.file_data_offset) << file.path;
        EXPECT_LE(file.offset + file.size, region.size()) << file.path;
    }
}

TEST(RomFsFileContents, ReturnsTheBytesOfEachFile) {
    const SyntheticRomFsSpec spec = DefaultRomFsSpec();
    const std::vector<std::uint8_t> region = BuildSyntheticRomFs(spec);
    const RomFs filesystem = ParseOrFail(region);

    for (const SyntheticRomFsFile& expected : spec.files) {
        const RomFsFile* file = filesystem.Find(expected.path);
        ASSERT_NE(file, nullptr) << expected.path;
        ASSERT_EQ(file->size, expected.data.size()) << expected.path;

        const core::Result<core::ByteSpan> contents =
            RomFsFileContents(core::ByteSpan{region}, *file);
        ASSERT_TRUE(contents.Ok()) << contents.Failure().Message();
        EXPECT_TRUE(
            std::equal(expected.data.begin(), expected.data.end(), contents.Value().begin()))
            << expected.path;
    }
}

TEST(RomFs, SumsTheBytesItWouldWrite) {
    const SyntheticRomFsSpec spec = DefaultRomFsSpec();
    const RomFs filesystem = ParseOrFail(BuildSyntheticRomFs(spec));

    std::size_t expected = 0;
    for (const SyntheticRomFsFile& file : spec.files) {
        expected += file.data.size();
    }
    EXPECT_EQ(filesystem.TotalFileBytes(), expected);
}

TEST(ParseRomFs, ParsesAFilesystemWithNothingInIt) {
    const RomFs filesystem = ParseOrFail(BuildSyntheticRomFs(SyntheticRomFsSpec{}));
    EXPECT_TRUE(filesystem.files.empty());
    EXPECT_TRUE(filesystem.directories.empty());
}

TEST(ParseRomFs, RefusesARegionWithoutTheIvfcMagic) {
    std::vector<std::uint8_t> region = BuildSyntheticRomFs(DefaultRomFsSpec());
    region[1] = 'X';

    const core::Result<RomFs> filesystem = ParseRomFs(core::ByteSpan{region});
    ASSERT_FALSE(filesystem.Ok());
    EXPECT_NE(filesystem.Failure().Message().find("no IVFC magic"), std::string::npos);
}

// The logical offsets in the IVFC header are not file offsets. Level 3 is at
// a fixed 0x1000, and a header length field that does not read 0x28 is how a
// parser finds out it looked somewhere else.
TEST(ParseRomFs, RefusesARegionWhoseLevel3HeaderIsNotWhereItShouldBe) {
    std::vector<std::uint8_t> region = BuildSyntheticRomFs(DefaultRomFsSpec());
    region[romfs::Level3Offset] = WrongHeaderLength;

    const core::Result<RomFs> filesystem = ParseRomFs(core::ByteSpan{region});
    ASSERT_FALSE(filesystem.Ok());
    EXPECT_NE(filesystem.Failure().Message().find("level 3 header length"), std::string::npos)
        << filesystem.Failure().Message();
}

TEST(ParseRomFs, RefusesARegionTooShortForLevel3) {
    const std::vector<std::uint8_t> region = BuildSyntheticRomFs(DefaultRomFsSpec());
    const core::Result<RomFs> filesystem =
        ParseRomFs(core::ByteSpan{region}.subspan(0, romfs::Level3Offset + 4));
    ASSERT_FALSE(filesystem.Ok());
    EXPECT_NE(filesystem.Failure().Message().find("the level 3 header"), std::string::npos)
        << filesystem.Failure().Message();
}

// A filesystem image is untrusted input, so a name that could reach outside
// the output directory is refused rather than sanitised. Sanitising would
// mean two different names collapsing onto the same file.
TEST(ParseRomFs, RefusesANameHoldingAPathSeparator) {
    SyntheticRomFsSpec spec;
    spec.files = {SyntheticRomFsFile{.path = "plain", .data = {1, 2, 3}}};
    std::vector<std::uint8_t> region = BuildSyntheticRomFs(spec);

    // The name follows the entry header, as UTF-16, so its third character
    // is four bytes in.
    const std::size_t level3 = romfs::Level3Offset;
    const std::size_t file_table =
        level3 +
        core::ReadU32At(core::ByteSpan{region}, level3 + romfs::FileMetadataOffsetOffset).Value();
    region[file_table + romfs::FileEntrySize + 4] = '/';

    const core::Result<RomFs> filesystem = ParseRomFs(core::ByteSpan{region});
    ASSERT_FALSE(filesystem.Ok());
    EXPECT_NE(filesystem.Failure().Message().find("path separator"), std::string::npos)
        << filesystem.Failure().Message();
}

TEST(ParseRomFs, RefusesANameThatNamesItsOwnParent) {
    SyntheticRomFsSpec spec;
    spec.files = {SyntheticRomFsFile{.path = "ab", .data = {1}}};
    std::vector<std::uint8_t> region = BuildSyntheticRomFs(spec);

    const std::size_t level3 = romfs::Level3Offset;
    const std::size_t file_table =
        level3 +
        core::ReadU32At(core::ByteSpan{region}, level3 + romfs::FileMetadataOffsetOffset).Value();
    region[file_table + romfs::FileEntrySize] = '.';
    region[file_table + romfs::FileEntrySize + 2] = '.';

    const core::Result<RomFs> filesystem = ParseRomFs(core::ByteSpan{region});
    ASSERT_FALSE(filesystem.Ok());
    EXPECT_NE(filesystem.Failure().Message().find("names a directory"), std::string::npos)
        << filesystem.Failure().Message();
}

TEST(ParseRomFs, RefusesAFileWhoseDataRunsPastTheEnd) {
    SyntheticRomFsSpec spec;
    spec.files = {SyntheticRomFsFile{.path = "plain", .data = {1, 2, 3}}};
    std::vector<std::uint8_t> region = BuildSyntheticRomFs(spec);

    const RomFs filesystem = ParseOrFail(region);
    ASSERT_EQ(filesystem.files.size(), 1U);

    // Find the file metadata entry through the header and enlarge its length.
    const std::size_t level3 = romfs::Level3Offset;
    const std::size_t file_table =
        level3 +
        core::ReadU32At(core::ByteSpan{region}, level3 + romfs::FileMetadataOffsetOffset).Value();
    region[file_table + romfs::FileDataLengthField + 3] = OversizedLengthByte;

    const core::Result<RomFs> reparsed = ParseRomFs(core::ByteSpan{region});
    ASSERT_FALSE(reparsed.Ok());
    EXPECT_NE(reparsed.Failure().Message().find("past the end"), std::string::npos)
        << reparsed.Failure().Message();
}

TEST(ParseRomFs, RefusesASiblingChainThatLoops) {
    SyntheticRomFsSpec spec;
    spec.files = {SyntheticRomFsFile{.path = "one", .data = {1}},
                  SyntheticRomFsFile{.path = "two", .data = {2}}};
    std::vector<std::uint8_t> region = BuildSyntheticRomFs(spec);

    const std::size_t level3 = romfs::Level3Offset;
    const std::size_t file_table =
        level3 +
        core::ReadU32At(core::ByteSpan{region}, level3 + romfs::FileMetadataOffsetOffset).Value();

    // Point the first file's sibling at itself.
    for (std::size_t index = 0; index < 4; ++index) {
        region[file_table + romfs::FileNextSiblingOffset + index] = 0;
    }

    const core::Result<RomFs> reparsed = ParseRomFs(core::ByteSpan{region});
    ASSERT_FALSE(reparsed.Ok());
    EXPECT_NE(reparsed.Failure().Message().find("appears twice"), std::string::npos)
        << reparsed.Failure().Message();
}

}  // namespace
}  // namespace hexlith::domain::ctr
