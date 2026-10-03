#include "adapters/filesystem/file_rom_source.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::adapters::filesystem {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t SampleSize = 0x4000;

// A prime, so the filler never lines up with a power-of-two boundary.
constexpr std::size_t PatternPeriod = 251;

class TemporaryFile {
public:
    explicit TemporaryFile(const std::vector<std::uint8_t>& contents) {
        static int counter = 0;
        directory_ = fs::temp_directory_path() / ("hexlith-rom-" + std::to_string(::getpid()) +
                                                  "-" + std::to_string(++counter));
        fs::create_directories(directory_);
        path_ = directory_ / "image.3ds";

        std::ofstream file(path_, std::ios::binary);
        file.write(reinterpret_cast<const char*>(contents.data()),
                   static_cast<std::streamsize>(contents.size()));
    }

    ~TemporaryFile() {
        std::error_code ignored;
        fs::remove_all(directory_, ignored);
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    TemporaryFile(TemporaryFile&&) = delete;
    TemporaryFile& operator=(TemporaryFile&&) = delete;

    [[nodiscard]] const fs::path& Path() const {
        return path_;
    }

    [[nodiscard]] const fs::path& Directory() const {
        return directory_;
    }

private:
    fs::path directory_;
    fs::path path_;
};

std::vector<std::uint8_t> Sample() {
    std::vector<std::uint8_t> bytes(SampleSize);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(index % PatternPeriod);
    }
    return bytes;
}

TEST(FileRomSource, ReadsTheWholeFile) {
    const std::vector<std::uint8_t> contents = Sample();
    const TemporaryFile file{contents};

    FileRomSource source{file.Path()};
    const core::Result<core::ByteSpan> read = source.Contents();
    ASSERT_TRUE(read.Ok()) << read.Failure().Message();
    EXPECT_EQ(read.Value().size(), contents.size());
    EXPECT_TRUE(std::equal(contents.begin(), contents.end(), read.Value().begin()));
}

TEST(FileRomSource, ReadsOnlyOnce) {
    const TemporaryFile file{Sample()};
    FileRomSource source{file.Path()};

    const core::Result<core::ByteSpan> first = source.Contents();
    ASSERT_TRUE(first.Ok()) << first.Failure().Message();
    const core::Result<core::ByteSpan> second = source.Contents();
    ASSERT_TRUE(second.Ok()) << second.Failure().Message();
    EXPECT_EQ(first.Value().data(), second.Value().data());
}

// ADR 0008 refuses a link so that a path cannot be redirected after it was
// checked.
TEST(FileRomSource, RefusesASymbolicLink) {
    const TemporaryFile file{Sample()};
    const fs::path link = file.Directory() / "link.3ds";

    std::error_code failure;
    fs::create_symlink(file.Path(), link, failure);
    if (failure) {
        GTEST_SKIP() << "this filesystem does not support symbolic links";
    }

    FileRomSource source{link};
    const core::Result<core::ByteSpan> read = source.Contents();
    ASSERT_FALSE(read.Ok());
    EXPECT_NE(read.Failure().Message().find("symbolic link"), std::string::npos)
        << read.Failure().Message();
}

TEST(FileRomSource, ReportsAMissingFile) {
    FileRomSource source{fs::temp_directory_path() / "hexlith-does-not-exist.3ds"};
    const core::Result<core::ByteSpan> read = source.Contents();
    ASSERT_FALSE(read.Ok());
    EXPECT_NE(read.Failure().Message().find("is not a file"), std::string::npos);
}

TEST(FileRomSource, ReportsADirectory) {
    const TemporaryFile file{Sample()};
    FileRomSource source{file.Directory()};
    const core::Result<core::ByteSpan> read = source.Contents();
    ASSERT_FALSE(read.Ok());
    EXPECT_NE(read.Failure().Message().find("is not a file"), std::string::npos);
}

TEST(FileRomSource, ReportsAnEmptyFile) {
    const TemporaryFile file{std::vector<std::uint8_t>{}};
    FileRomSource source{file.Path()};
    const core::Result<core::ByteSpan> read = source.Contents();
    ASSERT_FALSE(read.Ok());
    EXPECT_NE(read.Failure().Message().find("is empty"), std::string::npos);
}

TEST(FileRomSource, DescribesItsPath) {
    const TemporaryFile file{Sample()};
    const FileRomSource source{file.Path()};
    EXPECT_EQ(source.Describe(), file.Path().string());
}

}  // namespace
}  // namespace hexlith::adapters::filesystem
