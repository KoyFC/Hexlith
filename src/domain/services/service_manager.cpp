#include "domain/services/service_manager.h"

#include <algorithm>
#include <array>
#include <string>

namespace hexlith::domain::services {
namespace {

// A service name is at most eight characters, passed as two words.
constexpr std::uint32_t ServiceNameSize = 8;
constexpr unsigned BitsPerByte = 8;
constexpr std::uint32_t ByteMask = 0xFF;

// How many notifications srv: queues for a process before it drops them.
// Source: MAX_PENDING_NOTIFICATIONS in Azahar's srv.cpp.
constexpr std::int32_t PendingNotifications = 0x10;

}  // namespace

ServiceManager::ServiceManager(const ServiceRegistry& registry) : registry_(&registry) {}

std::span<const ServiceManager::Command> ServiceManager::Commands() {
    static constexpr std::array<Command, 14> Table = {{
        {.id = 0x0001, .name = "RegisterClient", .handler = &ServiceManager::RegisterClient},
        {.id = 0x0002,
         .name = "EnableNotification",
         .handler = &ServiceManager::EnableNotification},
        {.id = 0x0003, .name = "RegisterService"},
        {.id = 0x0004, .name = "UnregisterService"},
        {.id = 0x0005, .name = "GetServiceHandle", .handler = &ServiceManager::GetServiceHandle},
        {.id = 0x0006, .name = "RegisterPort"},
        {.id = 0x0007, .name = "UnregisterPort"},
        {.id = 0x0008, .name = "GetPort"},
        {.id = 0x0009, .name = "Subscribe", .handler = &ServiceManager::Subscribe},
        {.id = 0x000A, .name = "Unsubscribe", .handler = &ServiceManager::Unsubscribe},
        {.id = 0x000B, .name = "ReceiveNotification"},
        {.id = 0x000C, .name = "PublishToSubscriber"},
        {.id = 0x000D, .name = "PublishAndGetSubscriber"},
        {.id = 0x000E, .name = "IsServiceRegistered"},
    }};
    return Table;
}

void ServiceManager::RegisterClient(kernel::IpcRequest& request) {
    static_cast<void>(request.PopProcessId());
    if (request.Malformed()) {
        request.ReplyWith(kernel::ipc::InvalidBufferDescriptor);
        return;
    }
    registered_ = true;
    request.ReplyWith(kernel::result::Success);
}

void ServiceManager::EnableNotification(kernel::IpcRequest& request) {
    // A semaphore srv: releases once for each notification it queues.
    notifications_ = std::make_shared<kernel::Semaphore>(0, PendingNotifications);
    request.Begin(1, 2);
    request.Push(kernel::result::Success);
    const std::array<std::shared_ptr<kernel::Object>, 1> objects = {notifications_};
    request.PushCopyObjects(objects);
}

void ServiceManager::GetServiceHandle(kernel::IpcRequest& request) {
    const std::array<std::uint32_t, 2> words = {request.Pop(), request.Pop()};
    const std::uint32_t length = request.Pop();
    // Bit 0 of the flags says not to wait for a service that is not registered
    // yet; every service here is registered from the start.
    static_cast<void>(request.Pop());

    if (length > ServiceNameSize) {
        request.ReplyWith(InvalidNameSize);
        return;
    }
    std::string name;
    for (std::uint32_t index = 0; index < length; ++index) {
        const std::uint32_t word = words.at(index / 4);
        name.push_back(static_cast<char>((word >> (BitsPerByte * (index % 4))) & ByteMask));
    }

    const std::shared_ptr<kernel::SessionHandler> service = registry_->Find(name);
    if (service == nullptr) {
        request.ReplyWith(AccessDenied);
        return;
    }
    request.Begin(1, 2);
    request.Push(kernel::result::Success);
    const std::array<std::shared_ptr<kernel::Object>, 1> session = {
        std::make_shared<kernel::ClientSession>(service)};
    request.PushMoveObjects(session);
}

void ServiceManager::Subscribe(kernel::IpcRequest& request) {
    // Nothing here publishes notifications yet, so a subscription is only
    // remembered.
    subscriptions_.insert(request.Pop());
    request.ReplyWith(kernel::result::Success);
}

void ServiceManager::Unsubscribe(kernel::IpcRequest& request) {
    subscriptions_.erase(request.Pop());
    request.ReplyWith(kernel::result::Success);
}

}  // namespace hexlith::domain::services
