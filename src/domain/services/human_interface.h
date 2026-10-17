#ifndef HEXLITH_DOMAIN_SERVICES_HUMAN_INTERFACE_H
#define HEXLITH_DOMAIN_SERVICES_HUMAN_INTERFACE_H

#include "domain/kernel/kernel.h"
#include "domain/services/command_table.h"

#include <cstdint>
#include <memory>
#include <span>

namespace hexlith::domain::services {

// hid:USER, the buttons, the touch screen, the circle pad and the motion
// sensors. The game reads their state from a shared page the service fills in
// and waits on events the service signals when it does. Commands: 3dbrew, HID
// Services; answers: Module::Interface in Azahar,
// src/core/hle/service/hid/hid.cpp.
//
// Nothing fills the page yet: there is no input and no clock driving it. It
// stays zero, which reads as no button held and the stylus up.
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
