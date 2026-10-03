#include "adapters/filesystem/cartridge_image.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

#include <unistd.h>

namespace hexlith::adapters::filesystem {
namespace {

namespace fs = std::filesystem;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("hexlith-images-" + std::to_string(::getpid()) + "-" + std::to_string(++counter));
        fs::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

    void Add(std::string_view name) const {
        std::ofstream file(path_ / name, std::ios::binary);
        file << "not really an image";
    }

    [[nodiscard]] const fs::path& Path() const {
        return path_;
    }

private:
    fs::path path_;
};

TEST(FindCartridgeImage, FindsTheOnlyCandidate) {
    const TemporaryDirectory directory;
    directory.Add("pokemon-x.3ds");
    directory.Add("notes.txt");

    const core::Result<fs::path> found = FindCartridgeImage(directory.Path());
    ASSERT_TRUE(found.Ok()) << found.Failure().Message();
    EXPECT_EQ(found.Value().filename(), "pokemon-x.3ds");
}

TEST(FindCartridgeImage, AcceptsTheCciExtensionAndIgnoresCase) {
    const TemporaryDirectory directory;
    directory.Add("image.CCI");

    const core::Result<fs::path> found = FindCartridgeImage(directory.Path());
    ASSERT_TRUE(found.Ok()) << found.Failure().Message();
    EXPECT_EQ(found.Value().filename(), "image.CCI");
}

// Picking the first of several is how a tool extracts the wrong game and
// says nothing about it.
TEST(FindCartridgeImage, RefusesToChooseBetweenCandidates) {
    const TemporaryDirectory directory;
    directory.Add("pokemon-x.3ds");
    directory.Add("pokemon-y.3ds");

    const core::Result<fs::path> found = FindCartridgeImage(directory.Path());
    ASSERT_FALSE(found.Ok());
    EXPECT_NE(found.Failure().Message().find("--image has to say which"), std::string::npos)
        << found.Failure().Message();
    EXPECT_NE(found.Failure().Message().find("pokemon-y.3ds"), std::string::npos)
        << "the message lists what it found";
}

TEST(FindCartridgeImage, ReportsADirectoryWithNoImageInIt) {
    const TemporaryDirectory directory;
    directory.Add("notes.txt");

    const core::Result<fs::path> found = FindCartridgeImage(directory.Path());
    ASSERT_FALSE(found.Ok());
    EXPECT_NE(found.Failure().Message().find("no .3ds or .cci file"), std::string::npos);
}

TEST(FindCartridgeImage, ReportsSomethingThatIsNotADirectory) {
    const TemporaryDirectory directory;
    directory.Add("pokemon-x.3ds");

    const core::Result<fs::path> found = FindCartridgeImage(directory.Path() / "pokemon-x.3ds");
    ASSERT_FALSE(found.Ok());
    EXPECT_NE(found.Failure().Message().find("is not a directory"), std::string::npos);
}

}  // namespace
}  // namespace hexlith::adapters::filesystem
