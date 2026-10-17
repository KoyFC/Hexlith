#include "domain/services/human_interface.h"

#include <array>
#include <bit>

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

}  // namespace

HumanInterface::HumanInterface(kernel::Kernel& kernel)
    : shared_page_(kernel.CreateSystemMemoryBlock(SharedPageSize, ReadOnly)) {}

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
