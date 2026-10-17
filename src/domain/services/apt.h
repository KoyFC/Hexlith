#ifndef HEXLITH_DOMAIN_SERVICES_APT_H
#define HEXLITH_DOMAIN_SERVICES_APT_H

#include "domain/kernel/kernel.h"
#include "domain/services/command_table.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace hexlith::domain::services {

// APT:U, the applet manager as an application sees it: the lock every applet
// takes before it touches the manager, the events that tell the application a
// parameter or a notification is waiting, and the parameters themselves.
// Commands: 3dbrew, NS and APT Services; answers: Module::APTInterface and
// AppletManager in Azahar, src/core/hle/service/apt/.
//
// Only the application's own slot is modelled. Nothing else runs: no Home Menu
// and no applet, so the application is the first applet to register, and APT
// sends it the Wakeup parameter itself, as Azahar's AppletManager::Initialize
// does when nothing is active.
class AppletManager final : public CommandTable<AppletManager> {
public:
    explicit AppletManager(kernel::Kernel& kernel);

    [[nodiscard]] std::string_view Name() const override {
        return "APT:U";
    }

    static std::span<const Command> Commands();

    // The applet ID of an application, and the signal that wakes one up.
    // Source: AppletId and SignalType in Azahar's applet_manager.h.
    static constexpr std::uint32_t ApplicationId = 0x300;
    static constexpr std::uint32_t WakeupSignal = 0x1;

    // Answered when no parameter is waiting for the applet that asks.
    static constexpr kernel::ResultCode NoParameter{0xC8A0CFEF};

private:
    // A message from one applet to another. Source: MessageParameter in Azahar.
    struct Parameter {
        std::uint32_t sender = 0;
        std::uint32_t destination = 0;
        std::uint32_t signal = 0;
        std::vector<std::uint8_t> buffer;
    };

    void GetLockHandle(kernel::IpcRequest& request);
    void Initialize(kernel::IpcRequest& request);
    void Enable(kernel::IpcRequest& request);
    void InquireNotification(kernel::IpcRequest& request);
    void ReceiveParameter(kernel::IpcRequest& request);
    void GlanceParameter(kernel::IpcRequest& request);
    void NotifyToWait(kernel::IpcRequest& request);
    void AppletUtility(kernel::IpcRequest& request);
    void GetSharedFont(kernel::IpcRequest& request);

    void AnswerParameter(kernel::IpcRequest& request, bool consume);

    // Never null; the kernel outlives its services.
    kernel::Kernel* kernel_;
    std::shared_ptr<kernel::Mutex> lock_ = std::make_shared<kernel::Mutex>();
    std::shared_ptr<kernel::Event> notification_ =
        std::make_shared<kernel::Event>(kernel::ResetType::OneShot);
    std::shared_ptr<kernel::Event> parameter_event_ =
        std::make_shared<kernel::Event>(kernel::ResetType::OneShot);
    bool registered_ = false;
    std::uint32_t attributes_ = 0;
    std::optional<Parameter> next_parameter_;
    std::shared_ptr<kernel::SharedMemory> shared_font_;
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_APT_H
