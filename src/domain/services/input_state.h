#ifndef HEXLITH_DOMAIN_SERVICES_INPUT_STATE_H
#define HEXLITH_DOMAIN_SERVICES_INPUT_STATE_H

#include <cstdint>
#include <optional>

namespace hexlith::domain::services {

// What the player is doing at one moment: the buttons held, where the circle
// pad is and where the stylus touches, if it does. hid:USER reads one of
// these each time it fills its shared page.

// One bit per button, in the layout of the pad state the console writes.
// Source: PadState in Azahar's src/core/hle/service/hid/hid.h; 3dbrew, HID
// Shared Memory.
namespace button {

constexpr std::uint32_t A = 1U << 0U;
constexpr std::uint32_t B = 1U << 1U;
constexpr std::uint32_t Select = 1U << 2U;
constexpr std::uint32_t Start = 1U << 3U;
constexpr std::uint32_t Right = 1U << 4U;
constexpr std::uint32_t Left = 1U << 5U;
constexpr std::uint32_t Up = 1U << 6U;
constexpr std::uint32_t Down = 1U << 7U;
constexpr std::uint32_t R = 1U << 8U;
constexpr std::uint32_t L = 1U << 9U;
constexpr std::uint32_t X = 1U << 10U;
constexpr std::uint32_t Y = 1U << 11U;

// The circle pad's directions, which the service sets from its position.
constexpr std::uint32_t CircleRight = 1U << 28U;
constexpr std::uint32_t CircleLeft = 1U << 29U;
constexpr std::uint32_t CircleUp = 1U << 30U;
constexpr std::uint32_t CircleDown = 1U << 31U;

}  // namespace button

// A point on the bottom screen, in pixels from its top left corner.
struct TouchPoint {
    std::uint16_t x = 0;
    std::uint16_t y = 0;

    friend bool operator==(const TouchPoint&, const TouchPoint&) = default;
};

struct InputState {
    std::uint32_t buttons = 0;

    // The circle pad, calibrated: about plus or minus 0x9A at the rim.
    std::int16_t circle_x = 0;
    std::int16_t circle_y = 0;

    std::optional<TouchPoint> touch;

    friend bool operator==(const InputState&, const InputState&) = default;
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_INPUT_STATE_H
