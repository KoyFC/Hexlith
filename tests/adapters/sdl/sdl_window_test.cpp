#include "adapters/sdl/sdl_window.h"

#include <SDL3/SDL.h>
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

void Key(SDL_Scancode key, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = key;
    event.key.down = down;
    ASSERT_TRUE(SDL_PushEvent(&event));
}

void LeftButton(float x, float y, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = down;
    event.button.x = x;
    event.button.y = y;
    ASSERT_TRUE(SDL_PushEvent(&event));
}

TEST_F(SdlWindowTest, HoldsTheButtonsWhoseKeysAreDown) {
    auto made = SdlWindow::Create("Hexlith test", Scale);
    if (!made) {
        GTEST_SKIP() << made.Failure().Message();
    }
    SdlWindow& window = *made.Value();
    Key(SDL_SCANCODE_X, true);
    Key(SDL_SCANCODE_RETURN, true);
    Key(SDL_SCANCODE_L, true);
    domain::services::InputState state = window.StateAt(1);
    EXPECT_EQ(state.buttons, domain::services::button::A | domain::services::button::Start);
    EXPECT_GT(state.circle_x, 0);
    EXPECT_EQ(state.circle_y, 0);

    Key(SDL_SCANCODE_X, false);
    state = window.StateAt(2);
    EXPECT_EQ(state.buttons, domain::services::button::Start);
}

// At scale 1 the window's coordinates are its logical ones: the bottom screen
// starts 40 pixels in and 240 down.
constexpr float BottomLeft = 40.0F;
constexpr float BottomTop = 240.0F;
constexpr std::uint16_t Inside = 10;

TEST_F(SdlWindowTest, TouchesTheBottomScreenWhereTheMouseIsPressed) {
    auto made = SdlWindow::Create("Hexlith test", Scale);
    if (!made) {
        GTEST_SKIP() << made.Failure().Message();
    }
    SdlWindow& window = *made.Value();
    const float x = BottomLeft + Inside;
    const float y = BottomTop + Inside;
    LeftButton(x, y, true);
    EXPECT_EQ(window.StateAt(1).touch,
              (std::optional<domain::services::TouchPoint>{{.x = Inside, .y = Inside}}));

    LeftButton(x, y, false);
    EXPECT_FALSE(window.StateAt(2).touch.has_value());

    LeftButton(x, BottomTop - Inside, true);
    EXPECT_FALSE(window.StateAt(3).touch.has_value()) << "the top screen does not touch";
}

}  // namespace
}  // namespace hexlith::adapters::sdl
