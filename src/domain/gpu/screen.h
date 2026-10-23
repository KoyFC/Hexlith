#ifndef HEXLITH_DOMAIN_GPU_SCREEN_H
#define HEXLITH_DOMAIN_GPU_SCREEN_H

#include "domain/gpu/gpu_memory.h"
#include "domain/gpu/pixel_formats.h"
#include "domain/gpu/recording_index.h"

#include <array>
#include <cstdint>
#include <vector>

namespace hexlith::domain::gpu {

// A picture, row by row from the top left.
struct Picture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<Color> pixels;
};

// What the two screens are set to show, and what that looks like. Each
// screen has two framebuffers, so that a game can draw into one while the
// other is shown; a game describes one of them at a time and says which is
// shown. Source: GBATEK, 3DS GPU External Registers - Top/Bottom Screen and
// Framebuffer Setup, for the registers; Azahar's GPU::SetBufferSwap for how
// gsp::Gpu fills them from the game's description.
class ScreenSetup {
public:
    static constexpr std::uint32_t Top = 0;
    static constexpr std::uint32_t Bottom = 1;

    // A framebuffer line of a recording: a game's description of one of a
    // screen's framebuffers.
    void Apply(const RecordedFramebuffer& framebuffer);

    // What a screen shows: 400 by 240 pixels on the top screen, 320 by 240
    // on the bottom, from the left eye's framebuffer. A screen that was never
    // set up, or whose framebuffer is not in GPU memory, is black.
    [[nodiscard]] Picture Compose(const GpuMemory& memory, std::uint32_t screen) const;

private:
    struct Framebuffer {
        // Physical, or zero.
        std::uint32_t left = 0;
    };

    struct Screen {
        std::array<Framebuffer, 2> framebuffers{};
        std::uint32_t stride = 0;
        std::uint32_t format = 0;
        std::uint32_t shown = 0;
    };

    std::array<Screen, 2> screens_{};
};

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_SCREEN_H
