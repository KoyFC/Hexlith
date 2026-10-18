#include "adapters/filesystem/gpu_recording_directory.h"
#include "adapters/filesystem/gpu_recording_files.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include <unistd.h>

namespace hexlith::adapters::filesystem {
namespace {

namespace fs = std::filesystem;

TEST(GpuRecordingFiles, ReadsBackWhatTheRecorderWrote) {
    const fs::path root =
        fs::temp_directory_path() / ("hexlith-gpu-files-test-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const std::vector<std::uint8_t> page(0x1000, 0x5A);
    const std::vector<std::uint8_t> list = {1, 2, 3, 4};
    {
        GpuRecordingDirectory recording{root};
        ASSERT_TRUE(recording.Prepare().Ok());
        ASSERT_TRUE(recording.Page(0, 0x14000000, page).Ok());
        ASSERT_TRUE(recording.List(0, 0x14000000, list).Ok());
        ASSERT_TRUE(recording.Finish().Ok());
    }

    GpuRecordingFiles files{root};
    const core::Result<std::string> index = files.Index();
    ASSERT_TRUE(index.Ok());
    EXPECT_TRUE(index.Value().starts_with("# hexlith gpu recording, format 2\n"));
    const std::string digest = index.Value().substr(index.Value().find("page 14000000 ") + 14, 64);
    const core::Result<std::vector<std::uint8_t>> read_page = files.Page(digest);
    ASSERT_TRUE(read_page.Ok());
    EXPECT_EQ(read_page.Value(), page);
    const core::Result<std::vector<std::uint8_t>> read_list = files.List("000000.bin");
    ASSERT_TRUE(read_list.Ok());
    EXPECT_EQ(read_list.Value(), list);
    EXPECT_FALSE(files.List("../recording.txt").Ok()) << "a name stays in its directory";
    fs::remove_all(root);
}

}  // namespace
}  // namespace hexlith::adapters::filesystem
