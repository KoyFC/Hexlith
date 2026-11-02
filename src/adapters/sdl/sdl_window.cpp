#include "adapters/sdl/sdl_window.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <span>

namespace hexlith::adapters::sdl {

namespace {

constexpr int TopWidth = 400;
constexpr int BottomWidth = 320;
constexpr int ScreenHeight = 240;

core::Error SdlError(const std::string& doing) {
    return core::Error{std::format("{}: {}", doing, SDL_GetError())};
}

}  // namespace

struct SdlWindow::State {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    bool stop = false;
    std::string driver;
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
    State& shown = *state_;
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

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            shown.stop = true;
        }
    }
    return {};
}

bool SdlWindow::StopRequested() const {
    return state_->stop;
}

std::string SdlWindow::Describe() const {
    return std::format("an SDL window ({})", state_->driver);
}

}  // namespace hexlith::adapters::sdl
