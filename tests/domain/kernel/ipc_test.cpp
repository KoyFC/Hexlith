#include "domain/kernel/ipc.h"
#include "domain/kernel/kernel.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace hexlith::domain::kernel {
namespace {

constexpr std::uint32_t Page = layout::PageSize;
constexpr std::uint32_t RegionSize = 64 * Page;
constexpr std::uint32_t BufferAddress = 0x00200000;
constexpr std::uint32_t Command = 0x0042;

// A kernel with a main thread, whose thread-local storage holds the command
// buffer, and a page of data for buffers.
struct Harness {
    Harness()
        : kernel(KernelConfig{
              .regions = {.application = RegionSize, .system = RegionSize, .base = RegionSize}}) {
        kernel.Start(MainThreadStart{.entry_point = layout::ImageBase,
                                     .stack_top = layout::HeapEnd,
                                     .priority = LowestThreadPriority});
        EXPECT_EQ(kernel.Space().CommitFixed(
                      BufferAddress, Page, memory::Access::ReadWrite, MemoryState::Private),
                  result::Success);
        static_cast<void>(kernel.Threads().Reschedule());
    }

    Thread& Caller() {
        return *kernel.Threads().Threads().front();
    }

    // Writes the request into the caller's command buffer.
    void Send(const std::vector<std::uint32_t>& words) {
        const std::uint32_t base = Caller().thread_local_storage + ipc::CommandBufferOffset;
        for (std::size_t index = 0; index < words.size(); ++index) {
            ASSERT_TRUE(kernel.Space().Memory().Write32(
                base + static_cast<std::uint32_t>(index * 4), words.at(index)));
        }
    }

    std::uint32_t Reply(std::size_t index) {
        const std::uint32_t base = Caller().thread_local_storage + ipc::CommandBufferOffset;
        return kernel.Space()
            .Memory()
            .Read32(base + static_cast<std::uint32_t>(index * 4))
            .value_or(0);
    }

    Kernel kernel;
};

TEST(IpcRequest, ReadsNormalWordsInOrder) {
    Harness harness;
    Kernel& kernel = harness.kernel;
    constexpr std::uint32_t First = 0x11111111;
    constexpr std::uint32_t Second = 0x22222222;
    harness.Send({ipc::MakeHeader(Command, 2, 0), First, Second});
    IpcRequest request(kernel, harness.Caller());
    EXPECT_EQ(request.Command(), Command);
    EXPECT_EQ(request.Pop(), First);
    EXPECT_EQ(request.Pop(), Second);
    EXPECT_FALSE(request.Malformed());
    EXPECT_EQ(request.Pop(), 0U) << "past what the header declares";
    EXPECT_TRUE(request.Malformed());
}

TEST(IpcRequest, TranslatesHandlesAndClosesMovedOnes) {
    Harness harness;
    Kernel& kernel = harness.kernel;
    const auto event = std::make_shared<Event>(ResetType::OneShot);
    const Handle copied = kernel.Handles().Create(event).value_or(0);
    const Handle moved = kernel.Handles().Create(event).value_or(0);
    harness.Send({ipc::MakeHeader(Command, 0, 4),
                  ipc::CopyHandlesDescriptor(1),
                  copied,
                  ipc::MoveHandlesDescriptor(1),
                  moved});

    IpcRequest request(kernel, harness.Caller());
    EXPECT_EQ(request.PopHandles().front(), event);
    EXPECT_EQ(request.PopHandles().front(), event);
    EXPECT_NE(kernel.Handles().Get(copied), nullptr);
    EXPECT_EQ(kernel.Handles().Get(moved), nullptr) << "a moved handle leaves the caller";
}

TEST(IpcRequest, ReplacesTheProcessIdPlaceholder) {
    Harness harness;
    Kernel& kernel = harness.kernel;
    harness.Send({ipc::MakeHeader(Command, 0, 2), ipc::CallingProcessIdDescriptor, 0});
    IpcRequest request(kernel, harness.Caller());
    EXPECT_EQ(request.PopProcessId(), kernel.ProcessId());
    EXPECT_FALSE(request.Malformed());
}

TEST(IpcRequest, RefusesADescriptorOfTheWrongKind) {
    Harness harness;
    Kernel& kernel = harness.kernel;
    harness.Send({ipc::MakeHeader(Command, 0, 2), ipc::CopyHandlesDescriptor(1), 0});
    IpcRequest request(kernel, harness.Caller());
    static_cast<void>(request.PopMappedBuffer());
    EXPECT_TRUE(request.Malformed());
}

TEST(IpcRequest, ReadsMappedAndStaticBuffers) {
    Harness harness;
    Kernel& kernel = harness.kernel;
    constexpr std::uint32_t MappedSize = 0x30;
    constexpr std::uint32_t ReadWrite = 3;
    constexpr std::uint8_t Marker = 0x5A;
    ASSERT_TRUE(kernel.Space().Memory().Write8(BufferAddress, Marker));
    harness.Send({ipc::MakeHeader(Command, 0, 4),
                  ipc::MappedBufferDescriptorFor(MappedSize, ReadWrite),
                  BufferAddress,
                  ipc::StaticBufferDescriptorFor(2, 0),
                  BufferAddress});

    IpcRequest request(kernel, harness.Caller());
    const MappedBuffer mapped = request.PopMappedBuffer();
    EXPECT_EQ(mapped.address, BufferAddress);
    EXPECT_EQ(mapped.size, MappedSize);
    EXPECT_EQ(mapped.access, ReadWrite);
    EXPECT_EQ(request.PopStaticBuffer(), (std::vector<std::uint8_t>{Marker, 0}));
}

TEST(IpcRequest, WritesTheReplyWithNewHandles) {
    Harness harness;
    Kernel& kernel = harness.kernel;
    harness.Send({ipc::MakeHeader(Command, 0, 0)});
    IpcRequest request(kernel, harness.Caller());
    const std::array<std::shared_ptr<Object>, 1> objects = {
        std::make_shared<Event>(ResetType::Sticky)};
    request.Begin(1, 2);
    request.Push(result::Success);
    request.PushCopyObjects(objects);
    request.Complete();

    EXPECT_EQ(harness.Reply(0), ipc::MakeHeader(Command, 1, 2));
    EXPECT_EQ(harness.Reply(1), result::Success.raw);
    EXPECT_EQ(harness.Reply(2), ipc::CopyHandlesDescriptor(1));
    EXPECT_EQ(kernel.Handles().Get(harness.Reply(3)), objects.front());
}

TEST(IpcRequest, WritesStaticDataIntoTheBufferTheCallerListed) {
    Harness harness;
    Kernel& kernel = harness.kernel;
    constexpr std::uint32_t Capacity = 4;
    constexpr std::uint32_t Index = 2;
    const std::uint32_t entry = harness.Caller().thread_local_storage +
                                ipc::ReceiveBufferListOffset +
                                (Index * ipc::ReceiveBufferEntrySize);
    ASSERT_TRUE(
        kernel.Space().Memory().Write32(entry, ipc::StaticBufferDescriptorFor(Capacity, Index)));
    ASSERT_TRUE(kernel.Space().Memory().Write32(entry + 4, BufferAddress));
    harness.Send({ipc::MakeHeader(Command, 0, 0)});

    IpcRequest request(kernel, harness.Caller());
    const std::vector<std::uint8_t> data = {1, 2, 3, 4, 5, 6};
    request.Begin(1, 2);
    request.Push(result::Success);
    request.PushStaticBuffer(data, Index);
    request.Complete();

    EXPECT_EQ(kernel.Space().Memory().Read32(BufferAddress), 0x04030201U);
    EXPECT_EQ(kernel.Space().Memory().Read8(BufferAddress + Capacity), 0)
        << "no more than the buffer holds";
    EXPECT_EQ(harness.Reply(2), ipc::StaticBufferDescriptorFor(Capacity, Index));
    EXPECT_EQ(harness.Reply(3), BufferAddress);
}

}  // namespace
}  // namespace hexlith::domain::kernel
