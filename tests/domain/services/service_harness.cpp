#include "service_harness.h"

#include <gtest/gtest.h>

namespace hexlith::domain::services::testing {
namespace {

constexpr std::uint32_t RegionSize = 64 * kernel::layout::PageSize;

// Large enough for the shared font, the largest block a service hands out.
constexpr std::uint32_t SystemRegionSize = 0x00400000;

std::uint32_t CommandWordAddress(kernel::Thread& thread, std::size_t index) {
    return thread.thread_local_storage + kernel::ipc::CommandBufferOffset +
           static_cast<std::uint32_t>(index * 4);
}

}  // namespace

ServiceHarness::ServiceHarness()
    : kernel_(kernel::KernelConfig{
          .regions = {.application = RegionSize, .system = SystemRegionSize, .base = RegionSize}}) {
    kernel_.Start(kernel::MainThreadStart{.entry_point = kernel::layout::ImageBase,
                                          .stack_top = kernel::layout::HeapEnd,
                                          .priority = kernel::LowestThreadPriority});
    EXPECT_EQ(
        kernel_.Space().CommitFixed(
            BufferAddress, BufferSize, memory::Access::ReadWrite, kernel::MemoryState::Private),
        kernel::result::Success);
    static_cast<void>(kernel_.Threads().Reschedule());
}

kernel::Thread& ServiceHarness::Caller() {
    return *kernel_.Threads().Threads().front();
}

bool ServiceHarness::Send(kernel::SessionHandler& service,
                          const std::vector<std::uint32_t>& words) {
    for (std::size_t index = 0; index < words.size(); ++index) {
        EXPECT_TRUE(
            kernel_.Space().Memory().Write32(CommandWordAddress(Caller(), index), words.at(index)));
    }
    kernel::IpcRequest request(kernel_, Caller());
    const bool handled = service.Handle(request);
    if (handled) {
        request.Complete();
    }
    return handled;
}

std::uint32_t ServiceHarness::Reply(std::size_t index) {
    return Read32(CommandWordAddress(Caller(), index));
}

void ServiceHarness::ListReceiveBuffer(std::uint32_t index,
                                       std::uint32_t address,
                                       std::uint32_t capacity) {
    const std::uint32_t entry = Caller().thread_local_storage +
                                kernel::ipc::ReceiveBufferListOffset +
                                (index * kernel::ipc::ReceiveBufferEntrySize);
    EXPECT_TRUE(kernel_.Space().Memory().Write32(
        entry, kernel::ipc::StaticBufferDescriptorFor(capacity, index)));
    EXPECT_TRUE(kernel_.Space().Memory().Write32(entry + 4, address));
}

void ServiceHarness::WriteBytes(std::uint32_t address, std::span<const std::uint8_t> bytes) {
    EXPECT_TRUE(kernel_.Space().Memory().Load(address, bytes).Ok());
}

std::vector<std::uint8_t> ServiceHarness::ReadBytes(std::uint32_t address, std::uint32_t size) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(size);
    for (std::uint32_t offset = 0; offset < size; ++offset) {
        bytes.push_back(kernel_.Space().Memory().Read8(address + offset).value_or(0));
    }
    return bytes;
}

std::uint32_t ServiceHarness::Read32(std::uint32_t address) {
    return kernel_.Space().Memory().Read32(address).value_or(0);
}

}  // namespace hexlith::domain::services::testing
