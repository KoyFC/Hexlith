#include "domain/services/service_manager.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace hexlith::domain::services {
namespace {

using kernel::IpcRequest;
namespace ipc = kernel::ipc;

constexpr std::uint32_t Page = kernel::layout::PageSize;
constexpr std::uint32_t RegionSize = 64 * Page;

// srv: command headers. Source: 3dbrew, SRV.
constexpr std::uint32_t RegisterClient = 0x00010002;
constexpr std::uint32_t EnableNotification = 0x00020000;
constexpr std::uint32_t GetServiceHandle = 0x00050100;
constexpr std::uint32_t ReceiveNotification = 0x000B0000;

// "APT:U" and "fs:USER" as the two little-endian words GetServiceHandle takes.
constexpr std::uint32_t AptLow = 0x3A545041;
constexpr std::uint32_t AptHigh = 0x00000055;
constexpr std::uint32_t AptLength = 5;
constexpr std::uint32_t FsLow = 0x553A7366;
constexpr std::uint32_t FsHigh = 0x00524553;
constexpr std::uint32_t FsLength = 7;
constexpr std::uint32_t TooLong = 9;

struct Harness {
    Harness()
        : kernel(kernel::KernelConfig{
              .regions = {.application = RegionSize, .system = RegionSize, .base = RegionSize}}),
          registry(std::vector<std::string>{"APT:U"}),
          manager(registry) {
        kernel.Start(kernel::MainThreadStart{.entry_point = kernel::layout::ImageBase,
                                             .stack_top = kernel::layout::HeapEnd,
                                             .priority = kernel::LowestThreadPriority});
        static_cast<void>(kernel.Threads().Reschedule());
    }

    kernel::Thread& Caller() {
        return *kernel.Threads().Threads().front();
    }

    // Sends a request to srv: and answers whether it was implemented.
    bool Send(const std::vector<std::uint32_t>& words) {
        const std::uint32_t base = Caller().thread_local_storage + ipc::CommandBufferOffset;
        for (std::size_t index = 0; index < words.size(); ++index) {
            EXPECT_TRUE(kernel.Space().Memory().Write32(
                base + static_cast<std::uint32_t>(index * 4), words.at(index)));
        }
        IpcRequest request(kernel, Caller());
        const bool handled = manager.Handle(request);
        if (handled) {
            request.Complete();
        }
        return handled;
    }

    std::uint32_t Reply(std::size_t index) {
        const std::uint32_t base = Caller().thread_local_storage + ipc::CommandBufferOffset;
        return kernel.Space()
            .Memory()
            .Read32(base + static_cast<std::uint32_t>(index * 4))
            .value_or(0);
    }

    kernel::Kernel kernel;
    ServiceRegistry registry;
    ServiceManager manager;
};

TEST(ServiceManager, RegistersAClientThatSendsItsProcessId) {
    Harness harness;
    ASSERT_TRUE(harness.Send({RegisterClient, ipc::CallingProcessIdDescriptor, 0}));
    EXPECT_EQ(harness.Reply(0), ipc::MakeHeader(1, 1, 0));
    EXPECT_EQ(harness.Reply(1), kernel::result::Success.raw);
    EXPECT_TRUE(harness.manager.IsRegistered());
}

TEST(ServiceManager, RefusesAClientWithoutItsProcessId) {
    Harness harness;
    ASSERT_TRUE(harness.Send({RegisterClient, 0, 0}));
    EXPECT_EQ(harness.Reply(1), ipc::InvalidBufferDescriptor.raw);
    EXPECT_FALSE(harness.manager.IsRegistered());
}

TEST(ServiceManager, HandsOutANotificationSemaphore) {
    Harness harness;
    ASSERT_TRUE(harness.Send({EnableNotification}));
    EXPECT_EQ(harness.Reply(0), ipc::MakeHeader(2, 1, 2));
    EXPECT_EQ(harness.Reply(2), ipc::CopyHandlesDescriptor(1));
    EXPECT_NE(harness.kernel.Handles().Get<kernel::Semaphore>(harness.Reply(3),
                                                              kernel::ObjectType::Semaphore),
              nullptr);
}

TEST(ServiceManager, OpensAServiceTheTitleIsAllowed) {
    Harness harness;
    ASSERT_TRUE(harness.Send({GetServiceHandle, AptLow, AptHigh, AptLength, 0}));
    EXPECT_EQ(harness.Reply(0), ipc::MakeHeader(5, 1, 2));
    EXPECT_EQ(harness.Reply(1), kernel::result::Success.raw);
    EXPECT_EQ(harness.Reply(2), ipc::MoveHandlesDescriptor(1));
    const auto session = harness.kernel.Handles().Get<kernel::ClientSession>(
        harness.Reply(3), kernel::ObjectType::ClientSession);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->handler->Name(), "APT:U");
}

TEST(ServiceManager, RefusesAServiceTheTitleIsNotAllowed) {
    Harness harness;
    ASSERT_TRUE(harness.Send({GetServiceHandle, FsLow, FsHigh, FsLength, 0}));
    EXPECT_EQ(harness.Reply(1), ServiceManager::AccessDenied.raw);
}

TEST(ServiceManager, RefusesANameLongerThanEightCharacters) {
    Harness harness;
    ASSERT_TRUE(harness.Send({GetServiceHandle, AptLow, AptHigh, TooLong, 0}));
    EXPECT_EQ(harness.Reply(1), ServiceManager::InvalidNameSize.raw);
}

TEST(ServiceManager, DoesNotAnswerACommandItDoesNotImplement) {
    Harness harness;
    EXPECT_FALSE(harness.Send({ReceiveNotification}));
    EXPECT_EQ(harness.manager.CommandName(ipc::CommandOf(ReceiveNotification)),
              "ReceiveNotification");
}

TEST(ServiceManager, RemembersSubscriptions) {
    // The notification the reference title subscribes to, from the Azahar log.
    constexpr std::uint32_t Notification = 0x209;
    constexpr std::uint32_t Subscribe = 0x00090040;
    constexpr std::uint32_t Unsubscribe = 0x000A0040;
    Harness harness;
    ASSERT_TRUE(harness.Send({Subscribe, Notification}));
    EXPECT_EQ(harness.Reply(1), kernel::result::Success.raw);
    EXPECT_TRUE(harness.manager.Subscriptions().contains(Notification));
    ASSERT_TRUE(harness.Send({Unsubscribe, Notification}));
    EXPECT_TRUE(harness.manager.Subscriptions().empty());
}

TEST(ServiceRegistry, StartsEveryAllowedServiceUnimplemented) {
    const ServiceRegistry registry(std::vector<std::string>{"APT:U", "fs:USER"});
    EXPECT_TRUE(registry.IsAllowed("fs:USER"));
    EXPECT_FALSE(registry.IsAllowed("ptm:sysm"));
    EXPECT_EQ(registry.Names(), (std::vector<std::string>{"APT:U", "fs:USER"}));
    EXPECT_EQ(registry.Find("APT:U")->Name(), "APT:U");
    EXPECT_EQ(registry.Find("ptm:sysm"), nullptr);
}

// A service that answers every command with success.
class Answering final : public kernel::SessionHandler {
public:
    explicit Answering(std::string name) : name_(std::move(name)) {}

    [[nodiscard]] std::string_view Name() const override {
        return name_;
    }

    bool Handle(kernel::IpcRequest& request) override {
        request.ReplyWith(kernel::result::Success);
        return true;
    }

private:
    std::string name_;
};

TEST(ServiceRegistry, ReplacesAServiceWithItsImplementationOnlyIfAllowed) {
    ServiceRegistry registry(std::vector<std::string>{"APT:U"});
    const auto apt = std::make_shared<Answering>("APT:U");
    registry.Provide(apt);
    registry.Provide(std::make_shared<Answering>("ptm:sysm"));
    EXPECT_EQ(registry.Find("APT:U"), apt);
    EXPECT_FALSE(registry.IsAllowed("ptm:sysm"));
}

}  // namespace
}  // namespace hexlith::domain::services
