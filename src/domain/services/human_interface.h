#ifndef HEXLITH_DOMAIN_SERVICES_HUMAN_INTERFACE_H
#define HEXLITH_DOMAIN_SERVICES_HUMAN_INTERFACE_H

#include "domain/kernel/kernel.h"
#include "domain/services/command_table.h"
#include "domain/services/input_state.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace hexlith::domain::services {

// hid:USER, the buttons, the touch screen, the circle pad and the motion
// sensors. The game reads their state from a shared page the service fills in
// and waits on events the service signals when it does. Commands: 3dbrew, HID
// Services; answers: Module::Interface in Azahar,
// src/core/hle/service/hid/hid.cpp.
//
// The page is filled 234 times a second on the virtual clock, as Azahar fills
// it, with the pad and the touch screen as the input function says they are
// at that moment, and both of the events that say so are signalled each
// time. Until something supplies input nothing is held. The motion sensors
// are not filled: nothing on the way to the title screen enables them.
class HumanInterface final : public CommandTable<HumanInterface> {
public:
    explicit HumanInterface(kernel::Kernel& kernel);

    [[nodiscard]] std::string_view Name() const override {
        return "hid:USER";
    }

    static std::span<const Command> Commands();

    // The page the state is written to, a page of the BASE region that the
    // game maps read-only.
    static constexpr std::uint32_t SharedPageSize = 0x1000;

    // Ticks between two updates of the pad and the touch screen. Source:
    // pad_update_ticks in Azahar's hid.h, 234 updates a second.
    static constexpr std::uint64_t PadUpdateTicks = kernel::Arm11TicksPerSecond / 234;

    // Where the input comes from, asked once per update.
    void SetInput(std::function<InputState()> input) {
        input_ = std::move(input);
    }

    [[nodiscard]] const std::shared_ptr<kernel::SharedMemory>& SharedPage() const {
        return shared_page_;
    }

private:
    void GetIpcHandles(kernel::IpcRequest& request);
    void EnableAccelerometer(kernel::IpcRequest& request);
    void DisableAccelerometer(kernel::IpcRequest& request);
    void EnableGyroscope(kernel::IpcRequest& request);
    void DisableGyroscope(kernel::IpcRequest& request);
    void GetGyroscopeCoefficient(kernel::IpcRequest& request);
    void GetGyroscopeCalibration(kernel::IpcRequest& request);
    void GetSoundVolume(kernel::IpcRequest& request);

    void ScheduleUpdate();
    void Update();
    void UpdatePad(const InputState& state);
    void UpdateTouch(const InputState& state);
    void SetWord(std::uint32_t offset, std::uint32_t value);
    void SetDoubleWord(std::uint32_t offset, std::uint64_t value);
    [[nodiscard]] std::uint32_t Word(std::uint32_t offset) const;
    [[nodiscard]] std::uint64_t DoubleWord(std::uint32_t offset) const;

    // Never null; the kernel outlives its services.
    kernel::Kernel* kernel_;
    std::function<InputState()> input_;
    std::uint32_t next_pad_index_ = 0;
    std::uint32_t next_touch_index_ = 0;

    std::shared_ptr<kernel::SharedMemory> shared_page_;
    std::shared_ptr<kernel::Event> pad_ =
        std::make_shared<kernel::Event>(kernel::ResetType::OneShot);
    std::shared_ptr<kernel::Event> touch_ =
        std::make_shared<kernel::Event>(kernel::ResetType::OneShot);
    std::shared_ptr<kernel::Event> accelerometer_ =
        std::make_shared<kernel::Event>(kernel::ResetType::OneShot);
    std::shared_ptr<kernel::Event> gyroscope_ =
        std::make_shared<kernel::Event>(kernel::ResetType::OneShot);
    std::shared_ptr<kernel::Event> debug_pad_ =
        std::make_shared<kernel::Event>(kernel::ResetType::OneShot);
    std::uint32_t accelerometer_users_ = 0;
    std::uint32_t gyroscope_users_ = 0;
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_HUMAN_INTERFACE_H
