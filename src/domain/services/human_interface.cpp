#include "domain/services/human_interface.h"

#include "core/byte_reader.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <numbers>

namespace hexlith::domain::services {
namespace {

// MemoryPermission::Read, what the game may do with the shared page.
constexpr std::uint32_t ReadOnly = 1;

// Degrees per second per raw unit of the gyroscope, and the calibration of each
// of its three axes as a zero point and the raw readings at plus and minus one
// unit. Azahar's values, which it notes are measured on hardware.
constexpr float GyroscopeCoefficient = 14.375F;
constexpr std::int16_t GyroscopeUnit = 6700;
constexpr std::size_t GyroscopeAxes = 3;

// The volume slider at its top, out of 0x3F.
constexpr std::uint32_t FullVolume = 0x3F;

// The layout of the shared page. Source: SharedMem in Azahar's hid.h, which
// checks the pad at word 0 and the touch screen at word 0x2A; 3dbrew, HID
// Shared Memory.
constexpr std::size_t RingEntries = 8;

constexpr std::uint32_t PadResetTicks = 0x00;
constexpr std::uint32_t PadPreviousResetTicks = 0x08;
constexpr std::uint32_t PadIndex = 0x10;
constexpr std::uint32_t PadCurrentState = 0x1C;
constexpr std::uint32_t PadEntries = 0x28;
constexpr std::uint32_t PadEntrySize = 0x10;

constexpr std::uint32_t TouchResetTicks = 0xA8;
constexpr std::uint32_t TouchPreviousResetTicks = 0xB0;
constexpr std::uint32_t TouchIndex = 0xB8;
constexpr std::uint32_t TouchEntries = 0xC8;
constexpr std::uint32_t TouchEntrySize = 0x08;

// Within a pad entry: the state, the buttons newly pressed, the buttons newly
// released, and the circle pad's two coordinates as halfwords.
constexpr std::uint32_t EntryAdditions = 0x4;
constexpr std::uint32_t EntryRemovals = 0x8;
constexpr std::uint32_t EntryCircle = 0xC;

// Within a touch entry: x and y as halfwords, then whether the entry holds a
// touch at all.
constexpr std::uint32_t TouchValid = 0x4;

// The circle pad reads as a direction once it is more than 40 units from the
// centre, within 60 degrees of an axis. Source: GetStickDirectionState in
// Azahar's hid.cpp.
constexpr int CircleThreshold = 40;
// tan 30 degrees, which Azahar writes out as 0.577350269.
constexpr float Tan30 = std::numbers::inv_sqrt3_v<float>;
constexpr float Tan60 = 1.0F / Tan30;

std::uint32_t CircleDirections(std::int16_t x, std::int16_t y) {
    if ((x * x) + (y * y) <= CircleThreshold * CircleThreshold) {
        return 0;
    }
    std::uint32_t directions = 0;
    const float slope = std::abs(static_cast<float>(y) / static_cast<float>(x));
    if (x != 0 && slope < Tan60) {
        directions |= x > 0 ? button::CircleRight : button::CircleLeft;
    }
    if (x == 0 || slope > Tan30) {
        directions |= y > 0 ? button::CircleUp : button::CircleDown;
    }
    return directions;
}

std::uint32_t PackHalfwords(std::uint16_t low, std::uint16_t high) {
    return static_cast<std::uint32_t>(low) | (static_cast<std::uint32_t>(high) << 16U);
}

}  // namespace

HumanInterface::HumanInterface(kernel::Kernel& kernel)
    : kernel_(&kernel), shared_page_(kernel.CreateSystemMemoryBlock(SharedPageSize, ReadOnly)) {
    ScheduleUpdate();
}

void HumanInterface::ScheduleUpdate() {
    kernel::Scheduler& scheduler = kernel_->Threads();
    scheduler.ScheduleAt(scheduler.Ticks() + PadUpdateTicks, [this] {
        Update();
        ScheduleUpdate();
    });
}

void HumanInterface::Update() {
    const InputState state = input_ ? input_() : InputState{};
    UpdatePad(state);
    UpdateTouch(state);
    // Both of the events GetIPCHandles hands out for the pad and the touch
    // screen, which Azahar signals together.
    kernel_->Signal(*pad_);
    kernel_->Signal(*touch_);
}

// A ring of eight entries, each with the state and what changed since the
// one before. The tick count is stamped each time the ring comes back round
// to its first entry.
void HumanInterface::UpdatePad(const InputState& state) {
    const std::uint32_t buttons = state.buttons | CircleDirections(state.circle_x, state.circle_y);
    const std::uint32_t index = next_pad_index_;
    next_pad_index_ = (next_pad_index_ + 1) % RingEntries;

    const std::uint32_t previous_index = (index + RingEntries - 1) % RingEntries;
    const std::uint32_t previous = Word(PadEntries + (previous_index * PadEntrySize));
    const std::uint32_t changed = buttons ^ previous;
    const std::uint32_t entry = PadEntries + (index * PadEntrySize);

    SetWord(PadCurrentState, buttons);
    SetWord(PadIndex, index);
    SetWord(entry, buttons);
    SetWord(entry + EntryAdditions, changed & buttons);
    SetWord(entry + EntryRemovals, changed & previous);
    SetWord(entry + EntryCircle,
            PackHalfwords(static_cast<std::uint16_t>(state.circle_x),
                          static_cast<std::uint16_t>(state.circle_y)));
    if (index == 0) {
        SetDoubleWord(PadPreviousResetTicks, DoubleWord(PadResetTicks));
        SetDoubleWord(PadResetTicks, kernel_->Threads().Ticks());
    }
}

void HumanInterface::UpdateTouch(const InputState& state) {
    const std::uint32_t index = next_touch_index_;
    next_touch_index_ = (next_touch_index_ + 1) % RingEntries;

    const TouchPoint point = state.touch.value_or(TouchPoint{});
    const std::uint32_t entry = TouchEntries + (index * TouchEntrySize);
    SetWord(TouchIndex, index);
    SetWord(entry, PackHalfwords(point.x, point.y));
    SetWord(entry + TouchValid, state.touch.has_value() ? 1 : 0);
    if (index == 0) {
        SetDoubleWord(TouchPreviousResetTicks, DoubleWord(TouchResetTicks));
        SetDoubleWord(TouchResetTicks, kernel_->Threads().Ticks());
    }
}

std::uint32_t HumanInterface::Word(std::uint32_t offset) const {
    const std::span<const std::uint8_t> bytes = shared_page_->pages.front().backing->Bytes();
    return core::ReadU32At(bytes, offset).ValueOr(0);
}

std::uint64_t HumanInterface::DoubleWord(std::uint32_t offset) const {
    return Word(offset) | (static_cast<std::uint64_t>(Word(offset + 4)) << 32U);
}

void HumanInterface::SetWord(std::uint32_t offset, std::uint32_t value) {
    const std::span<std::uint8_t> bytes = shared_page_->pages.front().backing->Bytes();
    for (std::uint32_t byte = 0; byte < 4; ++byte) {
        bytes[offset + byte] = static_cast<std::uint8_t>(value >> (8U * byte));
    }
}

void HumanInterface::SetDoubleWord(std::uint32_t offset, std::uint64_t value) {
    SetWord(offset, static_cast<std::uint32_t>(value));
    SetWord(offset + 4, static_cast<std::uint32_t>(value >> 32U));
}

std::span<const HumanInterface::Command> HumanInterface::Commands() {
    static constexpr std::array<Command, 12> Table = {{
        {.id = 0x0001, .name = "CalibrateTouchScreen"},
        {.id = 0x0002, .name = "UpdateTouchConfig"},
        {.id = 0x000A, .name = "GetIPCHandles", .handler = &HumanInterface::GetIpcHandles},
        {.id = 0x000B, .name = "StartAnalogStickCalibration"},
        {.id = 0x000E, .name = "GetAnalogStickCalibrateParam"},
        {.id = 0x0011,
         .name = "EnableAccelerometer",
         .handler = &HumanInterface::EnableAccelerometer},
        {.id = 0x0012,
         .name = "DisableAccelerometer",
         .handler = &HumanInterface::DisableAccelerometer},
        {.id = 0x0013, .name = "EnableGyroscopeLow", .handler = &HumanInterface::EnableGyroscope},
        {.id = 0x0014, .name = "DisableGyroscopeLow", .handler = &HumanInterface::DisableGyroscope},
        {.id = 0x0015,
         .name = "GetGyroscopeLowRawToDpsCoefficient",
         .handler = &HumanInterface::GetGyroscopeCoefficient},
        {.id = 0x0016,
         .name = "GetGyroscopeLowCalibrateParam",
         .handler = &HumanInterface::GetGyroscopeCalibration},
        {.id = 0x0017, .name = "GetSoundVolume", .handler = &HumanInterface::GetSoundVolume},
    }};
    return Table;
}

void HumanInterface::GetIpcHandles(kernel::IpcRequest& request) {
    const std::array<std::shared_ptr<kernel::Object>, 6> objects = {
        shared_page_, pad_, touch_, accelerometer_, gyroscope_, debug_pad_};
    // A descriptor, then the handles.
    request.Begin(1, 1 + static_cast<std::uint32_t>(objects.size()));
    request.Push(kernel::result::Success);
    request.PushCopyObjects(objects);
}

void HumanInterface::EnableAccelerometer(kernel::IpcRequest& request) {
    ++accelerometer_users_;
    request.ReplyWith(kernel::result::Success);
}

void HumanInterface::DisableAccelerometer(kernel::IpcRequest& request) {
    if (accelerometer_users_ > 0) {
        --accelerometer_users_;
    }
    request.ReplyWith(kernel::result::Success);
}

void HumanInterface::EnableGyroscope(kernel::IpcRequest& request) {
    ++gyroscope_users_;
    request.ReplyWith(kernel::result::Success);
}

void HumanInterface::DisableGyroscope(kernel::IpcRequest& request) {
    if (gyroscope_users_ > 0) {
        --gyroscope_users_;
    }
    request.ReplyWith(kernel::result::Success);
}

void HumanInterface::GetGyroscopeCoefficient(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(std::bit_cast<std::uint32_t>(GyroscopeCoefficient));
}

void HumanInterface::GetGyroscopeCalibration(kernel::IpcRequest& request) {
    // Three axes of three signed halfwords each, packed into five words.
    std::array<std::uint16_t, (GyroscopeAxes * 3) + 1> halfwords{};
    for (std::size_t axis = 0; axis < GyroscopeAxes; ++axis) {
        halfwords.at((axis * 3) + 1) = static_cast<std::uint16_t>(GyroscopeUnit);
        halfwords.at((axis * 3) + 2) = static_cast<std::uint16_t>(-GyroscopeUnit);
    }
    request.Begin(1 + static_cast<std::uint32_t>(halfwords.size() / 2), 0);
    request.Push(kernel::result::Success);
    for (std::size_t word = 0; word < halfwords.size() / 2; ++word) {
        request.Push(static_cast<std::uint32_t>(halfwords.at(word * 2)) |
                     (static_cast<std::uint32_t>(halfwords.at((word * 2) + 1)) << 16U));
    }
}

void HumanInterface::GetSoundVolume(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(FullVolume);
}

}  // namespace hexlith::domain::services
