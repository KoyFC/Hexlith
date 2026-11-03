#include "adapters/filesystem/png_file.h"
#include "adapters/filesystem/png_frame_sink.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <vector>

namespace hexlith::adapters::filesystem {
namespace {

namespace gpu = domain::gpu;

// What the sink in front of the window would have been handed.
class CountingSink final : public ports::ScreenSink {
public:
    core::Status Present(std::uint64_t frame,
                         const gpu::Picture& /*top*/,
                         const gpu::Picture& /*bottom*/) override {
        frames.push_back(frame);
        return {};
    }

    [[nodiscard]] bool StopRequested() const override {
        return stop;
    }

    [[nodiscard]] std::string Describe() const override {
        return "counting";
    }

    std::vector<std::uint64_t> frames;
    bool stop = false;
};

gpu::Picture OnePixel(std::uint8_t red) {
    gpu::Picture picture{.width = 1, .height = 1, .pixels = {}};
    gpu::Color color;
    color.red = red;
    picture.pixels.push_back(color);
    return picture;
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

class PngFrameSinkTest : public testing::Test {
protected:
    void SetUp() override {
        directory = std::filesystem::temp_directory_path() /
                    std::format("hexlith-png-frame-sink-{}",
                                testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
    }

    void TearDown() override {
        std::filesystem::remove_all(directory);
    }

    std::filesystem::path directory;
};

// A frame that had no event of its own is not handed to the sink, and the
// picture of the next one that does stands for it too (ADR 0027): a call is
// given the first frame it is a picture of, and covers the ones up to the
// frame before the next call.
TEST_F(PngFrameSinkTest, SavesThePictureThatStandsForAFrameThatWasNotHandedOn) {
    CountingSink inner;
    PngFrameSink sink{directory, {3, 6}, &inner};
    ASSERT_TRUE(sink.Present(1, OnePixel(10), OnePixel(110)));
    ASSERT_TRUE(sink.Present(2, OnePixel(20), OnePixel(120)));
    ASSERT_TRUE(sink.Present(5, OnePixel(50), OnePixel(150)));
    // Frame 3 is the call for frame 2, which stood for frames 2 to 4, and it
    // can only be written once the next call shows where those end.
    EXPECT_EQ(ReadFile(directory / "frame-3-top.png"), EncodePng(OnePixel(20)));
    EXPECT_EQ(ReadFile(directory / "frame-3-bottom.png"), EncodePng(OnePixel(120)));
    EXPECT_FALSE(std::filesystem::exists(directory / "frame-6-top.png"));
    ASSERT_TRUE(sink.Present(7, OnePixel(70), OnePixel(170)));
    EXPECT_EQ(ReadFile(directory / "frame-6-top.png"), EncodePng(OnePixel(50)));
    EXPECT_FALSE(std::filesystem::exists(directory / "frame-1-top.png"));
    EXPECT_FALSE(std::filesystem::exists(directory / "frame-2-top.png"));
}

TEST_F(PngFrameSinkTest, SavesAFrameThatWasHandedOnAsItself) {
    CountingSink inner;
    PngFrameSink sink{directory, {2}, &inner};
    ASSERT_TRUE(sink.Present(1, OnePixel(10), OnePixel(110)));
    ASSERT_TRUE(sink.Present(2, OnePixel(20), OnePixel(120)));
    ASSERT_TRUE(sink.Present(3, OnePixel(30), OnePixel(130)));
    EXPECT_EQ(ReadFile(directory / "frame-2-top.png"), EncodePng(OnePixel(20)));
}

TEST_F(PngFrameSinkTest, PassesEveryFrameAndTheRequestToStopOnToTheSinkInFrontOfIt) {
    CountingSink inner;
    PngFrameSink sink{directory, {2}, &inner};
    for (std::uint64_t frame = 1; frame <= 3; ++frame) {
        ASSERT_TRUE(sink.Present(frame, OnePixel(1), OnePixel(1)));
    }
    EXPECT_EQ(inner.frames, (std::vector<std::uint64_t>{1, 2, 3}));
    EXPECT_FALSE(sink.StopRequested());
    inner.stop = true;
    EXPECT_TRUE(sink.StopRequested());
}

TEST_F(PngFrameSinkTest, ReportsAFrameItCouldNotWriteAndStillPassesItOn) {
    CountingSink inner;
    PngFrameSink sink{directory / "missing" / "deeper", {1}, &inner};
    ASSERT_TRUE(sink.Present(1, OnePixel(1), OnePixel(1)));
    EXPECT_FALSE(sink.Present(2, OnePixel(1), OnePixel(1)));
    EXPECT_EQ(inner.frames, (std::vector<std::uint64_t>{1, 2}));
}

TEST_F(PngFrameSinkTest, WorksWithNothingInFrontOfIt) {
    PngFrameSink sink{directory, {1}, nullptr};
    EXPECT_TRUE(sink.Present(1, OnePixel(9), OnePixel(9)));
    EXPECT_TRUE(sink.Present(2, OnePixel(9), OnePixel(9)));
    EXPECT_FALSE(sink.StopRequested());
    EXPECT_TRUE(std::filesystem::exists(directory / "frame-1-top.png"));
}

}  // namespace
}  // namespace hexlith::adapters::filesystem
