#include "adapters/filesystem/gpu_recording_directory.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <vector>

#include <unistd.h>

namespace hexlith::adapters::filesystem {
namespace {

namespace fs = std::filesystem;

fs::path Root(std::string_view name) {
    return fs::temp_directory_path() /
           ("hexlith-gpu-test-" + std::to_string(::getpid()) + "-" + std::string{name});
}

std::string Read(const fs::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>{file}, {}};
}

// Records a command, the same page content at two addresses, and a list.
fs::path RecordSample(std::string_view name) {
    const fs::path root = Root(name);
    fs::remove_all(root);
    GpuRecordingDirectory recording{root};
    EXPECT_TRUE(recording.Prepare().Ok());
    const std::vector<std::uint8_t> page(16, 0x11);
    const std::vector<std::uint8_t> list = {1, 2, 3, 4};
    EXPECT_TRUE(recording.Command(3, {0x00000001, 0x14000000, 4, 0, 0, 0, 0, 0}).Ok());
    EXPECT_TRUE(recording.Page(3, 0x14000000, page).Ok());
    EXPECT_TRUE(recording.Page(4, 0x14001000, page).Ok());
    EXPECT_TRUE(recording.List(4, 0x14000000, list).Ok());
    EXPECT_TRUE(recording.Finish().Ok());
    return root;
}

TEST(GpuRecordingDirectory, WritesAnIndexOfEveryEvent) {
    const fs::path root = RecordSample("index");
    const std::string index = Read(root / "recording.txt");
    EXPECT_TRUE(index.starts_with("# hexlith gpu recording, format 2\n")) << index;
    EXPECT_NE(index.find("3 gx 00000001 14000000 00000004"), std::string::npos) << index;
    EXPECT_NE(index.find("4 list 14000000 00000004 000000.bin"), std::string::npos) << index;
    fs::remove_all(root);
}

TEST(GpuRecordingDirectory, WritesAFramebufferAsItsWords) {
    const fs::path root = Root("framebuffer");
    fs::remove_all(root);
    GpuRecordingDirectory recording{root};
    EXPECT_TRUE(recording.Prepare().Ok());
    EXPECT_TRUE(recording.Framebuffer(5, 1, {1, 0x14400000, 0x14500000, 0x2D0, 0x201, 1, 0}).Ok());
    EXPECT_TRUE(recording.Finish().Ok());
    EXPECT_NE(Read(root / "recording.txt")
                  .find("5 framebuffer 1 00000001 14400000 14500000 000002D0 00000201 00000001 "
                        "00000000\n"),
              std::string::npos);
    fs::remove_all(root);
}

TEST(GpuRecordingDirectory, KeepsEachPageContentOnce) {
    const fs::path root = RecordSample("pages");
    EXPECT_EQ(std::distance(fs::directory_iterator{root / "pages"}, fs::directory_iterator{}), 1)
        << "two pages with one content are one file";
    EXPECT_EQ(fs::file_size(root / "lists" / "000000.bin"), 4U);
    EXPECT_FALSE(fs::exists(fs::path{root.string() + ".partial"}));
    fs::remove_all(root);
}

void RecordRun(const fs::path& root, bool with_list) {
    GpuRecordingDirectory recording{root};
    EXPECT_TRUE(recording.Prepare().Ok());
    if (with_list) {
        EXPECT_TRUE(recording.List(0, 0, std::vector<std::uint8_t>{1}).Ok());
    }
    EXPECT_TRUE(recording.Finish().Ok());
}

TEST(GpuRecordingDirectory, ReplacesARecordingWhole) {
    const fs::path root = Root("replace");
    fs::remove_all(root);
    RecordRun(root, true);
    RecordRun(root, false);
    EXPECT_FALSE(fs::exists(root / "lists" / "000000.bin")) << "nothing of the first run is left";
    fs::remove_all(root);
}

}  // namespace
}  // namespace hexlith::adapters::filesystem
