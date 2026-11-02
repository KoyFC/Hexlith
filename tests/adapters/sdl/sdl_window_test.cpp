#include "adapters/sdl/sdl_window.h"

#include <cstdlib>
#include <gtest/gtest.h>

namespace hexlith::adapters::sdl {
namespace {

namespace gpu = domain::gpu;

constexpr int Scale = 1;

gpu::Picture Plain(std::uint32_t width, gpu::Color color) {
    constexpr std::uint32_t Height = 240;
    return gpu::Picture{
        .width = width,
        .height = Height,
        .pixels = std::vector<gpu::Color>(static_cast<std::size_t>(width) * Height, color)};
}

// SDL's dummy video driver has a window and a renderer and shows nothing, so
// the tests run where there is no display.
class SdlWindowTest : public testing::Test {
protected:
    static void SetUpTestSuite() {
        setenv("SDL_VIDEODRIVER", "dummy", 1);
    }
};

TEST_F(SdlWindowTest, ShowsBothScreens) {
    auto made = SdlWindow::Create("Hexlith test", Scale);
    if (!made) {
        GTEST_SKIP() << made.Failure().Message();
    }
    const gpu::Color grey{.red = 90, .green = 90, .blue = 90, .alpha = 255};
    const core::Status shown = made.Value()->Present(1, Plain(400, grey), Plain(320, grey));
    EXPECT_TRUE(shown);
    EXPECT_FALSE(made.Value()->StopRequested());
}

TEST_F(SdlWindowTest, AcceptsAPictureThatIsNotWholeWithoutReadingPastIt) {
    auto made = SdlWindow::Create("Hexlith test", Scale);
    if (!made) {
        GTEST_SKIP() << made.Failure().Message();
    }
    EXPECT_TRUE(made.Value()->Present(1, gpu::Picture{}, gpu::Picture{}));
}

}  // namespace
}  // namespace hexlith::adapters::sdl
