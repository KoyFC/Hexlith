#ifndef HEXLITH_ADAPTERS_SDL_SDL_WINDOW_H
#define HEXLITH_ADAPTERS_SDL_SDL_WINDOW_H

#include "core/result.h"
#include "ports/input_source.h"
#include "ports/screen_sink.h"

#include <memory>
#include <string>

namespace hexlith::adapters::sdl {

// A window showing the two screens one above the other, the way the console
// lays them out: the top screen's 400 by 240 pixels over the bottom's 320 by
// 240, centred. The pictures are copied into a texture as they come and SDL
// scales the whole to the window, keeping the proportions.
//
// Nothing here draws the game: it only shows what the GPU backend already
// drew into memory (ADR 0027).
//
// It is also where the player's input comes from when nothing else gives it:
// the keyboard holds the buttons and the circle pad, and the left mouse button
// on the bottom screen touches it. What is held is what the game reads, whatever
// the frame, so a run played this way does not repeat.
class SdlWindow final : public ports::ScreenSink, public ports::InputSource {
public:
    // The top screen's width is the window's, and the two screens' heights
    // added make its height, before any scaling by the player.
    static constexpr int LogicalWidth = 400;
    static constexpr int LogicalHeight = 480;

    static core::Result<std::unique_ptr<SdlWindow>> Create(const std::string& title, int scale);

    SdlWindow(const SdlWindow&) = delete;
    SdlWindow& operator=(const SdlWindow&) = delete;
    SdlWindow(SdlWindow&&) = delete;
    SdlWindow& operator=(SdlWindow&&) = delete;
    ~SdlWindow() override;

    core::Status Present(std::uint64_t frame,
                         const domain::gpu::Picture& top,
                         const domain::gpu::Picture& bottom) override;

    [[nodiscard]] bool StopRequested() const override;

    [[nodiscard]] domain::services::InputState StateAt(std::uint64_t frame) override;

    [[nodiscard]] std::string Describe() const override;

private:
    struct State;
    void TakeEvents();
    explicit SdlWindow(std::unique_ptr<State> state);

    std::unique_ptr<State> state_;
};

}  // namespace hexlith::adapters::sdl

#endif  // HEXLITH_ADAPTERS_SDL_SDL_WINDOW_H
