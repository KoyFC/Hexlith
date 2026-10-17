#include "domain/services/apt.h"

#include "domain/services/stand_in_font.h"

#include <algorithm>
#include <array>

namespace hexlith::domain::services {
namespace {

// MemoryPermission::Read, what the game may do with the shared font.
constexpr std::uint32_t ReadOnly = 1;

// AppletUtility's command 6, which Azahar answers with true in the first byte
// of the output; the others it answers with zeros.
constexpr std::uint32_t UtilityCommandWithTrue = 0x6;

}  // namespace

AppletManager::AppletManager(kernel::Kernel& kernel) : kernel_(&kernel) {}

std::span<const AppletManager::Command> AppletManager::Commands() {
    static constexpr std::array<Command, 16> Table = {{
        {.id = 0x0001, .name = "GetLockHandle", .handler = &AppletManager::GetLockHandle},
        {.id = 0x0002, .name = "Initialize", .handler = &AppletManager::Initialize},
        {.id = 0x0003, .name = "Enable", .handler = &AppletManager::Enable},
        {.id = 0x0004, .name = "Finalize"},
        {.id = 0x0005, .name = "GetAppletManInfo"},
        {.id = 0x0006, .name = "GetAppletInfo"},
        {.id = 0x0009, .name = "IsRegistered"},
        {.id = 0x000B,
         .name = "InquireNotification",
         .handler = &AppletManager::InquireNotification},
        {.id = 0x000C, .name = "SendParameter"},
        {.id = 0x000D, .name = "ReceiveParameter", .handler = &AppletManager::ReceiveParameter},
        {.id = 0x000E, .name = "GlanceParameter", .handler = &AppletManager::GlanceParameter},
        {.id = 0x000F, .name = "CancelParameter"},
        {.id = 0x0043, .name = "NotifyToWait", .handler = &AppletManager::NotifyToWait},
        {.id = 0x0044, .name = "GetSharedFont", .handler = &AppletManager::GetSharedFont},
        {.id = 0x004B, .name = "AppletUtility", .handler = &AppletManager::AppletUtility},
        {.id = 0x004F, .name = "SetAppCpuTimeLimit"},
    }};
    return Table;
}

void AppletManager::GetLockHandle(kernel::IpcRequest& request) {
    // Bits 0 to 2 of the attributes say what kind of applet asks; an
    // application's are left as they are.
    const std::uint32_t attributes = request.Pop();
    request.Begin(3, 2);
    request.Push(kernel::result::Success);
    request.Push(attributes);
    // The applet manager's state, which nothing here moves from zero.
    request.Push(0);
    const std::array<std::shared_ptr<kernel::Object>, 1> objects = {lock_};
    request.PushCopyObjects(objects);
}

void AppletManager::Initialize(kernel::IpcRequest& request) {
    const std::uint32_t applet = request.Pop();
    attributes_ = request.Pop();
    registered_ = true;

    // The first applet to register is the active one and is woken at once.
    Parameter wakeup;
    wakeup.destination = applet;
    wakeup.signal = WakeupSignal;
    next_parameter_ = wakeup;
    kernel_->Signal(*parameter_event_);

    request.Begin(1, 3);
    request.Push(kernel::result::Success);
    const std::array<std::shared_ptr<kernel::Object>, 2> objects = {notification_,
                                                                    parameter_event_};
    request.PushCopyObjects(objects);
}

void AppletManager::Enable(kernel::IpcRequest& request) {
    attributes_ |= request.Pop();
    registered_ = true;
    request.ReplyWith(kernel::result::Success);
}

void AppletManager::InquireNotification(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop());
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    // No notification: nothing here sends any.
    request.Push(0);
}

void AppletManager::AnswerParameter(kernel::IpcRequest& request, bool consume) {
    const std::uint32_t applet = request.Pop();
    const std::uint32_t buffer_size = request.Pop();
    if (!next_parameter_.has_value() || next_parameter_->destination != applet) {
        request.ReplyWith(NoParameter);
        return;
    }
    Parameter parameter = *next_parameter_;
    if (consume) {
        next_parameter_.reset();
    }
    const auto size = std::min(static_cast<std::uint32_t>(parameter.buffer.size()), buffer_size);
    // APT always sends a buffer of the size asked for.
    parameter.buffer.resize(buffer_size);

    request.Begin(4, 4);
    request.Push(kernel::result::Success);
    request.Push(parameter.sender);
    request.Push(parameter.signal);
    request.Push(size);
    const std::array<std::shared_ptr<kernel::Object>, 1> no_object = {nullptr};
    request.PushMoveObjects(no_object);
    request.PushStaticBuffer(parameter.buffer, 0);
}

void AppletManager::ReceiveParameter(kernel::IpcRequest& request) {
    AnswerParameter(request, true);
}

void AppletManager::GlanceParameter(kernel::IpcRequest& request) {
    AnswerParameter(request, false);
}

void AppletManager::NotifyToWait(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop());
    request.ReplyWith(kernel::result::Success);
}

void AppletManager::GetSharedFont(kernel::IpcRequest& request) {
    // The console's shared font is a system title, not part of the cartridge.
    // The game stops without one, so it gets a stand-in built from the format
    // alone; ADR 0014 has why. The block comes from the system region, which
    // puts it at 0x18000000 as on the console.
    if (shared_font_ == nullptr) {
        shared_font_ = kernel_->CreateSystemMemoryBlock(
            SharedFontBlockSize, ReadOnly, kernel::Kernel::Region::System);
        WriteStandInFont(shared_font_->pages.front().backing->Bytes(),
                         shared_font_->linear_address);
    }
    request.Begin(2, 2);
    request.Push(kernel::result::Success);
    request.Push(shared_font_->linear_address);
    const std::array<std::shared_ptr<kernel::Object>, 1> objects = {shared_font_};
    request.PushCopyObjects(objects);
}

void AppletManager::AppletUtility(kernel::IpcRequest& request) {
    const std::uint32_t command = request.Pop();
    static_cast<void>(request.Pop());
    const std::uint32_t output_size = request.Pop();
    static_cast<void>(request.PopStaticBuffer());

    std::vector<std::uint8_t> output(output_size, 0);
    if (command == UtilityCommandWithTrue && !output.empty()) {
        output.front() = 1;
    }
    request.Begin(2, 2);
    request.Push(kernel::result::Success);
    // The utility's own result.
    request.Push(kernel::result::Success);
    request.PushStaticBuffer(output, 0);
}

}  // namespace hexlith::domain::services
