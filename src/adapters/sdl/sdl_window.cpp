#include "adapters/sdl/sdl_window.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <optional>
#include <span>

namespace hexlith::adapters::sdl {

namespace {

constexpr int TopWidth = 400;
constexpr int BottomWidth = 320;
constexpr int ScreenHeight = 240;

core::Error SdlError(const std::string& doing) {
    return core::Error{std::format("{}: {}", doing, SDL_GetError())};
}

namespace button = domain::services::button;

// The keys that stand for the console's buttons, laid out so that the face
// buttons sit under the left hand's fingers as they do on the console: B and A
// on Z and X, Y and X above them on A and S.
struct KeyButton {
    SDL_Scancode key;
    std::uint32_t button;
};

constexpr std::array<KeyButton, 12> KeyButtons = {{
    {.key = SDL_SCANCODE_X, .button = button::A},
    {.key = SDL_SCANCODE_Z, .button = button::B},
    {.key = SDL_SCANCODE_S, .button = button::X},
    {.key = SDL_SCANCODE_A, .button = button::Y},
    {.key = SDL_SCANCODE_Q, .button = button::L},
    {.key = SDL_SCANCODE_W, .button = button::R},
    {.key = SDL_SCANCODE_RETURN, .button = button::Start},
    {.key = SDL_SCANCODE_BACKSPACE, .button = button::Select},
    {.key = SDL_SCANCODE_UP, .button = button::Up},
    {.key = SDL_SCANCODE_DOWN, .button = button::Down},
    {.key = SDL_SCANCODE_LEFT, .button = button::Left},
    {.key = SDL_SCANCODE_RIGHT, .button = button::Right},
}};

// The circle pad, pushed to its rim by I, K, J and L. The rim is the
// calibrated value InputState gives.
constexpr std::int16_t CircleRim = 0x9A;

struct KeyCircle {
    SDL_Scancode key;
    std::int16_t x;
    std::int16_t y;
};

constexpr std::array<KeyCircle, 4> KeyCircles = {{
    {.key = SDL_SCANCODE_I, .x = 0, .y = CircleRim},
    {.key = SDL_SCANCODE_K, .x = 0, .y = -CircleRim},
    {.key = SDL_SCANCODE_J, .x = -CircleRim, .y = 0},
    {.key = SDL_SCANCODE_L, .x = CircleRim, .y = 0},
}};

}  // namespace

struct SdlWindow::State {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    bool stop = false;
    std::string driver;
    // The keys held, by scan code, and whether the left mouse button is.
    std::array<bool, SDL_SCANCODE_COUNT> held{};
    bool touching = false;
    std::optional<domain::services::TouchPoint> touch;
};

SdlWindow::SdlWindow(std::unique_ptr<State> state) : state_(std::move(state)) {}

SdlWindow::~SdlWindow() {
    if (state_->texture != nullptr) {
        SDL_DestroyTexture(state_->texture);
    }
    if (state_->renderer != nullptr) {
        SDL_DestroyRenderer(state_->renderer);
    }
    if (state_->window != nullptr) {
        SDL_DestroyWindow(state_->window);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

core::Result<std::unique_ptr<SdlWindow>> SdlWindow::Create(const std::string& title, int scale) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return SdlError("SDL could not start its video system");
    }
    auto state = std::make_unique<State>();
    // From here the destructor undoes what was done, so a failure only returns.
    std::unique_ptr<SdlWindow> window{new SdlWindow(std::move(state))};
    State& made = *window->state_;
    const int factor = std::max(scale, 1);
    made.window = SDL_CreateWindow(
        title.c_str(), LogicalWidth * factor, LogicalHeight * factor, SDL_WINDOW_RESIZABLE);
    if (made.window == nullptr) {
        return SdlError("a window could not be made");
    }
    made.renderer = SDL_CreateRenderer(made.window, nullptr);
    if (made.renderer == nullptr) {
        return SdlError("a renderer could not be made");
    }
    SDL_SetRenderLogicalPresentation(
        made.renderer, LogicalWidth, LogicalHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    made.texture = SDL_CreateTexture(made.renderer,
                                     SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_STREAMING,
                                     LogicalWidth,
                                     LogicalHeight);
    if (made.texture == nullptr) {
        return SdlError("a texture could not be made");
    }
    SDL_SetTextureScaleMode(made.texture, SDL_SCALEMODE_NEAREST);
    const char* driver = SDL_GetCurrentVideoDriver();
    made.driver = driver != nullptr ? driver : "no video driver";
    return window;
}

core::Status SdlWindow::Present(std::uint64_t /*frame*/,
                                const domain::gpu::Picture& top,
                                const domain::gpu::Picture& bottom) {
    const State& shown = *state_;
    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(shown.texture, nullptr, &pixels, &pitch)) {
        return SdlError("the texture could not be locked");
    }
    const std::span<std::uint8_t> bytes{static_cast<std::uint8_t*>(pixels),
                                        static_cast<std::size_t>(pitch) * LogicalHeight};
    constexpr std::size_t PixelSize = sizeof(domain::gpu::Color);
    const auto row_of = [bytes, pitch](int row) {
        return bytes.subspan(static_cast<std::size_t>(row) * static_cast<std::size_t>(pitch));
    };
    // The bottom screen is narrower, so the texture is cleared around it.
    for (int row = 0; row < LogicalHeight; ++row) {
        std::ranges::fill(row_of(row).first(static_cast<std::size_t>(LogicalWidth) * PixelSize), 0);
    }
    const auto copy = [&row_of](const domain::gpu::Picture& picture, int left, int top_row) {
        const auto width = static_cast<std::size_t>(picture.width);
        for (std::size_t row = 0;
             row < picture.height && picture.pixels.size() >= width * (row + 1);
             ++row) {
            const std::span<std::uint8_t> target =
                row_of(top_row + static_cast<int>(row))
                    .subspan(static_cast<std::size_t>(left) * PixelSize, width * PixelSize);
            const std::span<const domain::gpu::Color> source =
                std::span{picture.pixels}.subspan(row * width, width);
            std::memcpy(target.data(), source.data(), target.size());
        }
    };
    copy(top, 0, 0);
    copy(bottom, (TopWidth - BottomWidth) / 2, ScreenHeight);
    SDL_UnlockTexture(shown.texture);

    SDL_RenderClear(shown.renderer);
    SDL_RenderTexture(shown.renderer, shown.texture, nullptr, nullptr);
    SDL_RenderPresent(shown.renderer);

    TakeEvents();
    return {};
}

namespace {

// Where a point of the window falls on the bottom screen, in the window's
// logical coordinates, if it does.
std::optional<domain::services::TouchPoint> OnBottomScreen(float x, float y) {
    constexpr float Left = static_cast<float>(TopWidth - BottomWidth) / 2;
    constexpr auto Top = static_cast<float>(ScreenHeight);
    const float across = x - Left;
    const float down = y - Top;
    if (across < 0 || down < 0 || across >= static_cast<float>(BottomWidth) ||
        down >= static_cast<float>(ScreenHeight)) {
        return std::nullopt;
    }
    return domain::services::TouchPoint{.x = static_cast<std::uint16_t>(across),
                                        .y = static_cast<std::uint16_t>(down)};
}

}  // namespace

void SdlWindow::TakeEvents() {
    State& shown = *state_;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            shown.stop = true;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (event.key.scancode < SDL_SCANCODE_COUNT) {
                shown.held.at(event.key.scancode) = event.type == SDL_EVENT_KEY_DOWN;
            }
            break;
        // A key let go of while the window is not looking would otherwise
        // stay held.
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            shown.held.fill(false);
            shown.touching = false;
            shown.touch.reset();
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_MOTION:
            SDL_ConvertEventToRenderCoordinates(shown.renderer, &event);
            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                if (shown.touching) {
                    shown.touch = OnBottomScreen(event.motion.x, event.motion.y);
                }
            } else if (event.button.button == SDL_BUTTON_LEFT) {
                shown.touching = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                shown.touch =
                    shown.touching ? OnBottomScreen(event.button.x, event.button.y) : std::nullopt;
            }
            break;
        default:
            break;
        }
    }
}

domain::services::InputState SdlWindow::StateAt(std::uint64_t /*frame*/) {
    TakeEvents();
    const State& shown = *state_;
    domain::services::InputState state;
    for (const KeyButton& key : KeyButtons) {
        if (shown.held.at(key.key)) {
            state.buttons |= key.button;
        }
    }
    for (const KeyCircle& key : KeyCircles) {
        if (shown.held.at(key.key)) {
            state.circle_x = static_cast<std::int16_t>(state.circle_x + key.x);
            state.circle_y = static_cast<std::int16_t>(state.circle_y + key.y);
        }
    }
    state.touch = shown.touch;
    return state;
}

bool SdlWindow::StopRequested() const {
    return state_->stop;
}

std::string SdlWindow::Describe() const {
    return std::format("an SDL window ({})", state_->driver);
}

}  // namespace hexlith::adapters::sdl
