#ifndef HEXLITH_PORTS_SCREEN_SINK_H
#define HEXLITH_PORTS_SCREEN_SINK_H

#include "core/result.h"
#include "domain/gpu/screen.h"

#include <cstdint>
#include <string>

namespace hexlith::ports {

// Where what the screens show goes while the game runs: a window, a file, a
// counter in a test. It is handed a picture of each screen once for every
// frame the game draws, from the thread that runs the game.
class ScreenSink {
public:
    ScreenSink() = default;
    virtual ~ScreenSink() = default;

    ScreenSink(const ScreenSink&) = delete;
    ScreenSink& operator=(const ScreenSink&) = delete;
    ScreenSink(ScreenSink&&) = delete;
    ScreenSink& operator=(ScreenSink&&) = delete;

    [[nodiscard]] virtual core::Status Present(std::uint64_t frame,
                                               const domain::gpu::Picture& top,
                                               const domain::gpu::Picture& bottom) = 0;

    // Whether whoever watches asked to stop, by closing the window. Polled
    // by the run, which then stops where it is.
    [[nodiscard]] virtual bool StopRequested() const = 0;

    [[nodiscard]] virtual std::string Describe() const = 0;
};

}  // namespace hexlith::ports

#endif  // HEXLITH_PORTS_SCREEN_SINK_H
