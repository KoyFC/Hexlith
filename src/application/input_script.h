#ifndef HEXLITH_APPLICATION_INPUT_SCRIPT_H
#define HEXLITH_APPLICATION_INPUT_SCRIPT_H

#include "core/result.h"
#include "domain/services/input_state.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace hexlith::application {

// What a player does, written down in frames: a text file of lines such as
// "120 tap a" or "300 touch 160 120 for 6". docs/formats/input-script.md is
// the format. Parsing turns it into the state of the input at every frame,
// which is what hid:USER asks for.
class InputScript {
public:
    static core::Result<InputScript> Parse(std::string_view text);

    // The state at a frame: every line up to and including it applied in
    // order, and every tap or touch that has run its length let go.
    [[nodiscard]] domain::services::InputState StateAt(std::uint64_t frame) const;

    // The last frame at which anything changes.
    [[nodiscard]] std::uint64_t LastFrame() const;

private:
    struct Snapshot {
        std::uint64_t frame = 0;
        domain::services::InputState state;
    };

    explicit InputScript(std::vector<Snapshot> snapshots) : snapshots_(std::move(snapshots)) {}

    // The state from each frame something changes at, in order.
    std::vector<Snapshot> snapshots_;
};

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_INPUT_SCRIPT_H
