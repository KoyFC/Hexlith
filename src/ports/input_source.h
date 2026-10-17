#ifndef HEXLITH_PORTS_INPUT_SOURCE_H
#define HEXLITH_PORTS_INPUT_SOURCE_H

#include "domain/services/input_state.h"

#include <cstdint>
#include <string>

namespace hexlith::ports {

// Where the player's input comes from: a script, a controller, a recording.
//
// It is asked by frame, the frame being the count of the top screen's
// vertical blanks since the process started, and not by tick or by wall
// clock. A source that answers the same for the same frame makes a run
// repeatable, and a script is written in frames because that is the unit a
// person reading a capture of the game counts in.
class InputSource {
public:
    InputSource() = default;
    virtual ~InputSource() = default;

    InputSource(const InputSource&) = delete;
    InputSource& operator=(const InputSource&) = delete;
    InputSource(InputSource&&) = delete;
    InputSource& operator=(InputSource&&) = delete;

    [[nodiscard]] virtual domain::services::InputState StateAt(std::uint64_t frame) = 0;

    // Where this came from, for a message a person has to act on.
    [[nodiscard]] virtual std::string Describe() const = 0;
};

}  // namespace hexlith::ports

#endif  // HEXLITH_PORTS_INPUT_SOURCE_H
