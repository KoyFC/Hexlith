#include "domain/kernel/ipc.h"

#include "domain/kernel/kernel.h"

#include <algorithm>

namespace hexlith::domain::kernel {
namespace {

constexpr std::uint32_t WordBytes = 4;
constexpr unsigned HighWordShift = 32;

std::uint32_t CommandBufferAddress(const Thread& thread) {
    return thread.thread_local_storage + ipc::CommandBufferOffset;
}

}  // namespace

IpcRequest::IpcRequest(Kernel& kernel, Thread& thread) : kernel_(&kernel), thread_(&thread) {
    const memory::ProcessMemory& memory = kernel_->Space().Memory();
    const std::uint32_t base = CommandBufferAddress(*thread_);
    for (std::size_t index = 0; index < words_.size(); ++index) {
        words_.at(index) =
            memory.Read32(base + (static_cast<std::uint32_t>(index) * WordBytes)).value_or(0);
    }
}

std::uint32_t IpcRequest::Pop() {
    const std::size_t declared = 1 + ipc::NormalCountOf(Header()) + ipc::TranslateCountOf(Header());
    if (read_ >= declared) {
        malformed_ = true;
        return 0;
    }
    const std::uint32_t word = words_.at(read_);
    ++read_;
    return word;
}

std::uint64_t IpcRequest::Pop64() {
    const std::uint64_t low = Pop();
    const std::uint64_t high = Pop();
    return low | (high << HighWordShift);
}

std::vector<std::shared_ptr<Object>> IpcRequest::PopHandles() {
    const std::uint32_t descriptor = Pop();
    if ((descriptor & ipc::DescriptorTypeMask) != ipc::HandleDescriptor ||
        (descriptor & ipc::CallingProcessIdFlag) != 0) {
        malformed_ = true;
        return {};
    }
    const bool move = (descriptor & ipc::MoveHandlesFlag) != 0;
    const std::uint32_t count = (descriptor >> ipc::HandleCountShift) + 1;
    std::vector<std::shared_ptr<Object>> objects;
    for (std::uint32_t index = 0; index < count; ++index) {
        const Handle handle = Pop();
        std::shared_ptr<Object> object = handle == CurrentThreadHandle
                                             ? kernel_->Threads().CurrentShared()
                                             : kernel_->Handles().Get(handle);
        if (move && object != nullptr) {
            kernel_->Handles().Close(handle);
        }
        objects.push_back(std::move(object));
    }
    return objects;
}

std::uint32_t IpcRequest::PopProcessId() {
    if (Pop() != ipc::CallingProcessIdDescriptor) {
        malformed_ = true;
        return 0;
    }
    // The client's word is a placeholder the kernel overwrites.
    static_cast<void>(Pop());
    return kernel_->ProcessId();
}

MappedBuffer IpcRequest::PopMappedBuffer() {
    const std::uint32_t descriptor = Pop();
    if ((descriptor & ipc::MappedBufferFlag) == 0) {
        malformed_ = true;
        return {};
    }
    MappedBuffer buffer;
    buffer.size = descriptor >> ipc::MappedBufferSizeShift;
    buffer.access = (descriptor >> ipc::MappedBufferAccessShift) & ipc::MappedBufferAccessMask;
    buffer.address = Pop();
    return buffer;
}

std::vector<std::uint8_t> IpcRequest::PopStaticBuffer() {
    const std::uint32_t descriptor = Pop();
    if ((descriptor & ipc::DescriptorTypeMask) != ipc::StaticBufferDescriptor ||
        (descriptor & ipc::MappedBufferFlag) != 0) {
        malformed_ = true;
        return {};
    }
    const std::uint32_t size = descriptor >> ipc::StaticBufferSizeShift;
    const std::uint32_t address = Pop();
    std::vector<std::uint8_t> bytes(size);
    if (!kernel_->Space().Memory().Copy(address, bytes)) {
        malformed_ = true;
        return {};
    }
    return bytes;
}

void IpcRequest::Begin(std::uint32_t normal_words, std::uint32_t translate_words) {
    reply_.fill(0);
    reply_.at(0) = ipc::MakeHeader(Command(), normal_words, translate_words);
    written_ = 1;
}

void IpcRequest::Push(std::uint32_t word) {
    reply_.at(written_) = word;
    ++written_;
}

void IpcRequest::Push64(std::uint64_t value) {
    Push(static_cast<std::uint32_t>(value));
    Push(static_cast<std::uint32_t>(value >> HighWordShift));
}

void IpcRequest::PushHandles(std::span<const std::shared_ptr<Object>> objects, bool move) {
    const auto count = static_cast<std::uint32_t>(objects.size());
    Push(move ? ipc::MoveHandlesDescriptor(count) : ipc::CopyHandlesDescriptor(count));
    for (const std::shared_ptr<Object>& object : objects) {
        // A null object becomes handle zero, which the console sends for a
        // handle a service has none of.
        Push(object == nullptr ? 0 : kernel_->Handles().Create(object).value_or(0));
    }
}

void IpcRequest::PushCopyObjects(std::span<const std::shared_ptr<Object>> objects) {
    PushHandles(objects, false);
}

void IpcRequest::PushMoveObjects(std::span<const std::shared_ptr<Object>> objects) {
    PushHandles(objects, true);
}

void IpcRequest::PushMappedBuffer(const MappedBuffer& buffer) {
    Push(ipc::MappedBufferDescriptorFor(buffer.size, buffer.access));
    Push(buffer.address);
}

void IpcRequest::PushStaticBuffer(std::span<const std::uint8_t> data, std::uint32_t index) {
    // The client lists, for each index, how large a buffer it has and where.
    memory::ProcessMemory& memory = kernel_->Space().Memory();
    const std::uint32_t entry = thread_->thread_local_storage + ipc::ReceiveBufferListOffset +
                                (index * ipc::ReceiveBufferEntrySize);
    const std::uint32_t capacity = memory.Read32(entry).value_or(0) >> ipc::StaticBufferSizeShift;
    const std::uint32_t address = memory.Read32(entry + WordBytes).value_or(0);
    const auto size = std::min(static_cast<std::uint32_t>(data.size()), capacity);
    static_cast<void>(memory.Load(address, data.first(size)));
    Push(ipc::StaticBufferDescriptorFor(size, index));
    Push(address);
}

void IpcRequest::Complete() {
    memory::ProcessMemory& memory = kernel_->Space().Memory();
    const std::uint32_t base = CommandBufferAddress(*thread_);
    for (std::size_t index = 0; index < written_; ++index) {
        static_cast<void>(memory.Write32(base + (static_cast<std::uint32_t>(index) * WordBytes),
                                         reply_.at(index)));
    }
}

}  // namespace hexlith::domain::kernel
