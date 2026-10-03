#include "adapters/filesystem/directory_blob_sink.h"

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

// A directory of its own per test, removed afterwards whether the test passed
// or not. Nothing here touches the workspace: ADR 0008 keeps the tests away
// from it, which is also what lets the suite run on a machine with no
// cartridge.
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("hexlith-test-" + std::to_string(::getpid()) + "-" + std::to_string(++counter));
        fs::remove_all(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

    [[nodiscard]] const fs::path& Path() const {
        return path_;
    }

private:
    fs::path path_;
};

std::vector<std::uint8_t> ReadBack(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

core::ByteSpan Bytes(const std::vector<std::uint8_t>& data) {
    return core::ByteSpan{data};
}

TEST(DirectoryBlobSink, CreatesItsRoot) {
    const TemporaryDirectory temporary;
    DirectoryBlobSink sink{temporary.Path() / "extraction"};

    ASSERT_TRUE(sink.Prepare().Ok());
    EXPECT_TRUE(fs::is_directory(temporary.Path() / "extraction"));
}

TEST(DirectoryBlobSink, WritesAFileAndTheDirectoriesAboveIt) {
    const TemporaryDirectory temporary;
    DirectoryBlobSink sink{temporary.Path()};
    ASSERT_TRUE(sink.Prepare().Ok());

    const std::vector<std::uint8_t> data = {1, 2, 3, 4};
    const core::Status written = sink.Write("romfs/a/0/0", Bytes(data));
    ASSERT_TRUE(written.Ok()) << written.Failure().Message();

    EXPECT_EQ(ReadBack(temporary.Path() / "romfs/a/0/0"), data);
}

TEST(DirectoryBlobSink, WritesAnEmptyFile) {
    const TemporaryDirectory temporary;
    DirectoryBlobSink sink{temporary.Path()};
    ASSERT_TRUE(sink.Prepare().Ok());

    ASSERT_TRUE(sink.Write("empty", core::ByteSpan{}).Ok());
    EXPECT_TRUE(fs::is_regular_file(temporary.Path() / "empty"));
    EXPECT_EQ(fs::file_size(temporary.Path() / "empty"), 0U);
}

TEST(DirectoryBlobSink, WritesText) {
    const TemporaryDirectory temporary;
    DirectoryBlobSink sink{temporary.Path()};
    ASSERT_TRUE(sink.Prepare().Ok());

    ASSERT_TRUE(sink.WriteText("manifest.txt", "manifest-version value=1\n").Ok());

    std::ifstream file(temporary.Path() / "manifest.txt");
    std::string line;
    std::getline(file, line);
    EXPECT_EQ(line, "manifest-version value=1");
}

// Two runs write into the same directory, so the second has to replace what
// the first left rather than refuse or append.
TEST(DirectoryBlobSink, ReplacesAFileWrittenByAnEarlierRun) {
    const TemporaryDirectory temporary;
    DirectoryBlobSink sink{temporary.Path()};
    ASSERT_TRUE(sink.Prepare().Ok());

    const std::vector<std::uint8_t> first = {1, 2, 3, 4, 5, 6};
    const std::vector<std::uint8_t> second = {9, 9};
    ASSERT_TRUE(sink.Write("file", Bytes(first)).Ok());
    ASSERT_TRUE(sink.Write("file", Bytes(second)).Ok());

    EXPECT_EQ(ReadBack(temporary.Path() / "file"), second);
}

TEST(DirectoryBlobSink, MakesADirectoryWithNothingInIt) {
    const TemporaryDirectory temporary;
    DirectoryBlobSink sink{temporary.Path()};
    ASSERT_TRUE(sink.Prepare().Ok());

    ASSERT_TRUE(sink.MakeDirectory("romfs/shaders").Ok());
    EXPECT_TRUE(fs::is_directory(temporary.Path() / "romfs/shaders"));
}

TEST(DirectoryBlobSink, MakingADirectoryTwiceIsNotAFailure) {
    const TemporaryDirectory temporary;
    DirectoryBlobSink sink{temporary.Path()};
    ASSERT_TRUE(sink.Prepare().Ok());

    ASSERT_TRUE(sink.MakeDirectory("romfs").Ok());
    EXPECT_TRUE(sink.MakeDirectory("romfs").Ok());
}

// The RomFS parser already refuses a name that could escape. This is the
// other end of the same check, because a filesystem image is untrusted input
// and this is the last place before a file is created.
TEST(DirectoryBlobSink, RefusesAPathThatWouldReachOutsideTheRoot) {
    const TemporaryDirectory temporary;
    DirectoryBlobSink sink{temporary.Path() / "extraction"};
    ASSERT_TRUE(sink.Prepare().Ok());

    const std::vector<std::uint8_t> data = {1};
    for (const std::string_view path : {"../escaped", "romfs/../../escaped", "./here"}) {
        const core::Status written = sink.Write(path, Bytes(data));
        EXPECT_FALSE(written.Ok()) << path;
        if (!written.Ok()) {
            EXPECT_NE(written.Failure().Message().find("outside the extraction directory"),
                      std::string::npos)
                << written.Failure().Message();
        }
    }

    EXPECT_FALSE(fs::exists(temporary.Path() / "escaped"));
}

TEST(DirectoryBlobSink, RefusesAnAbsolutePath) {
    const TemporaryDirectory temporary;
    DirectoryBlobSink sink{temporary.Path()};
    ASSERT_TRUE(sink.Prepare().Ok());

    const std::vector<std::uint8_t> data = {1};
    const core::Status written = sink.Write("/etc/hexlith", Bytes(data));
    ASSERT_FALSE(written.Ok());
    EXPECT_NE(written.Failure().Message().find("is absolute"), std::string::npos);
}

TEST(DirectoryBlobSink, RefusesAnEmptyPath) {
    const TemporaryDirectory temporary;
    DirectoryBlobSink sink{temporary.Path()};
    ASSERT_TRUE(sink.Prepare().Ok());

    EXPECT_FALSE(sink.Write("", core::ByteSpan{}).Ok());
    EXPECT_FALSE(sink.MakeDirectory("").Ok());
}

TEST(DirectoryBlobSink, DescribesItsRoot) {
    const TemporaryDirectory temporary;
    const DirectoryBlobSink sink{temporary.Path()};
    EXPECT_EQ(sink.Describe(), temporary.Path().string());
}

}  // namespace
}  // namespace hexlith::adapters::filesystem
