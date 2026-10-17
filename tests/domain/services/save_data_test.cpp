#include "domain/services/save_data.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::domain::services {
namespace {

// Path types. Source: 3dbrew, Filesystem services, Paths.
constexpr std::uint32_t BinaryPath = 2;
constexpr std::uint32_t AsciiPath = 3;
constexpr std::uint32_t Utf16Path = 4;

std::vector<std::uint8_t> Ascii(const std::string& text) {
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0);
    return bytes;
}

std::vector<std::uint8_t> Utf16(const std::u16string& text) {
    std::vector<std::uint8_t> bytes;
    for (const char16_t unit : text) {
        bytes.push_back(static_cast<std::uint8_t>(unit));
        bytes.push_back(static_cast<std::uint8_t>(unit >> 8U));
    }
    bytes.push_back(0);
    bytes.push_back(0);
    return bytes;
}

constexpr std::uint32_t ArchiveSize = 0x1000;

SaveData Formatted(ArchiveKind kind = ArchiveKind::SaveData) {
    SaveData save{kind};
    save.Reformat(SaveDataFormat{.total_size = ArchiveSize, .directories = 4, .files = 4});
    return save;
}

constexpr OpenMode ReadWrite{.read = true, .write = true, .create = false};
constexpr OpenMode Create{.read = false, .write = true, .create = true};

TEST(SaveDataPath, KeepsTheComponentsOfATextPath) {
    EXPECT_EQ(SaveDataPath(AsciiPath, Ascii("/data/save.bin")), "data/save.bin");
    EXPECT_EQ(SaveDataPath(Utf16Path, Utf16(u"/data/save.bin")), "data/save.bin");
    EXPECT_EQ(SaveDataPath(AsciiPath, Ascii("/")), "") << "the root";
}

TEST(SaveDataPath, DropsEmptyComponentsAndFollowsParents) {
    EXPECT_EQ(SaveDataPath(AsciiPath, Ascii("//a/./b//")), "a/b");
    EXPECT_EQ(SaveDataPath(AsciiPath, Ascii("/a/b/../c")), "a/c");
}

TEST(SaveDataPath, RefusesWhatCannotNameAFile) {
    EXPECT_FALSE(SaveDataPath(AsciiPath, Ascii("relative")).has_value());
    EXPECT_FALSE(SaveDataPath(AsciiPath, Ascii("/..")).has_value()) << "above the root";
    EXPECT_FALSE(SaveDataPath(AsciiPath, Ascii("/a:b")).has_value());
    EXPECT_FALSE(SaveDataPath(BinaryPath, std::vector<std::uint8_t>{1, 2, 3, 4}).has_value());
    EXPECT_FALSE(SaveDataPath(Utf16Path, std::vector<std::uint8_t>{0x00, 0xD8, 0, 0}).has_value())
        << "a lone surrogate";
}

TEST(SaveDataPath, TurnsASurrogatePairIntoOneCodePoint) {
    // U+1F600, which UTF-16 writes as D83D DE00 and UTF-8 as F0 9F 98 80.
    const std::u16string name = {u'/', 0xD83D, 0xDE00};
    EXPECT_EQ(SaveDataPath(Utf16Path, Utf16(name)), "\xF0\x9F\x98\x80");
    EXPECT_EQ(Utf16Of("\xF0\x9F\x98\x80"), (std::u16string{0xD83D, 0xDE00}));
}

TEST(SaveData, IsNotFormattedUntilFormatted) {
    SaveData save;
    EXPECT_FALSE(save.IsFormatted());
    save.Reformat(SaveDataFormat{.total_size = ArchiveSize});
    ASSERT_TRUE(save.IsFormatted());
    EXPECT_EQ(save.Format().value_or(SaveDataFormat{}).total_size, ArchiveSize);
}

TEST(SaveData, FormattingEmptiesIt) {
    SaveData save = Formatted();
    ASSERT_EQ(save.CreateFile("file", 4), kernel::result::Success);
    save.Reformat(SaveDataFormat{});
    EXPECT_TRUE(save.Files().empty());
}

TEST(SaveData, CreatesFilesAndDirectoriesWhereTheParentExists) {
    SaveData save = Formatted();
    EXPECT_EQ(save.CreateFile("missing/file", 4), save_result::PathNotFound);
    ASSERT_EQ(save.CreateDirectory("dir"), kernel::result::Success);
    ASSERT_EQ(save.CreateFile("dir/file", 4), kernel::result::Success);
    EXPECT_EQ(save.SizeOf("dir/file"), 4U);

    EXPECT_EQ(save.CreateFile("dir/file", 4), save_result::FileAlreadyExists);
    EXPECT_EQ(save.CreateDirectory("dir"), save_result::DirectoryAlreadyExists);
    EXPECT_EQ(save.CreateFile("dir/file/inner", 4), save_result::UnexpectedFileOrDirectory);
}

TEST(SaveData, DeletesOnlyFiles) {
    SaveData save = Formatted();
    ASSERT_EQ(save.CreateDirectory("dir"), kernel::result::Success);
    EXPECT_EQ(save.DeleteFile("dir"), save_result::FileNotFound);
    ASSERT_EQ(save.CreateFile("dir/file", 1), kernel::result::Success);
    EXPECT_EQ(save.DeleteFile("dir/file"), kernel::result::Success);
    EXPECT_EQ(save.DeleteFile("dir/file"), save_result::FileNotFound);
}

TEST(SaveData, OpensWithTheRulesOfTheConsole) {
    SaveData save = Formatted();
    EXPECT_EQ(save.Open("file", OpenMode{}), save_result::UnsupportedOpenFlags);
    EXPECT_EQ(save.Open("file", OpenMode{.read = true, .write = false, .create = true}),
              save_result::UnsupportedOpenFlags)
        << "creating needs the write flag";
    EXPECT_EQ(save.Open("file", ReadWrite), save_result::FileNotFound);
    EXPECT_EQ(save.Open("file", Create), kernel::result::Success);
    EXPECT_EQ(save.SizeOf("file"), 0U);
    EXPECT_EQ(save.Open("", ReadWrite), save_result::UnexpectedFileOrDirectory);
}

TEST(SaveData, GrowsAFileWrittenPastItsEnd) {
    SaveData save = Formatted();
    ASSERT_EQ(save.CreateFile("file", 2), kernel::result::Success);
    const std::vector<std::uint8_t> data = {7, 8};
    const SaveData::Written written = save.Write("file", 4, data);
    EXPECT_EQ(written.result, kernel::result::Success);
    EXPECT_EQ(written.count, 2U);

    const std::span<const std::uint8_t> contents = save.Read("file", 0, 0x100);
    EXPECT_EQ(std::vector<std::uint8_t>(contents.begin(), contents.end()),
              (std::vector<std::uint8_t>{0, 0, 0, 0, 7, 8}));
    EXPECT_EQ(save.Read("file", 5, 4).size(), 1U) << "a read stops at the end";
}

TEST(SaveData, ResizesAFile) {
    SaveData save = Formatted();
    ASSERT_EQ(save.CreateFile("file", 8), kernel::result::Success);
    save.Resize("file", 3);
    EXPECT_EQ(save.SizeOf("file"), 3U);
}

TEST(ExtraData, KeepsEveryFileAtTheSizeItWasCreatedWith) {
    SaveData extra = Formatted(ArchiveKind::ExtraData);
    EXPECT_EQ(extra.CreateFile("empty", 0), save_result::UnsupportedOpenFlags);
    ASSERT_EQ(extra.CreateFile("file", 4), kernel::result::Success);

    const std::vector<std::uint8_t> data = {1, 2, 3};
    const SaveData::Written cut = extra.Write("file", 2, data);
    EXPECT_EQ(cut.result, kernel::result::Success);
    EXPECT_EQ(cut.count, 2U) << "the write stops at the end";
    EXPECT_EQ(extra.SizeOf("file"), 4U);

    EXPECT_EQ(extra.Write("file", 5, data).result, save_result::WriteBeyondEnd);
    extra.Resize("file", 16);
    EXPECT_EQ(extra.SizeOf("file"), 4U);
}

TEST(ExtraData, CannotCreateAFileByOpeningIt) {
    SaveData extra = Formatted(ArchiveKind::ExtraData);
    EXPECT_EQ(extra.Open("file", Create), save_result::UnsupportedOpenFlags);
}

TEST(SaveData, ListsTheChildrenOfADirectoryInOrder) {
    SaveData save = Formatted();
    ASSERT_EQ(save.CreateDirectory("b"), kernel::result::Success);
    ASSERT_EQ(save.CreateFile("c", 3), kernel::result::Success);
    ASSERT_EQ(save.CreateFile("a", 1), kernel::result::Success);
    ASSERT_EQ(save.CreateFile("b/inner", 2), kernel::result::Success);

    const SaveData::Listing root = save.List("");
    ASSERT_EQ(root.result, kernel::result::Success);
    ASSERT_EQ(root.entries.size(), 3U) << "a grandchild is not listed";
    EXPECT_EQ(root.entries.at(0).name, "a");
    EXPECT_EQ(root.entries.at(1).name, "b");
    EXPECT_TRUE(root.entries.at(1).is_directory);
    EXPECT_EQ(root.entries.at(2).size, 3U);

    EXPECT_EQ(save.List("b").entries.size(), 1U);
    EXPECT_EQ(save.List("missing").result, save_result::PathNotFound);
    EXPECT_EQ(save.List("a").result, save_result::UnexpectedFileOrDirectory);
}

TEST(SaveData, HandsEveryCommitToWhoeverAsked) {
    SaveData save = Formatted();
    std::size_t files_seen = 0;
    save.OnCommit(
        [&files_seen](const SaveData& committed) { files_seen = committed.Files().size(); });
    ASSERT_EQ(save.CreateFile("file", 1), kernel::result::Success);
    save.Commit();
    EXPECT_EQ(files_seen, 1U);
}

TEST(ExtraDataIdFromPath, ReadsTheThreeWordsOfTheBinaryPath) {
    const std::vector<std::uint8_t> path = {1, 0, 0, 0, 0x34, 0x12, 0, 0, 0, 0, 0, 0};
    const std::optional<ExtraDataId> id = ExtraDataIdFromPath(path);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id.value_or(ExtraDataId{}).media_type, 1U);
    EXPECT_EQ(id.value_or(ExtraDataId{}).low, 0x1234U);
    EXPECT_EQ(id.value_or(ExtraDataId{}).high, 0U);
    EXPECT_FALSE(ExtraDataIdFromPath(std::vector<std::uint8_t>{1, 2, 3}).has_value());
}

}  // namespace
}  // namespace hexlith::domain::services
